#include "support/backend/fake_backend.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QMetaObject>

namespace testsupport::backend {
namespace {

constexpr cb::BackendTimings kTimings{};

cb::Completion completionFor(cb::RequestId id, cb::Generation generation,
                             cb::CompletionStatus status, const cb::ErrorInfo &error) {
    cb::Completion completion;
    completion.request = id;
    completion.generation = generation;
    completion.status = status;
    completion.error = error;
    return completion;
}

bool isProviderKind(RequestKind kind) {
    return kind == RequestKind::FetchProviders || kind == RequestKind::UpdateProvider ||
           kind == RequestKind::HealthCheckProvider;
}

// Everything but the privileged-service query is answered by the controller over
// HTTP or a websocket. A controller that is not there answers none of it.
bool isControllerBound(RequestKind kind) { return kind != RequestKind::ServiceStatus; }

}  // namespace

// ---------------------------------------------------------------- MutationScope

FakeBackend::MutationScope::MutationScope(FakeBackend *self) noexcept : self_(self) {
    ++self_->mutatingDepth_;
}

FakeBackend::MutationScope::~MutationScope() {
    if (--self_->mutatingDepth_ == 0 && !self_->queue_.empty()) self_->scheduleDrain();
}

// ---------------------------------------------------------------- construction

FakeBackend::FakeBackend() {
    staged_.config.mode = QStringLiteral("rule");
    serviceStatus_.state = cb::ServiceState::Installed;
}

FakeBackend::~FakeBackend() = default;

// ------------------------------------------------------------------- delivery

void FakeBackend::enqueue(std::function<void(cb::BackendObserver &)> deliver) {
    queue_.push_back(Event{++sequence_, std::move(deliver)});
    // Never delivered from here: a mutating call in progress drains on its way
    // out, and anything produced outside one is drained by the event loop.
    if (mutatingDepth_ == 0) scheduleDrain();
}

void FakeBackend::scheduleDrain() {
    if (drainScheduled_) return;
    drainScheduled_ = true;
    QMetaObject::invokeMethod(&pump_, [this] { drain(); }, Qt::QueuedConnection);
}

void FakeBackend::drain() {
    drainScheduled_ = false;
    if (delivering_) return;
    delivering_ = true;
    std::vector<Event> batch;
    batch.swap(queue_);
    for (const Event &event : batch) {
        // A copy: an observer may add or remove observers from inside a callback.
        const std::vector<cb::BackendObserver *> snapshot = observers_;
        for (cb::BackendObserver *observer : snapshot) {
            // Removed during this delivery: it gets nothing more, including
            // events already produced and still queued.
            if (std::find(observers_.begin(), observers_.end(), observer) == observers_.end())
                continue;
            // Added during this delivery: it sees only what came after it.
            const auto added = observerAddedAt_.constFind(observer);
            if (added == observerAddedAt_.constEnd() || event.sequence < *added) continue;
            event.deliver(*observer);
        }
    }
    delivering_ = false;
    if (!queue_.empty()) scheduleDrain();
}

bool FakeBackend::flushEvents(int timeoutMs) {
    // The snapshot re-issue is queued work of the backend's own, so a flush that
    // ignored it would leave the obligation half-performed and make the suite
    // order-dependent.
    return waitFor(
        [this] { return queue_.empty() && !drainScheduled_ && !snapshotReissueScheduled_; },
        timeoutMs);
}

bool FakeBackend::waitFor(const std::function<bool()> &predicate, int timeoutMs) {
    QDeadlineTimer deadline(timeoutMs);
    while (!predicate()) {
        if (deadline.hasExpired()) return predicate();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

bool FakeBackend::addObserver(cb::BackendObserver *observer) noexcept {
    if (!observer) return false;
    if (std::find(observers_.begin(), observers_.end(), observer) != observers_.end()) return false;
    observers_.push_back(observer);
    observerAddedAt_.insert(observer, sequence_ + 1);
    return true;
}

bool FakeBackend::removeObserver(cb::BackendObserver *observer) noexcept {
    const auto it = std::find(observers_.begin(), observers_.end(), observer);
    if (it == observers_.end()) return false;
    observers_.erase(it);
    observerAddedAt_.remove(observer);
    return true;
}

// ------------------------------------------------------------------- identity

cb::RequestId FakeBackend::nextRequest() noexcept {
    return static_cast<cb::RequestId>(++nextRequest_);
}

void FakeBackend::bumpGeneration() noexcept {
    generation_ = static_cast<cb::Generation>(cb::number(generation_) + 1);
}

// backend-r2's answer to the second open question, restated by r3 B3. One global
// generation is enough ONLY if every bump that is not an endpoint change
// re-issues the snapshot set: fetchVersion and fetchProxies recover via the 5 s
// poll, but fetchRules and fetchConfigs are issued only from refreshState, so a
// discarded /rules or /configs reply would leave the rules list and BaseConfig
// stale until the next endpoint change.
//
// Queued rather than immediate, for the same reason events are: the obligation
// is discharged after the mutating call returns, never from inside it.
void FakeBackend::scheduleSnapshotReissue() {
    if (snapshotReissueScheduled_) return;
    snapshotReissueScheduled_ = true;
    QMetaObject::invokeMethod(&pump_, [this] { runSnapshotReissue(); }, Qt::QueuedConnection);
}

void FakeBackend::runSnapshotReissue() {
    // Cleared first: a bump raised while the set is being re-issued owes another
    // one, and must not be swallowed by the coalescing flag.
    snapshotReissueScheduled_ = false;
    if (!isAttached_) return;
    MutationScope guard(this);
    refreshState();
}

// ------------------------------------------------------------------- requests

cb::RequestId FakeBackend::submit(RequestKind kind, const QString &first, const QString &second,
                                  bool flag, const QString &coalesceKey) {
    if (!coalesceKey.isEmpty()) {
        // ProviderClient's pending_ set: an identical operation under the same
        // generation is coalesced onto the outstanding one.
        for (const InFlight &request : inFlight_) {
            if (request.coalesceKey == coalesceKey && request.generation == generation_)
                return request.id;
        }
    }
    InFlight request;
    request.id = nextRequest();
    request.generation = generation_;
    request.kind = kind;
    request.first = first;
    request.second = second;
    request.flag = flag;
    request.coalesceKey = coalesceKey;
    inFlight_.push_back(request);
    issued_[static_cast<int>(kind)] += 1;
    if (isProviderKind(kind)) publishProviderBusy();
    if (requestGate_ == Gate::Immediate) {
        // A controller that dropped us does not start answering again just
        // because something re-issued the snapshot set at it. Without this the
        // re-issue that a disconnect owes would silently reconnect the fake,
        // which no real controller would do.
        if (!controllerReachable_ && isControllerBound(kind)) {
            releaseRequest(
                request.id, RequestOutcome::Failure,
                {cb::ErrorCode::Network, QStringLiteral("the controller is unreachable")});
        } else {
            releaseRequest(request.id, RequestOutcome::Success, {});
        }
    }
    return request.id;
}

int FakeBackend::issuedCount(RequestKind kind) const noexcept {
    return issued_.value(static_cast<int>(kind), 0);
}

std::vector<cb::RequestId> FakeBackend::pendingRequests() const {
    std::vector<cb::RequestId> ids;
    ids.reserve(inFlight_.size());
    for (const InFlight &request : inFlight_) ids.push_back(request.id);
    return ids;
}

bool FakeBackend::isPending(cb::RequestId id) const {
    for (const InFlight &request : inFlight_)
        if (request.id == id) return true;
    return false;
}

std::optional<RequestKind> FakeBackend::pendingKind(cb::RequestId id) const {
    for (const InFlight &request : inFlight_)
        if (request.id == id) return request.kind;
    return std::nullopt;
}

cb::Generation FakeBackend::submittedGeneration(cb::RequestId id) const {
    for (const InFlight &request : inFlight_)
        if (request.id == id) return request.generation;
    return cb::Generation::Initial;
}

bool FakeBackend::releaseRequest(cb::RequestId id, RequestOutcome outcome,
                                 const cb::ErrorInfo &error) {
    MutationScope guard(this);
    const auto it = std::find_if(inFlight_.begin(), inFlight_.end(),
                                 [id](const InFlight &r) { return r.id == id; });
    if (it == inFlight_.end()) return false;
    const InFlight request = *it;
    inFlight_.erase(it);
    completeRequest(request, outcome, error);
    if (isProviderKind(request.kind)) publishProviderBusy();
    return true;
}

void FakeBackend::releaseAllRequests(RequestOutcome outcome) {
    MutationScope guard(this);
    while (!inFlight_.empty()) releaseRequest(inFlight_.front().id, outcome, {});
}

void FakeBackend::publishProviderBusy() {
    const bool busy = isProviderBusy();
    const cb::Generation generation = generation_;
    enqueue([generation, busy](cb::BackendObserver &observer) {
        observer.providerBusyChanged(generation, busy);
    });
}

void FakeBackend::completeRequest(const InFlight &request, RequestOutcome outcome,
                                  const cb::ErrorInfo &error) {
    cb::CompletionStatus status = cb::CompletionStatus::Ok;
    cb::ErrorInfo reported = error;
    if (outcome == RequestOutcome::Failure) {
        status = cb::CompletionStatus::Failed;
        if (!reported.isFailure()) reported = {cb::ErrorCode::Network, QStringLiteral("failed")};
    } else if (outcome == RequestOutcome::Cancel) {
        status = cb::CompletionStatus::Cancelled;
        if (!reported.isFailure()) reported = {cb::ErrorCode::Cancelled, QStringLiteral("cancelled")};
    }
    if (request.kind == RequestKind::SetTun) tunPending_ = false;
    if (request.kind == RequestKind::UpdateGeo) geoPending_ = false;
    // A successful /version answer is what proves the controller is there.
    if (request.kind == RequestKind::RefreshVersion && status == cb::CompletionStatus::Ok &&
        request.generation == generation_) {
        setConnected(true);
    }
    deliverCompletion(request, completionFor(request.id, request.generation, status, reported));
}

void FakeBackend::deliverCompletion(const InFlight &request, const cb::Completion &completion) {
    const bool ok = completion.status == cb::CompletionStatus::Ok;
    const StagedData data = staged_;
    switch (request.kind) {
        case RequestKind::RefreshVersion: {
            const QString version = ok ? data.version : QString();
            enqueue([completion, version](cb::BackendObserver &observer) {
                observer.versionReceived(completion, version);
            });
            break;
        }
        case RequestKind::RefreshProxies:
        case RequestKind::TestGroupDelay:
        case RequestKind::TestNodeDelay: {
            const QVector<cb::ProxyGroup> groups = ok ? data.groups : QVector<cb::ProxyGroup>{};
            const QVector<cb::ProxyNode> nodes = ok ? data.nodes : QVector<cb::ProxyNode>{};
            enqueue([completion, groups, nodes](cb::BackendObserver &observer) {
                observer.proxiesUpdated(completion, cb::makeSpan(groups), cb::makeSpan(nodes));
            });
            break;
        }
        case RequestKind::RefreshRules: {
            const QVector<cb::Rule> rules = ok ? data.rules : QVector<cb::Rule>{};
            enqueue([completion, rules](cb::BackendObserver &observer) {
                observer.rulesUpdated(completion, cb::makeSpan(rules));
            });
            break;
        }
        case RequestKind::RefreshConfig:
        case RequestKind::Attach: {
            const cb::BaseConfig config = ok ? data.config : cb::BaseConfig{};
            enqueue([completion, config](cb::BackendObserver &observer) {
                observer.configReceived(completion, config);
            });
            break;
        }
        case RequestKind::SetMode: {
            const QString mode = ok ? request.first : QString();
            enqueue([completion, mode](cb::BackendObserver &observer) {
                observer.modeChanged(completion, mode);
            });
            break;
        }
        case RequestKind::SetTun: {
            cb::TunChangeCompleted result;
            result.request = completion.request;
            result.generation = completion.generation;
            // backend-r3 B2: carried through, so a TUN change abandoned by an
            // endpoint change is marked Superseded rather than reported as a
            // protocol error the engine never committed.
            result.status = completion.status;
            result.requested = request.flag;
            // A read-back, never an echo.
            result.actual = ok ? data.tunActual : false;
            result.error = completion.error;
            enqueue([result](cb::BackendObserver &observer) { observer.tunChangeCompleted(result); });
            break;
        }
        case RequestKind::SelectNode:
        case RequestKind::ResetGroup: {
            const QString group = request.first;
            const QString node = ok ? request.second : QString();
            enqueue([completion, group, node](cb::BackendObserver &observer) {
                observer.nodeSelected(completion, group, node);
            });
            break;
        }
        case RequestKind::UpdateGeo:
            enqueue([completion](cb::BackendObserver &observer) {
                observer.geoDatabasesUpdated(completion);
            });
            break;
        case RequestKind::QueryDns: {
            const QString name = request.first;
            const QString result = ok ? data.dnsResultJson : QString();
            enqueue([completion, name, result](cb::BackendObserver &observer) {
                observer.dnsQueryFinished(completion, name, result);
            });
            break;
        }
        case RequestKind::FlushDns: {
            const bool fakeIp = request.flag;
            enqueue([completion, fakeIp](cb::BackendObserver &observer) {
                observer.dnsCacheFlushed(completion, fakeIp);
            });
            break;
        }
        case RequestKind::FetchProviders: {
            const bool rules = request.flag;
            const QVector<cb::Provider> providers = ok ? data.providers : QVector<cb::Provider>{};
            enqueue([completion, rules, providers](cb::BackendObserver &observer) {
                observer.providersReceived(completion, rules, cb::makeSpan(providers));
            });
            break;
        }
        case RequestKind::UpdateProvider:
        case RequestKind::HealthCheckProvider: {
            const QString message = ok ? QStringLiteral("done: %1").arg(request.first) : QString();
            enqueue([completion, message](cb::BackendObserver &observer) {
                observer.providerOperationFinished(completion, message);
            });
            break;
        }
        case RequestKind::ServiceStatus: {
            const cb::PrivilegedServiceStatus status =
                ok ? serviceStatus_ : cb::PrivilegedServiceStatus{};
            enqueue([completion, status](cb::BackendObserver &observer) {
                observer.privilegedServiceStatus(completion, status);
            });
            break;
        }
        case RequestKind::Detach:
        case RequestKind::CloseConnection:
        case RequestKind::CloseAllConnections:
        case RequestKind::OpenStream:
        case RequestKind::CloseStream:
            // No payload of their own: the global error channel below is the
            // only thing they report.
            break;
    }
    // The global error channel reports FAILURES. An abandoned or cancelled
    // request is not an error to show anyone: its completion already says so,
    // and the consumer's generation guard drops it.
    if (completion.status == cb::CompletionStatus::Failed) {
        enqueue([completion](cb::BackendObserver &observer) { observer.errorOccurred(completion); });
    }
}

void FakeBackend::abortInFlight(const cb::ErrorInfo &reason) {
    std::vector<InFlight> aborted;
    aborted.swap(inFlight_);
    tunPending_ = false;
    geoPending_ = false;
    for (const InFlight &request : aborted) {
        if (request.generation < generation_) {
            // The generation moved first, so the abort cannot deliver live data.
            deliverCompletion(request, completionFor(request.id, request.generation,
                                                     cb::CompletionStatus::Superseded, reason));
        } else {
            // The generation has NOT moved yet. This is the hazard
            // mihomo_client.cpp:44 documents: finished() runs synchronously
            // inside abort(), the reply still looks current, and its answer is
            // delivered as live data stamped with the generation that was about
            // to invalidate it.
            deliverCompletion(request, completionFor(request.id, request.generation,
                                                     cb::CompletionStatus::Ok, {}));
        }
    }
}

void FakeBackend::invalidate(const cb::ErrorInfo &reason) {
    if (abortOrdering_ == AbortOrdering::BumpThenAbort) {
        bumpGeneration();
        abortInFlight(reason);
    } else {
        abortInFlight(reason);
        bumpGeneration();
    }
    // An endpoint change re-issues the snapshot set by itself - attach() ends in
    // refreshState(), and detach() has no endpoint to fetch from. Any OTHER bump
    // through here is a disconnect, which owes the re-issue.
    if (!attachingEndpoint_) scheduleSnapshotReissue();
}

// ----------------------------------------------------------------- attachment

cb::Ownership FakeBackend::ownershipOf(const cb::Endpoint &endpoint) const {
    if (!cb::isValid(endpoint)) return cb::Ownership::None;
    if (cb::isValid(managedEndpoint_) && cb::isSameAddress(managedEndpoint_, endpoint))
        return cb::Ownership::Managed;
    return cb::Ownership::Attached;
}

void FakeBackend::setConnected(bool connected) {
    if (connected_ == connected) return;
    connected_ = connected;
    if (!connected) {
        invalidate({cb::ErrorCode::Superseded, QStringLiteral("the controller disconnected")});
        clearLiveState();
    }
    const cb::Generation generation = generation_;
    enqueue([generation, connected](cb::BackendObserver &observer) {
        observer.connectedChanged(generation, connected);
    });
}

void FakeBackend::clearLiveState() {
    // The five synchronous emissions of MihomoClient::clearLiveState(), now
    // queued rather than delivered from inside the mutating call.
    const cb::Completion completion =
        completionFor(cb::RequestId::Invalid, generation_, cb::CompletionStatus::Ok, {});
    enqueue([completion](cb::BackendObserver &observer) {
        observer.proxiesUpdated(completion, cb::Span<cb::ProxyGroup>{}, cb::Span<cb::ProxyNode>{});
    });
    enqueue([completion](cb::BackendObserver &observer) {
        observer.rulesUpdated(completion, cb::Span<cb::Rule>{});
    });
    const cb::Generation generation = generation_;
    enqueue([generation](cb::BackendObserver &observer) {
        observer.connectionsUpdated(generation, cb::Span<cb::Connection>{}, 0, 0);
    });
    enqueue([generation](cb::BackendObserver &observer) { observer.trafficSample(generation, 0, 0); });
    enqueue([generation](cb::BackendObserver &observer) { observer.memorySample(generation, 0, 0); });
}

void FakeBackend::refreshState() {
    submit(RequestKind::RefreshVersion);
    submit(RequestKind::RefreshProxies);
    submit(RequestKind::RefreshRules);
    submit(RequestKind::RefreshConfig);
}

cb::RequestId FakeBackend::attach(const cb::Endpoint &endpoint) noexcept {
    MutationScope guard(this);
    const cb::RequestId request = nextRequest();
    if (isAttached_ && cb::isSameEndpoint(attached_, endpoint)) {
        refreshState();
        return request;
    }
    attachingEndpoint_ = true;
    invalidate({cb::ErrorCode::Superseded, QStringLiteral("the controller changed")});
    setConnected(false);
    attached_ = endpoint;
    isAttached_ = cb::isValid(endpoint);
    clearLiveState();
    const cb::Generation generation = generation_;
    const cb::Ownership ownership = ownershipOf(endpoint);
    enqueue([generation, endpoint, ownership](cb::BackendObserver &observer) {
        observer.endpointChanged(generation, endpoint, ownership);
    });
    // The endpoint change's own re-issue. It is what makes the scheduled one
    // unnecessary here, not an optimisation on top of it.
    if (isAttached_) refreshState();
    attachingEndpoint_ = false;
    return request;
}

cb::RequestId FakeBackend::detach() noexcept {
    MutationScope guard(this);
    const cb::RequestId request = nextRequest();
    attachingEndpoint_ = true;
    // Never terminates the controller: the registry is untouched here.
    invalidate({cb::ErrorCode::Superseded, QStringLiteral("detached")});
    setConnected(false);
    attached_ = cb::Endpoint{};
    isAttached_ = false;
    clearLiveState();
    const cb::Generation generation = generation_;
    enqueue([generation](cb::BackendObserver &observer) {
        observer.endpointChanged(generation, cb::Endpoint{}, cb::Ownership::None);
    });
    attachingEndpoint_ = false;
    return request;
}

void FakeBackend::disconnectController() {
    MutationScope guard(this);
    controllerReachable_ = false;
    setConnected(false);
}

cb::Endpoint FakeBackend::discoverEndpoint() const noexcept {
    cb::Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = 9090;
    return endpoint;
}

bool FakeBackend::endpointFromConfigFile(const QString &path, cb::Endpoint *out) const noexcept {
    if (!out) return false;
    if (unparsableConfigs_.contains(path)) return false;
    const auto it = configEndpoints_.constFind(path);
    if (it != configEndpoints_.constEnd()) {
        *out = *it;
        return true;
    }
    *out = discoverEndpoint();
    return true;
}

cb::Endpoint FakeBackend::currentEndpoint() const noexcept { return attached_; }
bool FakeBackend::isAttached() const noexcept { return isAttached_; }
bool FakeBackend::isConnected() const noexcept { return connected_; }

cb::Ownership FakeBackend::attachmentOwnership() const noexcept {
    if (!isAttached_) return cb::Ownership::None;
    return ownershipOf(attached_);
}

bool FakeBackend::isExternalControllerConnected() const noexcept {
    return connected_ && attachmentOwnership() == cb::Ownership::Attached;
}

cb::RequestId FakeBackend::refreshConfig() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::RefreshConfig);
}

// ------------------------------------------------------------- controllers

void FakeBackend::addExternalController(const cb::Endpoint &endpoint) {
    controllers_.append(RunningController{endpoint, false});
}

bool FakeBackend::isControllerRunning(const cb::Endpoint &endpoint) const {
    for (const RunningController &controller : controllers_)
        if (cb::isSameAddress(controller.endpoint, endpoint)) return true;
    return false;
}

// --------------------------------------------------------------- lifecycle

QString FakeBackend::discoverBinary() const noexcept { return discoverableBinary_; }

void FakeBackend::setBinaryPath(const QString &path) noexcept { binaryPath_ = path; }
QString FakeBackend::binaryPath() const noexcept { return binaryPath_; }

bool FakeBackend::setExecutionMode(cb::ExecutionMode mode) noexcept {
    if (mode == cb::ExecutionMode::PrivilegedService && !serviceSupported_) return false;
    if (state_ != cb::CoreState::Stopped && state_ != cb::CoreState::Failed) return false;
    if (childRunning_ || retiring_ || validation_ || serviceActive_ || serviceParsing_ ||
        !cancellingValidations_.isEmpty())
        return false;
    executionMode_ = mode;
    return true;
}

cb::ExecutionMode FakeBackend::executionMode() const noexcept { return executionMode_; }
bool FakeBackend::usesPrivilegedService() const noexcept { return serviceActive_; }
cb::CoreState FakeBackend::state() const noexcept { return state_; }

cb::Ownership FakeBackend::ownership() const noexcept {
    if (childRunning_ || retiring_ || serviceActive_ || validation_ || pendingLaunch_)
        return cb::Ownership::Managed;
    return cb::Ownership::None;
}

cb::Endpoint FakeBackend::managedEndpoint() const noexcept { return managedEndpoint_; }

QStringList FakeBackend::activeConfigPaths() const noexcept {
    QStringList paths;
    if (childRunning_ || retiring_ || serviceActive_) paths.append(configPath_);
    if (validation_) paths.append(validation_->configPath);
    if (serviceParsing_) paths.append(configPath_);
    if (pendingLaunch_) paths.append(pendingLaunch_->configPath);
    // A cancelled validator's config stays active until the child exits, or the
    // snapshot is deleted out from under it.
    paths.append(cancellingValidations_);
    paths.removeAll(QString());
    paths.removeDuplicates();
    return paths;
}

bool FakeBackend::isRestartPending() const noexcept {
    return pendingLaunch_.has_value() || validation_.has_value() || serviceParsing_;
}

bool FakeBackend::isManagedCoreActive() const noexcept {
    return state_ == cb::CoreState::Running || state_ == cb::CoreState::Starting ||
           (state_ == cb::CoreState::Stopping && isRestartPending());
}

void FakeBackend::setState(cb::CoreState state) {
    if (state_ == state) return;
    state_ = state;
    const cb::Generation generation = generation_;
    const cb::Ownership owner = ownership();
    enqueue([generation, state, owner](cb::BackendObserver &observer) {
        observer.coreStateChanged(generation, state, owner);
    });
}

cb::RequestId FakeBackend::start(const QString &configPath, const QString &workDir) noexcept {
    MutationScope guard(this);
    const cb::RequestId request = nextRequest();
    explicitStop_ = false;
    // A managed start invalidates outstanding work (contract section 2), and is
    // not an endpoint change, so it owes the snapshot set (backend-r2).
    bumpGeneration();
    scheduleSnapshotReissue();
    serviceParsing_ = false;
    cancelValidation();
    pendingLaunch_.reset();
    startRequest_ = request;
    startGeneration_ = generation_;
    if (binaryPath_.isEmpty()) binaryPath_ = discoverBinary();
    if (binaryPath_.isEmpty()) {
        failManaged({cb::ErrorCode::BinaryNotFound, QStringLiteral("no mihomo binary")}, request);
        return request;
    }
    // The running core stays live while its replacement is validated.
    validation_ = Launch{configPath, workDir, request, generation_};
    if (validationGate_ == Gate::Immediate) completeValidation(true);
    return request;
}

QString FakeBackend::validatingConfigPath() const {
    return validation_ ? validation_->configPath : QString();
}

QStringList FakeBackend::cancellingValidationPaths() const { return cancellingValidations_; }

void FakeBackend::cancelValidation() {
    if (!validation_) return;
    // The child is killed but has not exited: its config path stays active.
    cancellingValidations_.append(validation_->configPath);
    validation_.reset();
}

bool FakeBackend::completeCancelledValidation(const QString &configPath) {
    MutationScope guard(this);
    const qsizetype index = cancellingValidations_.indexOf(configPath);
    if (index < 0) return false;
    cancellingValidations_.removeAt(index);
    if (!childRunning_ && !retiring_ && !validation_ && !serviceActive_) finishStop();
    return true;
}

bool FakeBackend::completeValidation(bool valid, const QString &reason) {
    MutationScope guard(this);
    if (!validation_) return false;
    const Launch launch = *validation_;
    validation_.reset();
    if (!valid) {
        const cb::ErrorInfo error{
            cb::ErrorCode::ValidationFailed,
            QStringLiteral("Core configuration validation failed: %1").arg(reason)};
        if (state_ == cb::CoreState::Running) {
            // Contract section 3: the running configuration is left intact. No
            // state change, no endpoint change, nothing leaves the active set,
            // and the generation does NOT move - the running core is still the
            // subject of every outstanding piece of work.
            const cb::Completion completion = completionFor(
                launch.request, generation_, cb::CompletionStatus::Failed, error);
            enqueue([completion](cb::BackendObserver &observer) { observer.coreFailed(completion); });
        } else {
            failManaged(error, launch.request);
        }
        return true;
    }
    launchValidated(launch);
    return true;
}

void FakeBackend::launchValidated(const Launch &launch) {
    pendingLaunch_ = launch;
    stopResult_ = cb::CoreState::Stopped;
    stopError_ = {};
    cancelProbe();
    terminateProcess();
}

void FakeBackend::launchProcess(const Launch &launch) {
    configPath_ = launch.configPath;
    workDir_ = launch.workDir;
    startRequest_ = launch.request;
    startGeneration_ = launch.generation;
    managedEndpoint_ = cb::Endpoint{};
    childKilled_ = false;
    if (executionMode_ == cb::ExecutionMode::PrivilegedService) {
        serviceActive_ = true;
        serviceStopping_ = false;
    } else {
        childRunning_ = true;
    }
    setState(cb::CoreState::Starting);
    awaitController();
}

void FakeBackend::awaitController() {
    cb::Endpoint parsed;
    if (!endpointFromConfigFile(configPath_, &parsed)) {
        failManaged({cb::ErrorCode::ConfigUnreadable,
                     QStringLiteral("No usable external-controller in %1").arg(configPath_)},
                    startRequest_);
        return;
    }
    pendingEndpoint_ = parsed;
    // The child is up; the controller is what has not answered yet.
    controllers_.append(RunningController{parsed, true});
    probing_ = true;
    probeInFlight_ = false;
    readyDeadlineMs_ =
        nowMs_ + (serviceActive_ ? kTimings.serviceIdleDeadlineMs : kTimings.idleDeadlineMs);
    hardDeadlineMs_ = nowMs_ + kTimings.hardCapMs;
}

void FakeBackend::cancelProbe() {
    // Signals disconnected BEFORE the abort: a held answer released afterwards
    // must reach nothing (contract section 5.2).
    probing_ = false;
    probeInFlight_ = false;
}

void FakeBackend::probeTick() {
    if (state_ != cb::CoreState::Starting) return;
    if (nowMs_ > readyDeadlineMs_ || nowMs_ > hardDeadlineMs_) {
        failManaged({cb::ErrorCode::ReadyTimeout,
                     QStringLiteral("Core did not answer and went quiet")},
                    startRequest_);
        return;
    }
    if (probeInFlight_) return;
    if (probeGate_ == Gate::Held) {
        probeInFlight_ = true;
        return;
    }
    deliverProbeAnswer();
}

void FakeBackend::deliverProbeAnswer() {
    probeInFlight_ = false;
    if (!probing_ || state_ != cb::CoreState::Starting) return;
    // Readiness is 200 AND a JSON object whose `version` is a string. Anything
    // else leaves the core Starting until the silence deadline expires.
    if (versionResponse_ != VersionResponse::Ok) return;
    probing_ = false;
    managedEndpoint_ = pendingEndpoint_;
    setState(cb::CoreState::Running);
    const cb::Completion completion =
        completionFor(startRequest_, generation_, cb::CompletionStatus::Ok, {});
    const cb::Endpoint endpoint = managedEndpoint_;
    enqueue([completion, endpoint](cb::BackendObserver &observer) {
        observer.coreReady(completion, endpoint);
    });
}

bool FakeBackend::releaseProbeAnswer() {
    MutationScope guard(this);
    if (!probeInFlight_) return false;
    deliverProbeAnswer();
    return true;
}

void FakeBackend::emitCoreLogLine(const QString &line) {
    MutationScope guard(this);
    if (state_ == cb::CoreState::Starting) {
        // Refreshes the idle deadline. It must NOT move the hard cap.
        readyDeadlineMs_ =
            nowMs_ + (serviceActive_ ? kTimings.serviceIdleDeadlineMs : kTimings.idleDeadlineMs);
    }
    const cb::Generation generation = generation_;
    enqueue([generation, line](cb::BackendObserver &observer) {
        observer.coreLogLine(generation, line);
    });
}

cb::RequestId FakeBackend::stop() noexcept {
    MutationScope guard(this);
    const cb::RequestId request = nextRequest();
    explicitStop_ = true;
    stopRequest_ = request;
    // A managed stop invalidates outstanding work and is not an endpoint change.
    bumpGeneration();
    stopGeneration_ = generation_;
    scheduleSnapshotReissue();
    serviceParsing_ = false;
    cancelValidation();
    pendingLaunch_.reset();
    stopResult_ = cb::CoreState::Stopped;
    stopError_ = {};
    cancelProbe();
    managedEndpoint_ = cb::Endpoint{};
    terminateProcess();
    return request;
}

void FakeBackend::terminateProcess() {
    if (serviceActive_) {
        if (!serviceStopping_) {
            serviceStopping_ = true;
            setState(cb::CoreState::Stopping);
        }
        return;
    }
    if (retiring_) {
        setState(cb::CoreState::Stopping);
        return;
    }
    if (!childRunning_) {
        finishStop();
        return;
    }
    childRunning_ = false;
    retiring_ = true;
    childKilled_ = false;
    terminateDeadlineMs_ = nowMs_ + kTimings.terminateWaitMs;
    setState(cb::CoreState::Stopping);
    if (childExitGate_ == Gate::Immediate) releaseChildExit(0);
}

bool FakeBackend::releaseChildExit(int exitCode) {
    (void)exitCode;
    MutationScope guard(this);
    if (!retiring_) return false;
    retiring_ = false;
    // Only the managed controller goes away; external ones are untouched.
    for (qsizetype i = controllers_.size() - 1; i >= 0; --i)
        if (controllers_[i].managed) controllers_.removeAt(i);
    finishStop();
    return true;
}

bool FakeBackend::crashChild(int exitCode, const QString &lastLine) {
    MutationScope guard(this);
    if (!childRunning_) return false;
    childRunning_ = false;
    for (qsizetype i = controllers_.size() - 1; i >= 0; --i)
        if (controllers_[i].managed) controllers_.removeAt(i);
    emitCoreLogLine(lastLine);
    failManaged({cb::ErrorCode::CoreExited,
                 QStringLiteral("Core exited with code %1").arg(exitCode)},
                cb::RequestId::Invalid);
    return true;
}

void FakeBackend::failManaged(const cb::ErrorInfo &reason, cb::RequestId request) {
    failRequest_ = request;
    // A managed failure invalidates outstanding work (contract section 2) and is
    // not an endpoint change.
    bumpGeneration();
    scheduleSnapshotReissue();
    serviceParsing_ = false;
    cancelValidation();
    pendingLaunch_.reset();
    cancelProbe();
    managedEndpoint_ = cb::Endpoint{};
    stopResult_ = cb::CoreState::Failed;
    stopError_ = reason;
    terminateProcess();
}

void FakeBackend::finishStop() {
    managedEndpoint_ = cb::Endpoint{};
    if (pendingLaunch_) {
        // Consumed ONLY here - that is, only once the retiring child's exit has
        // been observed (contract section 4).
        const Launch launch = *pendingLaunch_;
        pendingLaunch_.reset();
        launchProcess(launch);
        return;
    }
    if (validation_ || serviceParsing_) {
        setState(cb::CoreState::Starting);
        return;
    }
    if (!cancellingValidations_.isEmpty()) {
        setState(cb::CoreState::Stopping);
        return;
    }
    const cb::CoreState result = stopResult_;
    const cb::ErrorInfo reason = stopError_;
    stopError_ = {};
    setState(result);
    const cb::Generation generation = generation_;
    // backend-r2 A1, restated by r3 B1: a stop is the terminal outcome of an
    // operation that bumps the generation itself, so it carries the POST-bump
    // value. StopStamping::SubmitGeneration reproduces the real backend's defect
    // instead, so the suite can prove it would catch it.
    const cb::Generation stopStamp =
        stopStamping_ == StopStamping::PostBump ? generation : stopGeneration_;
    if (result == cb::CoreState::Failed) {
        // Stamped with the CURRENT generation, not the failing request's: this
        // event reports the managed core's new state and a consumer must act on
        // it. Only an abandoned request's completion carries an older stamp.
        const cb::Completion completion = completionFor(
            failRequest_, generation, cb::CompletionStatus::Failed, reason);
        failRequest_ = cb::RequestId::Invalid;
        enqueue([completion](cb::BackendObserver &observer) { observer.coreFailed(completion); });
        if (explicitStop_) {
            explicitStop_ = false;
            cb::StopCompleted result2;
            result2.request = stopRequest_;
            result2.generation = stopStamp;
            // backend-r3 B2: cleanup was requested and nothing confirmed the
            // exit. That is a failed stop, not an Ok one carrying a flag.
            result2.status = cb::CompletionStatus::Failed;
            result2.confirmed = false;
            result2.reason = reason;
            enqueue([result2](cb::BackendObserver &observer) { observer.stopCompleted(result2); });
        }
    } else {
        explicitStop_ = false;
        enqueue([generation](cb::BackendObserver &observer) { observer.coreStopped(generation); });
        cb::StopCompleted stopped;
        stopped.request = stopRequest_;
        stopped.generation = stopStamp;
        stopped.status = cb::CompletionStatus::Ok;
        stopped.confirmed = true;
        enqueue([stopped](cb::BackendObserver &observer) { observer.stopCompleted(stopped); });
    }
}

bool FakeBackend::disconnectPrivilegedService(const QString &reason) {
    MutationScope guard(this);
    if (!serviceActive_) return false;
    const bool wasStopping = serviceStopping_;
    serviceActive_ = false;
    serviceStopping_ = false;
    for (qsizetype i = controllers_.size() - 1; i >= 0; --i)
        if (controllers_[i].managed) controllers_.removeAt(i);
    if (wasStopping && stopResult_ == cb::CoreState::Stopped) {
        // Cleanup was requested, but nothing confirmed the child exited.
        //
        // This is a managed failure, so it bumps like any other (section 2) -
        // and that bump is precisely what makes the stop completion's stamp
        // observable: with the contract's PostBump stamping the stop carries the
        // new generation and a conforming consumer acts on it, while with the
        // real backend's B1 defect it carries the submit generation, compares
        // older than the coreFailed that preceded it, and is DROPPED.
        pendingLaunch_.reset();
        bumpGeneration();
        scheduleSnapshotReissue();
        stopResult_ = cb::CoreState::Failed;
        stopError_ = {cb::ErrorCode::ServiceDisconnected, reason};
        finishStop();
    } else {
        failManaged({cb::ErrorCode::ServiceDisconnected, reason}, cb::RequestId::Invalid);
    }
    return true;
}

void FakeBackend::advanceTime(qint64 ms) {
    MutationScope guard(this);
    const qint64 target = nowMs_ + ms;
    while (nowMs_ < target) {
        const qint64 step = std::min<qint64>(kTimings.probeIntervalMs, target - nowMs_);
        nowMs_ += step;
        if (retiring_ && !childKilled_ && nowMs_ >= terminateDeadlineMs_) {
            childKilled_ = true;
            if (childExitGate_ == Gate::Immediate) releaseChildExit(-9);
        }
        if (probing_) probeTick();
    }
}

void FakeBackend::mapConfigFile(const QString &configPath, const cb::Endpoint &endpoint) {
    configEndpoints_.insert(configPath, endpoint);
}

void FakeBackend::setConfigFileUnparsable(const QString &configPath) {
    if (!unparsableConfigs_.contains(configPath)) unparsableConfigs_.append(configPath);
}

// ------------------------------------------------------------------ control

cb::RequestId FakeBackend::setMode(const QString &mode) noexcept {
    MutationScope guard(this);
    return submit(RequestKind::SetMode, mode);
}

cb::RequestId FakeBackend::setTunEnabled(bool enabled) noexcept {
    MutationScope guard(this);
    if (tunPending_) return cb::RequestId::Invalid;
    tunPending_ = true;
    return submit(RequestKind::SetTun, {}, {}, enabled);
}

bool FakeBackend::isTunChangePending() const noexcept { return tunPending_; }

cb::RequestId FakeBackend::selectNode(const QString &group, const QString &node) noexcept {
    MutationScope guard(this);
    return submit(RequestKind::SelectNode, group, node);
}

cb::RequestId FakeBackend::resetGroupSelection(const QString &group) noexcept {
    MutationScope guard(this);
    return submit(RequestKind::ResetGroup, group);
}

cb::RequestId FakeBackend::testGroupDelay(const QString &group) noexcept {
    MutationScope guard(this);
    return submit(RequestKind::TestGroupDelay, group);
}

cb::RequestId FakeBackend::testNodeDelay(const QString &node) noexcept {
    MutationScope guard(this);
    return submit(RequestKind::TestNodeDelay, node);
}

cb::RequestId FakeBackend::closeConnection(const QString &id) noexcept {
    MutationScope guard(this);
    return submit(RequestKind::CloseConnection, id);
}

cb::RequestId FakeBackend::closeAllConnections() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::CloseAllConnections);
}

cb::RequestId FakeBackend::updateGeoDatabases() noexcept {
    MutationScope guard(this);
    if (geoPending_) return cb::RequestId::Invalid;
    geoPending_ = true;
    return submit(RequestKind::UpdateGeo);
}

cb::RequestId FakeBackend::queryDns(const QString &name, const QString &type) noexcept {
    MutationScope guard(this);
    return submit(RequestKind::QueryDns, name, type);
}

cb::RequestId FakeBackend::flushDnsCache(bool fakeIp) noexcept {
    MutationScope guard(this);
    return submit(RequestKind::FlushDns, {}, {}, fakeIp);
}

// ---------------------------------------------------------------- telemetry

cb::RequestId FakeBackend::refreshVersion() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::RefreshVersion);
}

cb::RequestId FakeBackend::refreshProxies() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::RefreshProxies);
}

cb::RequestId FakeBackend::refreshRules() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::RefreshRules);
}

cb::RequestId FakeBackend::openTrafficStream() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::OpenStream, QStringLiteral("traffic"));
}
cb::RequestId FakeBackend::closeTrafficStream() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::CloseStream, QStringLiteral("traffic"));
}
cb::RequestId FakeBackend::openConnectionsStream() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::OpenStream, QStringLiteral("connections"));
}
cb::RequestId FakeBackend::closeConnectionsStream() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::CloseStream, QStringLiteral("connections"));
}
cb::RequestId FakeBackend::openLogStream(const QString &level) noexcept {
    MutationScope guard(this);
    return submit(RequestKind::OpenStream, QStringLiteral("logs"), level);
}
cb::RequestId FakeBackend::closeLogStream() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::CloseStream, QStringLiteral("logs"));
}
cb::RequestId FakeBackend::openMemoryStream() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::OpenStream, QStringLiteral("memory"));
}
cb::RequestId FakeBackend::closeMemoryStream() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::CloseStream, QStringLiteral("memory"));
}

cb::RequestId FakeBackend::fetchProviders(bool rules) noexcept {
    MutationScope guard(this);
    const QString key = QStringLiteral("fetch:%1").arg(rules ? 1 : 0);
    return submit(RequestKind::FetchProviders, {}, {}, rules, key);
}

cb::RequestId FakeBackend::updateProvider(bool rules, const QString &name) noexcept {
    MutationScope guard(this);
    if (name.isEmpty()) return cb::RequestId::Invalid;
    const QString key = QStringLiteral("update:%1:%2").arg(rules ? 1 : 0).arg(name);
    return submit(RequestKind::UpdateProvider, name, {}, rules, key);
}

cb::RequestId FakeBackend::healthCheckProvider(const QString &name) noexcept {
    MutationScope guard(this);
    if (name.isEmpty()) return cb::RequestId::Invalid;
    const QString key = QStringLiteral("health:%1").arg(name);
    return submit(RequestKind::HealthCheckProvider, name, {}, false, key);
}

bool FakeBackend::isProviderBusy() const noexcept {
    for (const InFlight &request : inFlight_)
        if (isProviderKind(request.kind)) return true;
    return false;
}

// ------------------------------------------------------------- capabilities

cb::BackendIdentity FakeBackend::identity() const noexcept {
    cb::BackendIdentity identity;
    identity.name = QStringLiteral("fake");
    identity.moduleAbiVersion = 0;
    identity.interfaceRevision = 1;
    return identity;
}

cb::FeatureSet FakeBackend::features() const noexcept {
    cb::FeatureSet set = cb::Feature::ManagedLifecycle | cb::Feature::ConfirmedTunChange;
    set = set | cb::Feature::DnsQuery | cb::Feature::DnsCacheFlush;
    set = set | cb::Feature::GeoDatabaseUpdate | cb::Feature::MemoryStream;
    set = set | cb::Feature::Providers | cb::Feature::ConfigValidation;
    if (serviceSupported_) set = set | cb::Feature::PrivilegedService;
    return set;
}

bool FakeBackend::serviceSupported() const noexcept { return serviceSupported_; }
bool FakeBackend::serviceAvailable() const noexcept {
    return serviceSupported_ && serviceAvailable_;
}
cb::BackendTimings FakeBackend::timings() const noexcept { return kTimings; }

cb::RequestId FakeBackend::requestPrivilegedServiceStatus() noexcept {
    MutationScope guard(this);
    return submit(RequestKind::ServiceStatus);
}

}  // namespace testsupport::backend
