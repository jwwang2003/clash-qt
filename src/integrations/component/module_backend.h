#pragma once

// ModuleBackend: a core::backend::MihomoBackend whose implementation lives in
// a loaded module.
//
// The whole boundary, in one class. The UI, the Qt bridge and all ~66
// connect() sites are unchanged, because what they code against is
// MihomoBackend and this IS one. Everything that knows about the boundary -
// command codes, packed buffers, owned IBuffers, the reverse seam for
// privileged execution - is contained here, so a boundary bug is one file's
// problem rather than sixty call sites'.
//
// WHY THE CONTRACT'S OWN RULES STILL HOLD ACROSS THE BOUNDARY
//   * Non-re-entrant delivery (backend-r4 section 7). The wrapped backend
//     already queues its callbacks; this class additionally refuses to deliver
//     while it is inside one of its own methods, so an event produced inside a
//     command is still delivered after that command returns.
//   * Order (rule 2). Events are queued in arrival order and drained in that
//     order, so an abort's Superseded completions still reach the consumer
//     ahead of the event announcing the bump, which is what makes the
//     Superseded mark observable at all.
//   * Removal during delivery (rule 3). An observer removed inside a callback
//     receives nothing further, including events already queued.
//   * Nothing unwinds (rule 4). Every path here is noexcept or catches at the
//     edge; no exception crosses the module boundary in either direction.
//
// WHAT IT ADDS FOR TESTS, DELIBERATELY OUTSIDE THE CONTRACT SURFACE
//   lastError(), isValid(), outstandingWork(), drain() and shutdown(). None of
//   them is a failure-injection switch: they report and they tear down, which
//   is what a suite needs to assert the boundary rather than to bend it.

#include <cstdint>
#include <deque>
#include <vector>

#include <QObject>
#include <QString>

#include "core/backend/backend.h"
#include "core/backend/privileged_core_service.h"
#include "core/component/abi/backend_abi.h"
#include "core/component/com_ptr.h"
#include "core/component/object_support.h"
#include "integrations/component/marshal/codec.h"

namespace clashqt::integration {

namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;
namespace cb = ::core::backend;

class ModuleBackend final : public cb::MihomoBackend, private core::PrivilegedCoreServiceListener {
  public:
    /// Takes the session the loader created. `service` is the host-owned
    /// privileged-execution seam; null means this host has none,
    /// and the module is told exactly that rather than being left to guess.
    explicit ModuleBackend(com::ComPtr<abi::IBackendSession> session,
                           core::PrivilegedCoreService *service = nullptr);
    ~ModuleBackend() override;

    ModuleBackend(const ModuleBackend &) = delete;
    ModuleBackend &operator=(const ModuleBackend &) = delete;

    // ------------------------------------------------------- test helpers

    /// A usable session that has a host and has not been closed.
    bool isValid() const noexcept { return valid_; }

    /// The module's diagnostic for the most recent failed command, translated
    /// into the contract's own error vocabulary. code == None when the last
    /// command succeeded.
    cb::ErrorInfo lastError() const { return lastError_; }

    /// Delivers everything the module has produced, pumping the shared event
    /// loop so queued deliveries actually arrive. Returns true when the queue
    /// is empty at the end.
    bool drain(int timeoutMs = 2000);

    /// Requests that the module cancel what it can, optionally stop its
    /// managed core, and release the host. Idempotent. An UNCONFIRMED stop is
    /// still reported through stopCompleted(); this never converts one into a
    /// success.
    bool shutdown(bool stopManagedCore = true, int timeoutMs = 3000);

    /// Requests the module has not settled, plus native work it has running on
    /// its own pool. Zero is what makes unloading safe.
    std::int32_t outstandingWork() const;

    /// Just the native half of the number above: runnables the module submitted
    /// to a pool it owns and has not seen exit. A host that wants to know
    /// whether code in the module's IMAGE is still executing - rather than
    /// whether a request is still open - asks this. -1 when the module is too
    /// old to answer, which is not the same as zero and is not reported as it.
    std::int32_t pendingNativeWork() const;

    /// True while a command of this object is on the stack - the state that
    /// makes a delivery re-entrant, published so a test can assert it never
    /// coincides with a callback.
    bool isInsideCommand() const noexcept { return commandDepth_ > 0; }
    bool isDelivering() const noexcept { return delivering_; }
    /// Events accepted from the module and not yet delivered.
    std::size_t queuedEventCount() const noexcept { return queue_.size(); }
    /// Events this host did not understand. A newer module talking to an older
    /// host is normal; a non-zero count with a matching handshake is a bug.
    std::uint64_t unknownEventCount() const noexcept { return unknownEvents_; }

    // --------------------------------------------------- MihomoBackend
    cb::Generation generation() const noexcept override;
    bool addObserver(cb::BackendObserver *observer) noexcept override;
    bool removeObserver(cb::BackendObserver *observer) noexcept override;

    // ---- BackendLifecycle
    QString discoverBinary() const noexcept override;
    void setBinaryPath(const QString &path) noexcept override;
    QString binaryPath() const noexcept override;
    bool setExecutionMode(cb::ExecutionMode mode) noexcept override;
    cb::ExecutionMode executionMode() const noexcept override;
    bool usesPrivilegedService() const noexcept override;
    cb::RequestId start(const QString &configPath, const QString &workDir) noexcept override;
    cb::RequestId stop() noexcept override;
    cb::CoreState state() const noexcept override;
    cb::Ownership ownership() const noexcept override;
    cb::Endpoint managedEndpoint() const noexcept override;
    QStringList activeConfigPaths() const noexcept override;
    bool isRestartPending() const noexcept override;
    bool isManagedCoreActive() const noexcept override;

    // ---- BackendAttachment
    cb::Endpoint discoverEndpoint() const noexcept override;
    bool endpointFromConfigFile(const QString &path, cb::Endpoint *out) const noexcept override;
    cb::RequestId attach(const cb::Endpoint &endpoint) noexcept override;
    cb::RequestId detach() noexcept override;
    cb::Endpoint currentEndpoint() const noexcept override;
    bool isAttached() const noexcept override;
    bool isConnected() const noexcept override;
    cb::Ownership attachmentOwnership() const noexcept override;
    bool isExternalControllerConnected() const noexcept override;
    cb::RequestId refreshConfig() noexcept override;

    // ---- BackendControl
    cb::RequestId setMode(const QString &mode) noexcept override;
    cb::RequestId setTunEnabled(bool enabled) noexcept override;
    bool isTunChangePending() const noexcept override;
    cb::RequestId selectNode(const QString &group, const QString &node) noexcept override;
    cb::RequestId resetGroupSelection(const QString &group) noexcept override;
    cb::RequestId testGroupDelay(const QString &group) noexcept override;
    cb::RequestId testNodeDelay(const QString &node) noexcept override;
    cb::RequestId closeConnection(const QString &id) noexcept override;
    cb::RequestId closeAllConnections() noexcept override;
    cb::RequestId updateGeoDatabases() noexcept override;
    cb::RequestId queryDns(const QString &name, const QString &type) noexcept override;
    cb::RequestId flushDnsCache(bool fakeIp) noexcept override;

    // ---- BackendTelemetry
    cb::RequestId refreshVersion() noexcept override;
    cb::RequestId refreshProxies() noexcept override;
    cb::RequestId refreshRules() noexcept override;
    cb::RequestId openTrafficStream() noexcept override;
    cb::RequestId closeTrafficStream() noexcept override;
    cb::RequestId openConnectionsStream() noexcept override;
    cb::RequestId closeConnectionsStream() noexcept override;
    cb::RequestId openLogStream(const QString &level) noexcept override;
    cb::RequestId closeLogStream() noexcept override;
    cb::RequestId openMemoryStream() noexcept override;
    cb::RequestId closeMemoryStream() noexcept override;
    cb::RequestId fetchProviders(bool rules) noexcept override;
    cb::RequestId updateProvider(bool rules, const QString &name) noexcept override;
    cb::RequestId healthCheckProvider(const QString &name) noexcept override;
    bool isProviderBusy() const noexcept override;

    // ---- BackendCapabilities
    cb::BackendIdentity identity() const noexcept override;
    cb::FeatureSet features() const noexcept override;
    bool serviceSupported() const noexcept override;
    bool serviceAvailable() const noexcept override;
    cb::BackendTimings timings() const noexcept override;
    cb::RequestId requestPrivilegedServiceStatus() noexcept override;

  private:
    /// The host's side of the boundary. A separate object rather than a base
    /// of ModuleBackend, so the module's strong reference keeps only this
    /// small thing alive and cannot resurrect a backend the application has
    /// already deleted.
    class Host final : public abi::IBackendHost {
      public:
        explicit Host(ModuleBackend *owner) noexcept : owner_(owner) {}

        com::Result QueryInterface(const com::InterfaceId &id, void **out) noexcept override;
        std::int32_t AddRef() noexcept override;
        std::int32_t Release() noexcept override;
        com::Result Notify(std::uint32_t event, const void *data, std::size_t size) noexcept override;
        com::Result Invoke(std::uint32_t command, const void *data, std::size_t size,
                           com::IBuffer **reply) noexcept override;

        /// Called when the backend goes away, so a late callback from the
        /// module reaches a live object that simply refuses.
        void detachOwner() noexcept { owner_ = nullptr; }

      private:
        ~Host() = default;

        com::ReferenceCount references_;
        ModuleBackend *owner_ = nullptr;
    };

    /// Depth guard: while one of these is alive, an event is queued rather
    /// than delivered.
    class CommandScope {
      public:
        explicit CommandScope(const ModuleBackend *backend) noexcept : backend_(backend) {
            ++backend_->commandDepth_;
        }
        ~CommandScope() {
            --backend_->commandDepth_;
            if (backend_->commandDepth_ == 0) {
                backend_->deliverQueued();
            }
        }

      private:
        const ModuleBackend *backend_;
    };

    // ---- command plumbing
    com::Result call(std::uint32_t command, const marshal::ByteWriter &args,
                     std::vector<std::uint8_t> *reply) const;
    cb::RequestId requestCall(std::uint32_t command, const marshal::ByteWriter &args) const;
    bool flagCall(std::uint32_t command, const marshal::ByteWriter &args) const;
    QString textCall(std::uint32_t command, const marshal::ByteWriter &args) const;
    std::uint8_t byteCall(std::uint32_t command, const marshal::ByteWriter &args,
                          std::uint8_t fallback) const;
    void noteFailure(std::uint32_t command, com::Result status) const;

    // ---- events
    void enqueue(std::uint32_t event, const void *data, std::size_t size);
    void deliverQueued() const;
    void dispatchEvent(std::uint32_t event, const std::vector<std::uint8_t> &payload) const;
    /// Runs `callback` for every observer still admitted to the event being
    /// delivered. Membership is re-checked before every call, and so is the
    /// production sequence: both are ways an observer can stop being entitled
    /// to an event that is already on its way.
    template <typename Callback>
    void forEachObserver(Callback callback) const;
    /// The module's current production sequence, asked of the module. Used at
    /// registration time only, which is a human-rate event.
    std::uint64_t producedSequence() const;

    // ---- the privileged reverse seam, host side
    com::Result handleHostCommand(std::uint32_t command, const void *data, std::size_t size,
                                  com::IBuffer **reply);
    void privilegedConnectedChanged(bool connected) override;
    void privilegedStatusReceived(const QJsonObject &status) override;
    void privilegedCoreStarted(const QJsonObject &endpoint) override;
    void privilegedCoreStopped() override;
    void privilegedLogsReceived(const QString &logs) override;
    void privilegedRequestFinished(const QString &operation, bool success,
                                   const QString &error) override;
    void sendToModule(std::uint32_t command, const marshal::ByteWriter &args);

    com::ComPtr<abi::IBackendSession> session_;
    Host *host_ = nullptr;
    core::PrivilegedCoreService *service_ = nullptr;
    bool listenerActive_ = false;
    bool valid_ = false;

    /// Owns the queued-drain posting. A plain QObject, because this class is
    /// not one: MihomoBackend is deliberately not a QObject and the Qt-native
    /// view of it is BackendBridge's job.
    QObject pump_;

    /// One event as it arrived: the module's production sequence from wire.h's
    /// envelope, the event code, and the payload with the envelope stripped.
    struct QueuedEvent {
        std::uint64_t sequence = 0;
        std::uint32_t event = 0;
        std::vector<std::uint8_t> payload;
    };

    mutable std::vector<cb::BackendObserver *> observers_;
    /// What each observer was told the module had already produced when it
    /// registered. An event whose envelope sequence is not GREATER than this
    /// was produced before the observer existed, and backend-r4 says it does
    /// not get it - the same rule the in-process dispatcher applies with its
    /// own counter, applied here to a number that crossed the boundary.
    mutable std::vector<std::pair<cb::BackendObserver *, std::uint64_t>> admittedFrom_;
    mutable std::deque<QueuedEvent> queue_;
    mutable std::uint64_t deliveringSequence_ = 0;
    mutable int commandDepth_ = 0;
    mutable bool delivering_ = false;
    mutable bool drainScheduled_ = false;
    mutable std::uint64_t unknownEvents_ = 0;
    mutable cb::ErrorInfo lastError_;
};

}  // namespace clashqt::integration
