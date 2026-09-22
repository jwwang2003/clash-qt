#pragma once

// The module's implementation of IBackendSession: backend-r4 as commands and
// events, over one Qt event loop shared with the host.
//
// WHAT THIS CLASS IS NOT. It is not a second backend. It owns a
// core::backend::MihomoBackend built by a factory the module supplies - the
// real MihomoBackendImpl in the shipping module, the deterministic fake in the
// test module - and does nothing except translate. Every semantic rule of
// backend-r4 is still enforced by the wrapped backend, which is the point: the
// same contract suite can run against the in-process backend and the
// module-backed one and hold both to the same statements.
//
// NON-RE-ENTRANT DELIVERY SURVIVES BECAUSE IT IS NOT RE-IMPLEMENTED (D8).
// The wrapped backend already queues each observer callback and delivers it
// from the event loop after the mutating call returns. This class forwards on
// that same thread, inside that same delivery, so the guarantee is inherited
// rather than rebuilt. The host shim adds the other half - it defers a Notify
// that somehow arrives inside one of its own commands - and the ABI suite
// asserts both.
//
// LIFETIME ACCOUNTING, NOT A PERMANENT LEAK. OutstandingWork() counts requests
// that have been submitted and not settled, plus buffers the host still holds.
// Close() cancels what can be cancelled, optionally stops a managed core, waits
// for the rest with a deadline, then detaches the observer and destroys the
// wrapped backend BEFORE releasing the host. Only then can the module be
// unloaded, and IModuleLifetime is what the loader asks.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <QSet>
#include <QString>

#include "core/backend/backend.h"
#include "core/backend/observer.h"
#include "core/backend/privileged_core_service.h"
#include "core/component/abi/backend_abi.h"
#include "core/component/object_support.h"
#include "core/mihomo/module/host_privileged_service.h"
#include "integrations/component/marshal/codec.h"
#include "integrations/component/marshal/com_objects.h"

namespace core::module {

namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;
namespace cb = ::core::backend;
namespace marshal = ::clashqt::integration::marshal;

class BackendModule;

/// A backend plus the two numbers backend-r4's observer-admission rule needs
/// and its published facade does not carry.
///
/// WHY THIS IS NOT A MihomoBackend METHOD. backend-r4 admits an observer by
/// PRODUCTION: "an observer added after an event was produced does not receive
/// it". In process that rule needs no API, because the queue that produced the
/// event and the observer list that admits it are the same object. Across the
/// boundary they are not, so the number has to travel - and the place to get it
/// is the concrete dispatcher, not the published interface. Every module
/// factory knows its own concrete type; the facade stays exactly as it is.
///
/// A factory that supplies neither accessor gets the pre-envelope behaviour:
/// sequence 0 on every event, and a host that admits everything.
struct WrappedBackend {
    std::unique_ptr<cb::MihomoBackend> backend;
    /// The sequence of the last event the backend PRODUCED.
    std::function<std::uint64_t()> producedSequence;
    /// The sequence of the event being delivered right now, or 0 outside a
    /// delivery. This is what stamps an observer callback as it goes out.
    std::function<std::uint64_t()> deliverySequence;
};

/// Builds the backend this session wraps. The privileged service is the
/// host-owned reverse seam and may be null when the host injected none.
using BackendFactory = std::function<WrappedBackend(PrivilegedCoreService *)>;

/// Handles a command this session does not know. The shipping module has none;
/// the test module uses it for its control surface, which is why that surface
/// cannot exist in the shipping ABI.
/// The session is passed in, so an extension never needs process-global state
/// to know what it is driving - the defect class component-r1 calls out for
/// error info applies just as much to a test hook.
using CommandExtension =
    std::function<com::Result(class BackendSession &session, std::uint32_t command,
                              const void *args, std::size_t size, com::IBuffer **reply)>;

class BackendSession final : public abi::IBackendSession, private cb::BackendObserver {
  public:
    BackendSession(BackendModule *owner, BackendFactory factory) noexcept;

    BackendSession(const BackendSession &) = delete;
    BackendSession &operator=(const BackendSession &) = delete;

    /// Installed by the module before the session is handed out.
    void setCommandExtension(CommandExtension extension);

    /// The wrapped backend, or null before SetHost. The test module needs it
    /// for its control surface; nothing in the shipping path uses it.
    cb::MihomoBackend *backend() const noexcept { return wrapped_.backend.get(); }

    /// What a module-produced object reports its existence to, so the module
    /// refuses to be unmapped while that object lives. A command extension that
    /// allocates a reply buffer has to pass this: a buffer produced by the
    /// module and counted by nobody is exactly the hole the accounting exists
    /// to close, and a test double is no more exempt from it than the shipping
    /// module is.
    marshal::ObjectAnchor *anchor() const noexcept;

    /// This session's observer face - the thing that turns a backend callback
    /// into an event on the wire. A test module's command extension injects
    /// telemetry the deterministic fake cannot produce on its own (a populated
    /// connections snapshot, a log entry) by calling it directly.
    ///
    /// Reachable only from C++ inside the module: it is not a command, so the
    /// shipping module - which installs no extension - exposes nothing through
    /// it. That is deliberate. module-r1 forbids failure-injection switches in
    /// the shipping ABI, and a method no exported code path reaches is not one.
    cb::BackendObserver &sink() noexcept { return *this; }

    // ---- clashqt::com::IObject
    com::Result QueryInterface(const com::InterfaceId &id, void **out) noexcept override;
    std::int32_t AddRef() noexcept override;
    std::int32_t Release() noexcept override;

    // ---- clashqt::com::abi::IBackendSession
    com::Result SetHost(abi::IBackendHost *host) noexcept override;
    com::Result Invoke(std::uint32_t command, const void *args, std::size_t size,
                       com::IBuffer **reply) noexcept override;
    com::Result LastError(com::IErrorInfo **out) noexcept override;
    com::Result Close(std::uint32_t flags, std::uint32_t timeoutMs) noexcept override;
    std::int32_t OutstandingWork() noexcept override;

  private:
    ~BackendSession() override;

    /// The bodies of the four entry points above. Each public method is a
    /// try/catch around its Impl, because every one of them allocates - a
    /// QString, a reply buffer, a std::function copy - and an exception that
    /// escapes a COM method unwinds into a caller that may have been compiled
    /// by a different C++ runtime. That is undefined behaviour, not a failure
    /// path, so allocation failure is turned into kFail and a null output here.
    com::Result setHostImpl(abi::IBackendHost *host);
    com::Result invokeImpl(std::uint32_t command, const void *args, std::size_t size,
                           com::IBuffer **reply);
    com::Result lastErrorImpl(com::IErrorInfo **out);
    com::Result closeImpl(std::uint32_t flags, std::uint32_t timeoutMs);

    // ---- command handling
    com::Result dispatch(std::uint32_t command, marshal::ByteReader &in, marshal::ByteWriter &out);
    com::Result dispatchPrivileged(std::uint32_t command, marshal::ByteReader &in);
    void setError(com::Result code, const QString &message, const QString &source);
    void clearError();

    /// Records a request that backend-r4 documents as settling with an observer
    /// callback, so OutstandingWork() means something. Streams and
    /// closeConnection are deliberately not tracked: they settle with no event,
    /// and counting them would make the number monotonically wrong.
    cb::RequestId track(cb::RequestId id);
    void settle(cb::RequestId id);

    /// Both write wire.h's u64 production envelope in front of the payload.
    /// The number is the sequence the wrapped backend assigned when it produced
    /// the event, read back from the delivery that is running right now - not a
    /// counter of this class's own, which would count arrivals and defeat the
    /// point.
    void emitEvent(std::uint32_t event, const marshal::ByteWriter &payload) noexcept;
    void emitEventBytes(std::uint32_t event, const void *data, std::size_t size) noexcept;
    std::uint64_t currentProductionSequence() const noexcept;

    // ---- core::backend::BackendObserver
    void coreStateChanged(cb::Generation generation, cb::CoreState state,
                          cb::Ownership ownership) noexcept override;
    void coreReady(const cb::Completion &completion, const cb::Endpoint &endpoint) noexcept override;
    void coreLogLine(cb::Generation generation, const QString &line) noexcept override;
    void coreFailed(const cb::Completion &completion) noexcept override;
    void coreStopped(cb::Generation generation) noexcept override;
    void stopCompleted(const cb::StopCompleted &result) noexcept override;
    void endpointChanged(cb::Generation generation, const cb::Endpoint &endpoint,
                         cb::Ownership ownership) noexcept override;
    void connectedChanged(cb::Generation generation, bool connected) noexcept override;
    void configReceived(const cb::Completion &completion,
                        const cb::BaseConfig &config) noexcept override;
    void modeChanged(const cb::Completion &completion, const QString &mode) noexcept override;
    void tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept override;
    void nodeSelected(const cb::Completion &completion, const QString &group,
                      const QString &node) noexcept override;
    void geoDatabasesUpdated(const cb::Completion &completion) noexcept override;
    void dnsQueryFinished(const cb::Completion &completion, const QString &name,
                          const QString &resultJson) noexcept override;
    void dnsCacheFlushed(const cb::Completion &completion, bool fakeIp) noexcept override;
    void versionReceived(const cb::Completion &completion, const QString &version) noexcept override;
    void proxiesUpdated(const cb::Completion &completion, cb::Span<cb::ProxyGroup> groups,
                        cb::Span<cb::ProxyNode> nodes) noexcept override;
    void rulesUpdated(const cb::Completion &completion, cb::Span<cb::Rule> rules) noexcept override;
    void trafficSample(cb::Generation generation, quint64 up, quint64 down) noexcept override;
    void memorySample(cb::Generation generation, quint64 inuse, quint64 oslimit) noexcept override;
    void connectionsUpdated(cb::Generation generation, cb::Span<cb::Connection> connections,
                            quint64 uploadTotal, quint64 downloadTotal) noexcept override;
    void logReceived(cb::Generation generation, const cb::LogEntry &entry) noexcept override;
    void providersReceived(const cb::Completion &completion, bool rules,
                           cb::Span<cb::Provider> providers) noexcept override;
    void providerBusyChanged(cb::Generation generation, bool busy) noexcept override;
    void providerOperationFinished(const cb::Completion &completion,
                                   const QString &message) noexcept override;
    void privilegedServiceStatus(const cb::Completion &completion,
                                 const cb::PrivilegedServiceStatus &status) noexcept override;
    void errorOccurred(const cb::Completion &completion) noexcept override;

    com::ReferenceCount references_;
    BackendModule *owner_ = nullptr;
    BackendFactory factory_;
    CommandExtension extension_;

    abi::IBackendHost *host_ = nullptr;
    std::unique_ptr<HostPrivilegedService> privileged_;
    WrappedBackend wrapped_;

    /// One reusable buffer for the envelope plus payload, so stamping an event
    /// costs a copy rather than an allocation. It matters for exactly one
    /// event: a connections snapshot is the packed buffer D8 asked for, it
    /// arrives at the engine's emission rate, and allocating a fresh envelope
    /// for each one would put the allocation back that the packing removed.
    std::vector<std::uint8_t> envelope_;

    QSet<quint64> inFlight_;
    bool closed_ = false;
    bool managedCoreActive_ = false;
    cb::RequestId closingStop_ = cb::RequestId::Invalid;
    bool closingStopSettled_ = false;

    com::Result lastErrorCode_ = com::kOk;
    QString lastErrorMessage_;
    QString lastErrorSource_;
};

}  // namespace core::module
