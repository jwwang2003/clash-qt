#pragma once

// The real MihomoBackend: an ADAPTER over the existing CoreProcess,
// MihomoClient and ProviderClient, not a rewrite of them.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r3.
//
// WHAT THIS CLASS ADDS OVER THE THREE OBJECTS IT WRAPS
//
//   * Request identity. The three collaborators are fire-and-forget plus a bare
//     signal. Each of them now returns the identity of the operation it
//     submitted and settles it exactly once, and this class maps those onto the
//     published RequestId and the Generation the work was SUBMITTED under.
//
//   * One generation in place of four counters and a pending-key set:
//       MihomoClient::endpointEpoch_  ->  Generation (endpoint change)
//       MihomoClient::requestEpoch_   ->  Generation (disconnect)
//       CoreProcess::launchGeneration_->  Generation (managed start/stop/fail)
//       MihomoClient::tunChangeId_    ->  a RequestId. It is incremented per
//                                         OPERATION, so it was never a
//                                         generation (backend-r2, answer 2).
//       ProviderClient::pending_      ->  coalescing keyed by (Generation,
//                                         operation identity); a duplicate
//                                         submission returns the outstanding
//                                         request's id (backend-r2, answer 1).
//
//     Folding launchGeneration_ into the global counter means a managed
//     start/stop/fail now invalidates in-flight CONTROLLER replies, which it did
//     not before. fetchVersion/fetchProxies recover through the application's
//     5 s poll; fetchRules and fetchConfigs have NO recovery poll. So the
//     obligation r2 attaches to the single counter is honoured here: every
//     generation bump that is not an endpoint change re-issues the snapshot set
//     (see scheduleSnapshotReissue). An endpoint change needs no help, because
//     MihomoClient::setEndpoint already ends in refreshState().
//
//   * Non-re-entrant delivery. Every observer callback is queued and delivered
//     after the mutating call returns, on the owning thread. MihomoClient emits
//     five signals synchronously from inside setEndpoint/setConnected; this
//     class is what stops that reaching a consumer mid-mutation.
//
//   * Terminal-outcome stamping that survives its own teardown (backend-r3 B1).
//     A stop's StopCompleted is stamped when it is PRODUCED, not when it was
//     submitted, because CoreProcess emits failed() before stopFinished() on the
//     unconfirmed path and this class bumps on failed(). See the stopFinished
//     handler; it is the one place where A1's "post-bump" clause is load-bearing
//     rather than a restatement of the submit-time stamp.

#include <cstdint>
#include <functional>
#include <vector>

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include "core/backend/backend.h"
#include "core/mihomo/mihomo_client.h"
#include "core/mihomo/process/core_process.h"
#include "core/mihomo/provider_client.h"

namespace core {

namespace cb = backend;

class MihomoBackendImpl final : public cb::MihomoBackend {
  public:
    /// `service` is the privileged-execution seam (DECISION D2); null means this
    /// backend has no privileged service and reports none. No QObject *parent,
    /// and no collaborator is implicitly constructed for a null argument
    /// (contract section 9): the owner creates this and deletes it.
    explicit MihomoBackendImpl(PrivilegedCoreService *service = nullptr);
    ~MihomoBackendImpl() override;

    MihomoBackendImpl(const MihomoBackendImpl &) = delete;
    MihomoBackendImpl &operator=(const MihomoBackendImpl &) = delete;

    /// Delivers everything already produced. Nothing in the published contract
    /// needs it; it exists so a test can pump the owning thread's queue without
    /// guessing at a sleep.
    bool drainPendingEvents(int timeoutMs = 2000);
    bool hasPendingEvents() const noexcept { return !queue_.empty() || drainScheduled_; }
    /// True while a mutating call of this backend is on the stack. An observer
    /// invoked while this is true was delivered re-entrantly.
    bool isInsideMutatingCall() const noexcept { return mutatingDepth_ > 0; }

    /// The sequence of the last event this backend PRODUCED, and the sequence
    /// of the one it is delivering right now (0 outside a delivery).
    ///
    /// These are the numbers addObserver() already uses to admit an observer by
    /// production rather than by arrival - "added during this delivery: it sees
    /// only what came after it". They are published so a MODULE can apply the
    /// same rule on the far side of a boundary, where the producing queue and
    /// the observer list are no longer the same object. Nothing in the
    /// published facade changes: this is the concrete dispatcher's own surface,
    /// which is where a module factory already is.
    std::uint64_t producedSequence() const noexcept { return sequence_; }
    std::uint64_t deliverySequence() const noexcept { return deliveringSequence_; }

    /// Runnables this backend has submitted to a pool it OWNS and not yet seen
    /// exit. Published for the same reason the two sequences above are: across
    /// a module boundary the consumer has to know whether code in the module's
    /// image is still executing before it unmaps that image, and no published
    /// backend-r4 operation can tell it. Nothing in the facade changes.
    int pendingNativeWork() const noexcept { return process_.pendingNativeWork(); }
    /// Readiness-probe completions delivered after their probe was cancelled.
    /// Contract section 5.2 requires the probe to disconnect before aborting, so
    /// this must always be 0; published here because the rule is otherwise
    /// unobservable through the facade (backend-r3, weak-coverage item 3).
    int probeCompletionsAfterCancel() const noexcept {
        return process_.probeCompletionsAfterCancel();
    }

    /// The readiness and termination budget this backend will REALLY apply.
    /// timings() publishes whatever is set here, so a consumer sizing its own
    /// progress reporting is never told one thing and given another. It is not
    /// part of the published facade: a host sets it, a module consumer does not.
    void setTimings(const CoreTimings &timings);

    // =============================================================== facade
    cb::Generation generation() const noexcept override { return generation_; }
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
    // What a completion is a completion OF. It is how a failed or superseded
    // operation still reports on the right channel with an empty payload.
    enum class Kind : std::uint8_t {
        Version, Proxies, Rules, Config, Mode, SelectNode, ResetGroup,
        TestGroupDelay, TestNodeDelay, CloseConnection, CloseAllConnections,
        UpdateGeo, QueryDns, FlushDns,
        FetchProviders, UpdateProvider, HealthCheckProvider,
        Unknown,
    };

    struct Pending {
        cb::RequestId id = cb::RequestId::Invalid;
        cb::Generation generation = cb::Generation::Initial;
        Kind kind = Kind::Unknown;
        QString first;
        QString second;
        bool flag = false;
    };

    // Payload captured between a payload signal and the settle that follows it
    // in the same reply handler. Qt emits synchronously on one thread, so a
    // handler runs to completion and the pairing cannot interleave.
    struct Payload {
        bool has = false;
        Kind kind = Kind::Unknown;
        QString version;
        QString mode;
        QString group;
        QString node;
        QString dnsName;
        QString dnsJson;
        QString message;
        bool fakeIp = false;
        bool providerRules = false;
        cb::BaseConfig config;
        QVector<cb::ProxyGroup> groups;
        QVector<cb::ProxyNode> nodes;
        QVector<cb::Rule> ruleList;
        QVector<cb::Provider> providers;
        void clear() { *this = Payload{}; }
    };

    struct Event {
        std::uint64_t sequence = 0;
        std::function<void(cb::BackendObserver &)> deliver;
    };

    class MutationScope {
      public:
        explicit MutationScope(MihomoBackendImpl *self) noexcept;
        ~MutationScope();
        MutationScope(const MutationScope &) = delete;
        MutationScope &operator=(const MutationScope &) = delete;

      private:
        MihomoBackendImpl *self_;
    };

    void connectCollaborators();

    // delivery
    void enqueue(std::function<void(cb::BackendObserver &)> deliver);
    void scheduleDrain();
    void drain();

    // identity
    cb::RequestId nextRequest() noexcept;
    void bumpGeneration() noexcept;
    /// The other half of every bump this class makes ITSELF - a managed start,
    /// a managed failure, a stop. Those invalidate outstanding work by the
    /// contract's own definition (section 2), but they are not client events,
    /// so nothing used to retire the work: a reply or a provider operation the
    /// collaborators still owed settled Ok, carrying the retired session's
    /// payload, under a generation that had already moved.
    ///
    /// It calls MihomoClient::retireSession(), which does NOT emit
    /// invalidating(): that signal is what makes this class bump, and
    /// re-entering the bump from inside the retirement it caused is the
    /// recursive path this seam exists to avoid. The bump has already happened
    /// when this runs, so every completion the retirement produces is stamped
    /// against the new generation and marked Superseded.
    void retireTransport(const QString &reason);
    /// Contract obligation that comes with ONE global generation: a bump that is
    /// not an endpoint change re-issues the snapshot set, because fetchRules and
    /// fetchConfigs have no recovery poll.
    void scheduleSnapshotReissue();

    cb::RequestId submitRest(Kind kind, quint64 operation, const QString &first = {},
                             const QString &second = {}, bool flag = false);
    void capture(Kind kind, const std::function<void(Payload &)> &fill);
    void settleRest(quint64 operation, bool superseded, const QString &error);
    /// `requested` is what the collaborator settled; the status actually
    /// published is downgraded to Superseded when the request's own generation
    /// has been left behind, so no path can publish retired work as Ok.
    void publish(const Pending &request, cb::CompletionStatus requested,
                 const cb::ErrorInfo &error, const Payload &payload);
    void publishProviderBusy();

    cb::Ownership ownershipOf(const Endpoint &endpoint) const;

    // --- collaborators. Declared in construction order: ProviderClient takes
    //     the client, and both outlive nothing but this object.
    QObject pump_;
    MihomoClient client_;
    ProviderClient providers_;
    CoreProcess process_;

    // --- observers and delivery
    std::vector<cb::BackendObserver *> observers_;
    QHash<cb::BackendObserver *, std::uint64_t> observerAddedAt_;
    std::vector<Event> queue_;
    std::uint64_t sequence_ = 0;
    std::uint64_t deliveringSequence_ = 0;
    int mutatingDepth_ = 0;
    bool delivering_ = false;
    bool drainScheduled_ = false;
    bool snapshotReissueScheduled_ = false;

    // --- identity
    cb::Generation generation_ = cb::Generation::Initial;
    std::uint64_t nextRequest_ = 0;
    QHash<quint64, Pending> restRequests_;
    QHash<QString, Pending> providerRequests_;
    Payload payload_;

    // --- the operations that own a terminal event of their own
    cb::RequestId startRequest_ = cb::RequestId::Invalid;
    cb::Generation startGeneration_ = cb::Generation::Initial;
    cb::RequestId stopRequest_ = cb::RequestId::Invalid;
    cb::Generation stopGeneration_ = cb::Generation::Initial;
    cb::RequestId tunRequest_ = cb::RequestId::Invalid;
    cb::Generation tunGeneration_ = cb::Generation::Initial;
    /// Set when MihomoClient::invalidating fires while a TUN change is
    /// outstanding. MihomoClient cancels the pending change on exactly those
    /// three paths (setEndpoint, detach, setConnected(false)), so this - not the
    /// text of the error message, which is translated - is how the adapter knows
    /// a TUN completion is a SUPERSESSION rather than a protocol error
    /// (backend-r3 B2).
    bool tunSuperseded_ = false;
    cb::RequestId serviceStatusRequest_ = cb::RequestId::Invalid;
    cb::Generation serviceStatusGeneration_ = cb::Generation::Initial;

    bool attachingEndpoint_ = false;
    bool providerBusy_ = false;
};

}  // namespace core
