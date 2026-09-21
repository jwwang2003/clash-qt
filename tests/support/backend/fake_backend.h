// An in-process, deterministic implementation of core::backend::MihomoBackend.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r1.
//
// WHAT MAKES IT DETERMINISTIC
//   * No sleeps and no wall clock. Time is a counter the test advances, and
//     advancing it is what drives readiness probes and termination escalation.
//   * No real sockets, processes or files.
//   * Every asynchronous step is an explicit GATE: hold the operation, observe
//     the pending state, release it, then await the completion with a deadline.
//     A deadline never decides an outcome; it only bounds a failure.
//
// It can produce every outcome the contract names, including the three that are
// easy to leave untestable: a validation failure that leaves the running
// configuration intact, an unconfirmed stop, and a superseded generation.
#ifndef CLASHQT_TESTS_SUPPORT_BACKEND_FAKE_BACKEND_H
#define CLASHQT_TESTS_SUPPORT_BACKEND_FAKE_BACKEND_H

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "core/backend/backend.h"

namespace testsupport::backend {

namespace cb = core::backend;

// Whether an operation completes by itself or waits for the test to release it.
enum class Gate : std::uint8_t { Immediate, Held };

// The one hazard the fake exposes as a knob rather than as a source edit,
// because backend-r1 section 2 requires the contract test to tell the two
// orders apart.
enum class AbortOrdering : std::uint8_t {
    // The contract: bump the generation, THEN abort in-flight work, so a
    // completion the abort delivers synchronously is already stale.
    BumpThenAbort,
    // The inversion: abort first. The completion is produced while its
    // generation still looks current, so it is delivered as live data stamped
    // with the very generation it was meant to invalidate.
    AbortThenBump,
};

// What GET /version answers. Readiness requires 200 AND a JSON object whose
// `version` field is a string - nothing less.
enum class VersionResponse : std::uint8_t {
    Ok,               // 200, {"version":"1.19.0"}
    Unreachable,      // no answer at all
    ServerError,      // 500 with a perfectly good body
    NotFound,         // 404
    VersionMissing,   // 200, {} - no version field
    VersionNotString, // 200, {"version":1} - the field is a number
    NotAnObject,      // 200, "1.19.0" - valid JSON, wrong shape
};

enum class RequestOutcome : std::uint8_t { Success, Failure, Cancel };

// Every operation the fake can have outstanding. Published so a test can assert
// on what is pending rather than on a count.
enum class RequestKind : std::uint8_t {
    Attach, Detach, RefreshConfig,
    RefreshVersion, RefreshProxies, RefreshRules,
    SetMode, SetTun, SelectNode, ResetGroup, TestGroupDelay, TestNodeDelay,
    CloseConnection, CloseAllConnections, UpdateGeo, QueryDns, FlushDns,
    OpenStream, CloseStream,
    FetchProviders, UpdateProvider, HealthCheckProvider,
    ServiceStatus,
};

// The payloads a released request delivers. Defaults are valid, so a test that
// does not care about the data does not have to set any of it.
struct StagedData {
    QString version = QStringLiteral("1.19.0");
    QString mode = QStringLiteral("rule");
    bool tunActual = false;  // the READ-BACK, deliberately settable apart from the request
    QString dnsResultJson = QStringLiteral("{\"Status\":0}");
    cb::BaseConfig config;
    QVector<cb::ProxyGroup> groups;
    QVector<cb::ProxyNode> nodes;
    QVector<cb::Rule> rules;
    QVector<cb::Provider> providers;
};

class FakeBackend final : public cb::MihomoBackend {
  public:
    FakeBackend();
    ~FakeBackend() override;

    FakeBackend(const FakeBackend &) = delete;
    FakeBackend &operator=(const FakeBackend &) = delete;

    // ================================================================ driving

    // ---- virtual time. Advancing drives readiness probing at the contract's
    //      probe interval and the terminate->kill escalation. Nothing else in
    //      the fake looks at a clock.
    void advanceTime(qint64 ms);
    qint64 elapsedMs() const noexcept { return nowMs_; }

    // ---- delivery. Events are queued and delivered on the owning thread after
    //      the mutating call returns; these pump that queue.
    // Runs the event loop until every queued event has been delivered.
    bool flushEvents(int timeoutMs = 2000);
    // Runs the event loop until `predicate` holds. The deadline bounds a
    // failure; it never produces one.
    static bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 2000);
    // True while a mutating call of this backend is on the stack. An observer
    // that sees this true was invoked re-entrantly, which the contract forbids.
    bool isInsideMutatingCall() const noexcept { return mutatingDepth_ > 0; }
    bool isDelivering() const noexcept { return delivering_; }

    // ---- the request gate (attachment, control, telemetry, capabilities)
    void setRequestGate(Gate gate) noexcept { requestGate_ = gate; }
    std::vector<cb::RequestId> pendingRequests() const;
    bool isPending(cb::RequestId id) const;
    std::optional<RequestKind> pendingKind(cb::RequestId id) const;
    cb::Generation submittedGeneration(cb::RequestId id) const;
    bool releaseRequest(cb::RequestId id, RequestOutcome outcome = RequestOutcome::Success,
                        const cb::ErrorInfo &error = {});
    void releaseAllRequests(RequestOutcome outcome = RequestOutcome::Success);
    StagedData &staged() noexcept { return staged_; }

    // ---- the managed-core gates
    void setValidationGate(Gate gate) noexcept { validationGate_ = gate; }
    bool isValidating() const noexcept { return validation_.has_value(); }
    QString validatingConfigPath() const;
    // The validation child exits. `valid == false` must leave the running
    // configuration intact.
    bool completeValidation(bool valid, const QString &reason = QStringLiteral("bad config"));
    // Config paths of validation children that were cancelled and have not
    // exited yet. They stay in activeConfigPaths() until they do.
    QStringList cancellingValidationPaths() const;
    bool completeCancelledValidation(const QString &configPath);

    void setChildExitGate(Gate gate) noexcept { childExitGate_ = gate; }
    bool isChildRunning() const noexcept { return childRunning_; }
    bool isChildTerminating() const noexcept { return retiring_; }
    bool wasChildKilled() const noexcept { return childKilled_; }
    // The retiring child's exit is observed. This is what consumes a pending
    // launch - nothing else may.
    bool releaseChildExit(int exitCode = 0);
    // A running child dies on its own.
    bool crashChild(int exitCode, const QString &lastLine = QStringLiteral("panic"));

    // ---- readiness gates
    // Which endpoint a config file parses to. Unset paths parse to
    // 127.0.0.1:9090 unless setConfigFileUnparsable() names them.
    void mapConfigFile(const QString &configPath, const cb::Endpoint &endpoint);
    void setConfigFileUnparsable(const QString &configPath);
    void setVersionResponse(VersionResponse response) noexcept { versionResponse_ = response; }
    // Holds the answer to the next probe, so a test can cancel a probe that is
    // in flight and prove the cancelled probe delivers nothing.
    void setProbeGate(Gate gate) noexcept { probeGate_ = gate; }
    bool isProbeInFlight() const noexcept { return probeInFlight_; }
    // Returns false when there was no probe to release, including the case
    // that matters: a probe cancelled while its answer was held.
    bool releaseProbeAnswer();
    // A log line from the managed core. Refreshes the idle deadline while
    // Starting; it must not move the hard cap.
    void emitCoreLogLine(const QString &line);

    // ---- controller registry. A controller is "running" independently of
    //      whether we are attached to it; that is what makes "detach must never
    //      terminate an attached controller" assertable.
    void addExternalController(const cb::Endpoint &endpoint);
    bool isControllerRunning(const cb::Endpoint &endpoint) const;
    void setControllerReachable(bool reachable) noexcept { controllerReachable_ = reachable; }
    // The controller drops us: an invalidating event, like an endpoint change.
    void disconnectController();

    // ---- privileged service
    void setServiceSupported(bool supported) noexcept { serviceSupported_ = supported; }
    void setServiceAvailable(bool available) noexcept { serviceAvailable_ = available; }
    void setServiceStatus(const cb::PrivilegedServiceStatus &status) { serviceStatus_ = status; }
    // The service disconnects. During a stop this is what makes the stop
    // UNCONFIRMED: cleanup was requested, nothing confirmed the child exited.
    bool disconnectPrivilegedService(
        const QString &reason =
            QStringLiteral("The privileged service disconnected before confirming core shutdown."));

    // ---- the ordering knob
    void setAbortOrdering(AbortOrdering ordering) noexcept { abortOrdering_ = ordering; }
    AbortOrdering abortOrdering() const noexcept { return abortOrdering_; }

    // ======================================================= MihomoBackend

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
    struct InFlight {
        cb::RequestId id = cb::RequestId::Invalid;
        cb::Generation generation = cb::Generation::Initial;
        RequestKind kind = RequestKind::RefreshVersion;
        QString first;
        QString second;
        bool flag = false;
        QString coalesceKey;  // non-empty for the deduplicated provider operations
    };
    struct Launch {
        QString configPath;
        QString workDir;
        cb::RequestId request = cb::RequestId::Invalid;
        cb::Generation generation = cb::Generation::Initial;
    };
    struct Event {
        std::uint64_t sequence = 0;
        std::function<void(cb::BackendObserver &)> deliver;
    };

    class MutationScope {
      public:
        explicit MutationScope(FakeBackend *self) noexcept;
        ~MutationScope();
        MutationScope(const MutationScope &) = delete;
        MutationScope &operator=(const MutationScope &) = delete;

      private:
        FakeBackend *self_;
    };

    // delivery
    void enqueue(std::function<void(cb::BackendObserver &)> deliver);
    void scheduleDrain();
    void drain();

    // identity
    cb::RequestId nextRequest() noexcept;
    void bumpGeneration() noexcept;

    // requests
    cb::RequestId submit(RequestKind kind, const QString &first = {}, const QString &second = {},
                         bool flag = false, const QString &coalesceKey = {});
    void completeRequest(const InFlight &request, RequestOutcome outcome,
                         const cb::ErrorInfo &error);
    void deliverCompletion(const InFlight &request, const cb::Completion &completion);
    void abortInFlight(const cb::ErrorInfo &reason);
    // Contract section 2 and 5.3: bump, THEN abort. The knob inverts exactly
    // this and nothing else.
    void invalidate(const cb::ErrorInfo &reason);
    void publishProviderBusy();

    // attachment
    void setConnected(bool connected);
    void clearLiveState();
    void refreshState();
    cb::Ownership ownershipOf(const cb::Endpoint &endpoint) const;

    // managed core
    void setState(cb::CoreState state);
    void failManaged(const cb::ErrorInfo &reason, cb::RequestId request);
    void cancelValidation();
    void launchValidated(const Launch &launch);
    void launchProcess(const Launch &launch);
    void terminateProcess();
    void finishStop();
    void awaitController();
    void cancelProbe();
    void probeTick();
    void deliverProbeAnswer();

    // --- observers and delivery
    QObject pump_;
    std::vector<cb::BackendObserver *> observers_;
    QHash<cb::BackendObserver *, std::uint64_t> observerAddedAt_;
    std::vector<Event> queue_;
    std::uint64_t sequence_ = 0;
    int mutatingDepth_ = 0;
    bool delivering_ = false;
    bool drainScheduled_ = false;

    // --- identity
    cb::Generation generation_ = cb::Generation::Initial;
    std::uint64_t nextRequest_ = 0;

    // --- gates and knobs
    Gate requestGate_ = Gate::Immediate;
    Gate validationGate_ = Gate::Held;
    Gate childExitGate_ = Gate::Held;
    Gate probeGate_ = Gate::Immediate;
    AbortOrdering abortOrdering_ = AbortOrdering::BumpThenAbort;
    VersionResponse versionResponse_ = VersionResponse::Ok;
    StagedData staged_;

    // --- clock
    qint64 nowMs_ = 0;

    // --- attachment
    cb::Endpoint attached_;
    bool isAttached_ = false;
    bool connected_ = false;
    bool controllerReachable_ = true;
    std::vector<InFlight> inFlight_;
    bool tunPending_ = false;
    bool geoPending_ = false;

    // --- managed core
    cb::CoreState state_ = cb::CoreState::Stopped;
    cb::ExecutionMode executionMode_ = cb::ExecutionMode::Managed;
    QString binaryPath_ = QStringLiteral("/fake/mihomo");
    QString discoverableBinary_ = QStringLiteral("/fake/mihomo");
    QString configPath_;
    QString workDir_;
    cb::Endpoint managedEndpoint_;
    cb::Endpoint pendingEndpoint_;
    std::optional<Launch> pendingLaunch_;
    std::optional<Launch> validation_;
    QStringList cancellingValidations_;
    bool serviceParsing_ = false;
    bool childRunning_ = false;
    bool retiring_ = false;
    bool childKilled_ = false;
    qint64 terminateDeadlineMs_ = 0;
    bool serviceActive_ = false;
    bool serviceStopping_ = false;
    bool explicitStop_ = false;
    cb::RequestId stopRequest_ = cb::RequestId::Invalid;
    cb::CoreState stopResult_ = cb::CoreState::Stopped;
    cb::ErrorInfo stopError_;
    cb::RequestId startRequest_ = cb::RequestId::Invalid;
    cb::Generation startGeneration_ = cb::Generation::Initial;
    cb::RequestId failRequest_ = cb::RequestId::Invalid;

    // --- readiness
    bool probing_ = false;
    bool probeInFlight_ = false;
    qint64 readyDeadlineMs_ = 0;
    qint64 hardDeadlineMs_ = 0;

    // --- controllers and service
    struct RunningController {
        cb::Endpoint endpoint;
        bool managed = false;
    };
    QVector<RunningController> controllers_;
    QHash<QString, cb::Endpoint> configEndpoints_;
    QStringList unparsableConfigs_;
    bool serviceSupported_ = true;
    bool serviceAvailable_ = true;
    cb::PrivilegedServiceStatus serviceStatus_;
};

}  // namespace testsupport::backend

#endif  // CLASHQT_TESTS_SUPPORT_BACKEND_FAKE_BACKEND_H
