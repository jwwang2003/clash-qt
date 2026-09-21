#include "core/mihomo/mihomo_backend.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#include "core/mihomo/controller_discovery.h"

namespace core {
namespace {

cb::Endpoint toBackend(const Endpoint &endpoint) {
    cb::Endpoint result;
    result.host = endpoint.host;
    result.port = endpoint.port;
    result.secret = endpoint.secret;
    return result;
}

Endpoint fromBackend(const cb::Endpoint &endpoint) {
    Endpoint result;
    result.host = endpoint.host;
    result.port = endpoint.port;
    result.secret = endpoint.secret;
    return result;
}

cb::CoreState toBackend(CoreState state) {
    switch (state) {
        case CoreState::Stopped:  return cb::CoreState::Stopped;
        case CoreState::Starting: return cb::CoreState::Starting;
        case CoreState::Running:  return cb::CoreState::Running;
        case CoreState::Stopping: return cb::CoreState::Stopping;
        case CoreState::Failed:   return cb::CoreState::Failed;
    }
    return cb::CoreState::Failed;
}

cb::ErrorCode toBackend(CoreFailure failure) {
    switch (failure) {
        case CoreFailure::None:                return cb::ErrorCode::None;
        case CoreFailure::BinaryNotFound:      return cb::ErrorCode::BinaryNotFound;
        case CoreFailure::ValidationFailed:    return cb::ErrorCode::ValidationFailed;
        case CoreFailure::LaunchFailed:        return cb::ErrorCode::LaunchFailed;
        case CoreFailure::CoreExited:          return cb::ErrorCode::CoreExited;
        case CoreFailure::ReadyTimeout:        return cb::ErrorCode::ReadyTimeout;
        case CoreFailure::ServiceUnavailable:  return cb::ErrorCode::ServiceUnavailable;
        case CoreFailure::ServiceDisconnected: return cb::ErrorCode::ServiceDisconnected;
        case CoreFailure::ConfigUnreadable:    return cb::ErrorCode::ConfigUnreadable;
        case CoreFailure::Unspecified:         break;
    }
    return cb::ErrorCode::Unspecified;
}

cb::Provider toBackend(const Provider &provider) {
    cb::Provider result;
    result.name = provider.name;
    result.type = provider.type;
    result.vehicle = provider.vehicle;
    result.behavior = provider.behavior;
    result.count = provider.count;
    result.updated = provider.updated;
    result.used = provider.used;
    result.total = provider.total;
    result.expires = provider.expires;
    return result;
}

cb::Completion completionFor(const cb::RequestId id, cb::Generation generation,
                             cb::CompletionStatus status, const cb::ErrorInfo &error) {
    cb::Completion completion;
    completion.request = id;
    completion.generation = generation;
    completion.status = status;
    completion.error = error;
    return completion;
}

}  // namespace

// ------------------------------------------------------------------ scopes

MihomoBackendImpl::MutationScope::MutationScope(MihomoBackendImpl *self) noexcept : self_(self) {
    ++self_->mutatingDepth_;
}
MihomoBackendImpl::MutationScope::~MutationScope() {
    if (--self_->mutatingDepth_ == 0 && !self_->queue_.empty()) self_->scheduleDrain();
}

// ------------------------------------------------------------- construction

MihomoBackendImpl::MihomoBackendImpl(PrivilegedCoreService *service)
    : client_(nullptr), providers_(&client_, nullptr), process_(nullptr, service) {
    connectCollaborators();
}

MihomoBackendImpl::~MihomoBackendImpl() {
    // The collaborators outlive nothing; drop every connection first so a
    // teardown signal cannot reach a half-destroyed queue.
    client_.disconnect(&pump_);
    providers_.disconnect(&pump_);
    process_.disconnect(&pump_);
    observers_.clear();
    observerAddedAt_.clear();
    queue_.clear();
}

void MihomoBackendImpl::connectCollaborators() {
    QObject *context = &pump_;

    // ---- the invalidating events. The generation is bumped HERE, before the
    // collaborator aborts anything, because finished() may run synchronously
    // inside abort() (mihomo_client.cpp:44). An observer told afterwards would
    // already have accepted the reply the bump was meant to discard.
    QObject::connect(&client_, &MihomoClient::invalidating, context, [this] {
        bumpGeneration();
        // backend-r3 B2. MihomoClient cancels an outstanding TUN change on
        // exactly the three paths that emit this signal - setEndpoint, detach
        // and setConnected(false) - and finishTunChange() follows it
        // synchronously (mihomo_client.cpp:46-47, :78-79, :119-120). Recording
        // it HERE is what lets tunChangeFinished classify that cancellation as a
        // supersession without matching on the error text, which is translated
        // and is not something any caller may switch on.
        if (tunRequest_ != cb::RequestId::Invalid) tunSuperseded_ = true;
        // An endpoint change re-issues the snapshot set by itself: setEndpoint
        // ends in refreshState(). Any OTHER bump - a disconnect - does not, and
        // fetchRules/fetchConfigs have no recovery poll.
        if (!attachingEndpoint_) scheduleSnapshotReissue();
    });

    QObject::connect(&client_, &MihomoClient::endpointChanged, context, [this] {
        const cb::Generation generation = generation_;
        const cb::Endpoint endpoint = toBackend(client_.endpoint());
        const cb::Ownership ownership = attachmentOwnership();
        enqueue([generation, endpoint, ownership](cb::BackendObserver &observer) {
            observer.endpointChanged(generation, endpoint, ownership);
        });
    });

    QObject::connect(&client_, &MihomoClient::connectedChanged, context, [this](bool connected) {
        const cb::Generation generation = generation_;
        enqueue([generation, connected](cb::BackendObserver &observer) {
            observer.connectedChanged(generation, connected);
        });
    });

    // ---- payload signals. Inside a reply handler they are CAPTURED and
    // published by the settle that follows; outside one - the five synchronous
    // emissions of clearLiveState(), and the stream samples - they are
    // unsolicited and published straight away.
    QObject::connect(&client_, &MihomoClient::versionReceived, context,
                     [this](const QString &version) {
        capture(Kind::Version, [&](Payload &payload) { payload.version = version; });
    });

    QObject::connect(&client_, &MihomoClient::proxiesUpdated, context,
                     [this](const QVector<ProxyGroup> &groups, const QHash<QString, ProxyNode> &nodes) {
        capture(Kind::Proxies, [&](Payload &payload) {
            payload.groups = groups;
            payload.nodes.clear();
            payload.nodes.reserve(nodes.size());
            // A span, not a hash: no container crosses the boundary.
            for (auto it = nodes.constBegin(); it != nodes.constEnd(); ++it)
                payload.nodes.append(it.value());
        });
    });

    QObject::connect(&client_, &MihomoClient::rulesUpdated, context,
                     [this](const QVector<Rule> &rules) {
        capture(Kind::Rules, [&](Payload &payload) { payload.ruleList = rules; });
    });

    QObject::connect(&client_, &MihomoClient::configReceived, context,
                     [this](const BaseConfig &config) {
        capture(Kind::Config, [&](Payload &payload) { payload.config = config; });
    });

    QObject::connect(&client_, &MihomoClient::modeChanged, context, [this](const QString &mode) {
        capture(Kind::Mode, [&](Payload &payload) { payload.mode = mode; });
    });

    QObject::connect(&client_, &MihomoClient::nodeSelected, context,
                     [this](const QString &group, const QString &node) {
        capture(Kind::SelectNode, [&](Payload &payload) {
            payload.group = group;
            payload.node = node;
        });
    });

    QObject::connect(&client_, &MihomoClient::geoDatabasesUpdated, context,
                     [this](const QString &) { capture(Kind::UpdateGeo, [](Payload &) {}); });

    QObject::connect(&client_, &MihomoClient::dnsQueryFinished, context,
                     [this](const QString &name, const QJsonObject &result, const QString &) {
        capture(Kind::QueryDns, [&](Payload &payload) {
            payload.dnsName = name;
            // As text: a parsed document cannot cross a module boundary.
            payload.dnsJson = result.isEmpty()
                ? QString()
                : QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
        });
    });

    QObject::connect(&client_, &MihomoClient::dnsCacheFlushed, context,
                     [this](bool fakeIp, const QString &) {
        capture(Kind::FlushDns, [&](Payload &payload) { payload.fakeIp = fakeIp; });
    });

    // ---- the single terminal event of every REST operation.
    QObject::connect(&client_, &MihomoClient::requestSettled, context,
                     [this](quint64 operation, bool superseded, const QString &error) {
        settleRest(operation, superseded, error);
    });

    // ---- streams and the unsolicited channel.
    QObject::connect(&client_, &MihomoClient::trafficSample, context,
                     [this](quint64 up, quint64 down) {
        const cb::Generation generation = generation_;
        enqueue([generation, up, down](cb::BackendObserver &observer) {
            observer.trafficSample(generation, up, down);
        });
    });
    QObject::connect(&client_, &MihomoClient::memorySample, context,
                     [this](quint64 inuse, quint64 oslimit) {
        const cb::Generation generation = generation_;
        enqueue([generation, inuse, oslimit](cb::BackendObserver &observer) {
            observer.memorySample(generation, inuse, oslimit);
        });
    });
    QObject::connect(&client_, &MihomoClient::connectionsUpdated, context,
                     [this](const QVector<Connection> &connections, quint64 up, quint64 down) {
        const cb::Generation generation = generation_;
        enqueue([generation, connections, up, down](cb::BackendObserver &observer) {
            observer.connectionsUpdated(generation, cb::makeSpan(connections), up, down);
        });
    });
    QObject::connect(&client_, &MihomoClient::logReceived, context, [this](const LogEntry &entry) {
        const cb::Generation generation = generation_;
        enqueue([generation, entry](cb::BackendObserver &observer) {
            observer.logReceived(generation, entry);
        });
    });
    QObject::connect(&client_, &MihomoClient::tunChangeFinished, context,
                     [this](bool requested, bool actual, const QString &error) {
        cb::TunChangeCompleted result;
        result.request = tunRequest_;
        // A TUN change bumps nothing of its own, so A1's ordinary rule applies:
        // the generation it was SUBMITTED under. On the superseded path that
        // makes it compare older than the newest observed value, which is the
        // consumer's second line of defence behind the explicit mark below.
        result.generation = tunGeneration_;
        result.requested = requested;
        // A READ-BACK, never an echo: MihomoClient re-reads /configs after the
        // PATCH, including after an HTTP failure.
        result.actual = actual;
        if (tunSuperseded_) {
            // backend-r3 B2. "Cancelled because the controller changed" is a
            // SUPERSESSION, not a protocol error: the controller did not answer
            // unusably, it stopped being the controller. Labelling it Protocol
            // told a consumer the engine misbehaved when nothing had.
            result.status = cb::CompletionStatus::Superseded;
            result.error = {cb::ErrorCode::Superseded,
                            error.isEmpty()
                                ? QCoreApplication::translate("core::MihomoBackend",
                                                              "the generation moved on")
                                : error};
        } else if (!error.isEmpty()) {
            result.status = cb::CompletionStatus::Failed;
            result.error = {cb::ErrorCode::Protocol, error};
        } else {
            result.status = cb::CompletionStatus::Ok;
        }
        tunSuperseded_ = false;
        tunRequest_ = cb::RequestId::Invalid;
        enqueue([result](cb::BackendObserver &observer) { observer.tunChangeCompleted(result); });
    });
    QObject::connect(&client_, &MihomoClient::errorOccurred, context, [this](const QString &message) {
        // Inside a reply handler the settle reports it; this channel is for
        // failures that belong to no request, such as a stream that dropped.
        if (client_.currentOperation() != 0) return;
        const cb::Completion completion = completionFor(
            cb::RequestId::Invalid, generation_, cb::CompletionStatus::Failed,
            {cb::ErrorCode::Network, message});
        enqueue([completion](cb::BackendObserver &observer) { observer.errorOccurred(completion); });
    });

    // ---- providers
    QObject::connect(&providers_, &ProviderClient::providersReceived, context,
                     [this](bool rules, const QVector<Provider> &providers) {
        capture(Kind::FetchProviders, [&](Payload &payload) {
            payload.providerRules = rules;
            payload.providers.clear();
            payload.providers.reserve(providers.size());
            for (const Provider &provider : providers) payload.providers.append(toBackend(provider));
        });
    });
    QObject::connect(&providers_, &ProviderClient::operationFinished, context,
                     [this](const QString &message) {
        capture(Kind::UpdateProvider, [&](Payload &payload) { payload.message = message; });
    });
    QObject::connect(&providers_, &ProviderClient::busyChanged, context, [this](bool busy) {
        if (providerBusy_ == busy) return;
        providerBusy_ = busy;
        publishProviderBusy();
    });
    QObject::connect(&providers_, &ProviderClient::errorOccurred, context,
                     [this](const QString &message) {
        if (!providers_.currentKey().isEmpty()) return;
        const cb::Completion completion = completionFor(
            cb::RequestId::Invalid, generation_, cb::CompletionStatus::Failed,
            {cb::ErrorCode::Network, message});
        enqueue([completion](cb::BackendObserver &observer) { observer.errorOccurred(completion); });
    });
    QObject::connect(&providers_, &ProviderClient::requestSettled, context,
                     [this](const QString &key, bool superseded, const QString &error) {
        Pending request;
        if (const auto it = providerRequests_.find(key); it != providerRequests_.end()) {
            request = *it;
            providerRequests_.erase(it);
        } else {
            // A refresh ProviderClient issued for itself after an update.
            request.generation = generation_;
            request.kind = payload_.has ? payload_.kind : Kind::FetchProviders;
        }
        cb::CompletionStatus status = cb::CompletionStatus::Ok;
        cb::ErrorInfo reported;
        if (superseded) {
            status = cb::CompletionStatus::Superseded;
            reported = {cb::ErrorCode::Superseded,
                        QCoreApplication::translate("core::MihomoBackend",
                                                    "the generation moved on")};
        } else if (!error.isEmpty()) {
            status = cb::CompletionStatus::Failed;
            reported = {cb::ErrorCode::Network, error};
        }
        const Payload payload = payload_;
        payload_.clear();
        publish(request, status, reported, payload);
    });

    // ---- the managed core
    QObject::connect(&process_, &CoreProcess::stateChanged, context, [this](CoreState state) {
        const cb::Generation generation = generation_;
        const cb::CoreState published = toBackend(state);
        const cb::Ownership owner = ownership();
        enqueue([generation, published, owner](cb::BackendObserver &observer) {
            observer.coreStateChanged(generation, published, owner);
        });
    });
    QObject::connect(&process_, &CoreProcess::logLine, context, [this](const QString &line) {
        const cb::Generation generation = generation_;
        enqueue([generation, line](cb::BackendObserver &observer) {
            observer.coreLogLine(generation, line);
        });
    });
    QObject::connect(&process_, &CoreProcess::ready, context, [this](const Endpoint &endpoint) {
        // The terminal outcome of an operation that bumped the generation
        // itself carries the POST-bump value (backend-r2 A1).
        const cb::Completion completion =
            completionFor(startRequest_, startGeneration_, cb::CompletionStatus::Ok, {});
        const cb::Endpoint published = toBackend(endpoint);
        startRequest_ = cb::RequestId::Invalid;
        enqueue([completion, published](cb::BackendObserver &observer) {
            observer.coreReady(completion, published);
        });
    });
    QObject::connect(&process_, &CoreProcess::failed, context, [this](const QString &reason) {
        // A managed failure invalidates outstanding work.
        bumpGeneration();
        scheduleSnapshotReissue();
        const cb::Completion completion = completionFor(
            startRequest_, generation_, cb::CompletionStatus::Failed,
            {toBackend(process_.lastFailure()), reason});
        startRequest_ = cb::RequestId::Invalid;
        enqueue([completion](cb::BackendObserver &observer) {
            observer.coreFailed(completion);
            observer.errorOccurred(completion);
        });
    });
    QObject::connect(&process_, &CoreProcess::stopped, context, [this] {
        const cb::Generation generation = generation_;
        enqueue([generation](cb::BackendObserver &observer) { observer.coreStopped(generation); });
    });
    QObject::connect(&process_, &CoreProcess::stopFinished, context,
                     [this](bool confirmed, const QString &error) {
        // CoreProcess answers every completed teardown; only one that answers a
        // stop() of ours is a StopCompleted.
        if (stopRequest_ == cb::RequestId::Invalid) return;
        cb::StopCompleted result;
        result.request = stopRequest_;
        // backend-r3 B1, applying A1 to the case that broke it.
        //
        // A stop's terminal outcome carries the generation current AFTER every
        // bump its own operation caused - not only the bump stop() itself made
        // at submit. On the unconfirmed path CoreProcess emits failed() and THEN
        // stopFinished() from one call (core_process.cpp:486-491), and the
        // failed handler above bumps. Stamping stopGeneration_ here therefore
        // delivered coreFailed(N+1) followed by stopCompleted(N), and a consumer
        // applying section 2's MANDATORY rejection rule dropped the unconfirmed
        // stop - the precise failure A1 was written to prevent, and the one the
        // application turns into a warning that blocks quit.
        //
        // Re-read at emit rather than reordering CoreProcess's two signals: the
        // re-read absorbs ANY bump between submit and the terminal answer, while
        // a reorder would fix only the one path we know about today and would
        // change an emission order that CoreProcess's own consumers rely on.
        // This is not "stamped at delivery" - the queue is drained later and the
        // stamp does not move again - and it matches what the fake already does
        // (fake_backend.cpp:859-880), so fake and real stop diverging.
        Q_ASSERT(!cb::isSuperseded(generation_, stopGeneration_));
        result.generation = generation_;
        result.confirmed = confirmed;
        // backend-r3 B2: the outcome is now stated, not inferred from a bool.
        result.status = confirmed ? cb::CompletionStatus::Ok : cb::CompletionStatus::Failed;
        if (!confirmed) {
            // NOT a success. Lease cleanup was requested and nothing confirmed
            // the child exited; the application turns this into a shutdown
            // warning that blocks quit.
            const cb::ErrorCode code = process_.lastFailure() == CoreFailure::None
                                           ? cb::ErrorCode::ServiceDisconnected
                                           : toBackend(process_.lastFailure());
            result.reason = {code, error.isEmpty()
                                       ? QCoreApplication::translate(
                                             "core::MihomoBackend",
                                             "The privileged service disconnected before "
                                             "confirming core shutdown.")
                                       : error};
        }
        stopRequest_ = cb::RequestId::Invalid;
        enqueue([result](cb::BackendObserver &observer) { observer.stopCompleted(result); });
    });
    QObject::connect(&process_, &CoreProcess::engineResolved, context,
                     [this](const QString &path, const QString &label, const QString &provenance) {
        // G1: whatever is resolved is reported. It reaches a consumer on the log
        // channel as well as through resolvedEngine(), so provenance is never
        // implied by silence.
        const cb::Generation generation = generation_;
        const QString line = QCoreApplication::translate("core::MihomoBackend",
                                                         "engine %1 [%2] %3")
                                 .arg(path, label, provenance);
        enqueue([generation, line](cb::BackendObserver &observer) {
            observer.coreLogLine(generation, line);
        });
    });
    QObject::connect(&process_, &CoreProcess::serviceStatusReceived, context,
                     [this](const QJsonObject &status) {
        if (serviceStatusRequest_ == cb::RequestId::Invalid) return;
        cb::PrivilegedServiceStatus published;
        published.state = cb::ServiceState::Connected;
        published.version = status.value(QStringLiteral("version")).toString();
        const cb::Completion completion = completionFor(
            serviceStatusRequest_, serviceStatusGeneration_, cb::CompletionStatus::Ok, {});
        serviceStatusRequest_ = cb::RequestId::Invalid;
        enqueue([completion, published](cb::BackendObserver &observer) {
            observer.privilegedServiceStatus(completion, published);
        });
    });
    QObject::connect(&process_, &CoreProcess::serviceConnectedChanged, context,
                     [this](bool connected) {
        if (connected || serviceStatusRequest_ == cb::RequestId::Invalid) return;
        cb::PrivilegedServiceStatus published;
        published.state = cb::ServiceState::Failed;
        published.error = {cb::ErrorCode::ServiceDisconnected,
                           QCoreApplication::translate(
                               "core::MihomoBackend",
                               "The privileged service disconnected before answering.")};
        const cb::Completion completion =
            completionFor(serviceStatusRequest_, serviceStatusGeneration_,
                          cb::CompletionStatus::Failed, published.error);
        serviceStatusRequest_ = cb::RequestId::Invalid;
        enqueue([completion, published](cb::BackendObserver &observer) {
            observer.privilegedServiceStatus(completion, published);
        });
    });
}

// ---------------------------------------------------------------- delivery

void MihomoBackendImpl::enqueue(std::function<void(cb::BackendObserver &)> deliver) {
    queue_.push_back(Event{++sequence_, std::move(deliver)});
    // Never delivered from here: a mutating call in progress drains on its way
    // out, and anything produced outside one is drained by the event loop.
    if (mutatingDepth_ == 0) scheduleDrain();
}

void MihomoBackendImpl::scheduleDrain() {
    if (drainScheduled_) return;
    drainScheduled_ = true;
    QMetaObject::invokeMethod(&pump_, [this] { drain(); }, Qt::QueuedConnection);
}

void MihomoBackendImpl::drain() {
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

bool MihomoBackendImpl::drainPendingEvents(int timeoutMs) {
    QDeadlineTimer deadline(timeoutMs);
    while (hasPendingEvents()) {
        if (deadline.hasExpired()) return !hasPendingEvents();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

bool MihomoBackendImpl::addObserver(cb::BackendObserver *observer) noexcept {
    if (!observer) return false;
    if (std::find(observers_.begin(), observers_.end(), observer) != observers_.end()) return false;
    observers_.push_back(observer);
    observerAddedAt_.insert(observer, sequence_ + 1);
    return true;
}

bool MihomoBackendImpl::removeObserver(cb::BackendObserver *observer) noexcept {
    const auto it = std::find(observers_.begin(), observers_.end(), observer);
    if (it == observers_.end()) return false;
    observers_.erase(it);
    observerAddedAt_.remove(observer);
    return true;
}

// ---------------------------------------------------------------- identity

cb::RequestId MihomoBackendImpl::nextRequest() noexcept {
    return static_cast<cb::RequestId>(++nextRequest_);
}

void MihomoBackendImpl::bumpGeneration() noexcept {
    generation_ = static_cast<cb::Generation>(cb::number(generation_) + 1);
}

void MihomoBackendImpl::scheduleSnapshotReissue() {
    if (snapshotReissueScheduled_) return;
    snapshotReissueScheduled_ = true;
    QMetaObject::invokeMethod(&pump_, [this] {
        snapshotReissueScheduled_ = false;
        if (!isAttached()) return;
        // Deliberately unregistered: nobody asked for these, so their
        // completions carry RequestId::Invalid. They exist because a bump that
        // is not an endpoint change discards in-flight /rules and /configs
        // replies, and those two have no recovery poll anywhere.
        client_.fetchVersion();
        client_.fetchProxies();
        client_.fetchRules();
        client_.fetchConfigs();
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------- requests

cb::RequestId MihomoBackendImpl::submitRest(Kind kind, quint64 operation, const QString &first,
                                            const QString &second, bool flag) {
    if (operation == 0) return cb::RequestId::Invalid;
    Pending request;
    request.id = nextRequest();
    request.generation = generation_;
    request.kind = kind;
    request.first = first;
    request.second = second;
    request.flag = flag;
    // An operation that already settled synchronously (an abort delivered from
    // inside the call that issued it) has no entry to make.
    restRequests_.insert(operation, request);
    return request.id;
}

void MihomoBackendImpl::capture(Kind kind, const std::function<void(Payload &)> &fill) {
    const bool solicited =
        client_.currentOperation() != 0 || !providers_.currentKey().isEmpty();
    Payload payload;
    payload.has = true;
    payload.kind = kind;
    fill(payload);
    if (solicited) {
        payload_ = payload;
        return;
    }
    // Unsolicited: the cleared live state, or a refresh the collaborator issued
    // for itself. Published immediately, completing no request of anyone's.
    Pending request;
    request.generation = generation_;
    request.kind = kind;
    publish(request, cb::CompletionStatus::Ok, {}, payload);
}

void MihomoBackendImpl::settleRest(quint64 operation, bool superseded, const QString &error) {
    Pending request;
    if (const auto it = restRequests_.find(operation); it != restRequests_.end()) {
        request = *it;
        restRequests_.erase(it);
    } else {
        request.generation = generation_;
        request.kind = payload_.has ? payload_.kind : Kind::Unknown;
    }
    cb::CompletionStatus status = cb::CompletionStatus::Ok;
    cb::ErrorInfo reported;
    if (superseded) {
        // backend-r2 A2: an abandoned completion is MARKED, not left for the
        // consumer to infer. With non-re-entrant delivery the completions an
        // abort produces are queued before the invalidating event, so a
        // generation comparison alone could never catch them.
        status = cb::CompletionStatus::Superseded;
        reported = {cb::ErrorCode::Superseded,
                    QCoreApplication::translate("core::MihomoBackend", "the generation moved on")};
    } else if (!error.isEmpty()) {
        status = cb::CompletionStatus::Failed;
        reported = {cb::ErrorCode::Network, error};
    }
    const Payload payload = payload_;
    payload_.clear();
    publish(request, status, reported, payload);
}

void MihomoBackendImpl::publish(const Pending &request, cb::CompletionStatus status,
                                const cb::ErrorInfo &error, const Payload &payload) {
    const cb::Completion completion =
        completionFor(request.id, request.generation, status, error);
    const bool ok = status == cb::CompletionStatus::Ok;
    // A completion that is not Ok carries no payload: empty, never stale.
    const Kind kind = request.kind != Kind::Unknown ? request.kind
                                                    : (payload.has ? payload.kind : Kind::Unknown);

    switch (kind) {
        case Kind::Version: {
            const QString version = ok ? payload.version : QString();
            enqueue([completion, version](cb::BackendObserver &observer) {
                observer.versionReceived(completion, version);
            });
            break;
        }
        case Kind::Proxies:
        case Kind::SelectNode:
        case Kind::ResetGroup:
        case Kind::TestGroupDelay:
        case Kind::TestNodeDelay: {
            if (kind == Kind::SelectNode || kind == Kind::ResetGroup) {
                const QString group = request.first.isEmpty() ? payload.group : request.first;
                const QString node = ok ? (request.second.isEmpty() ? payload.node : request.second)
                                        : QString();
                enqueue([completion, group, node](cb::BackendObserver &observer) {
                    observer.nodeSelected(completion, group, node);
                });
                break;
            }
            const QVector<cb::ProxyGroup> groups = ok ? payload.groups : QVector<cb::ProxyGroup>{};
            const QVector<cb::ProxyNode> nodes = ok ? payload.nodes : QVector<cb::ProxyNode>{};
            enqueue([completion, groups, nodes](cb::BackendObserver &observer) {
                observer.proxiesUpdated(completion, cb::makeSpan(groups), cb::makeSpan(nodes));
            });
            break;
        }
        case Kind::Rules: {
            const QVector<cb::Rule> rules = ok ? payload.ruleList : QVector<cb::Rule>{};
            enqueue([completion, rules](cb::BackendObserver &observer) {
                observer.rulesUpdated(completion, cb::makeSpan(rules));
            });
            break;
        }
        case Kind::Config: {
            const cb::BaseConfig config = ok ? payload.config : cb::BaseConfig{};
            enqueue([completion, config](cb::BackendObserver &observer) {
                observer.configReceived(completion, config);
            });
            break;
        }
        case Kind::Mode: {
            const QString mode = ok ? (request.first.isEmpty() ? payload.mode : request.first)
                                    : QString();
            enqueue([completion, mode](cb::BackendObserver &observer) {
                observer.modeChanged(completion, mode);
            });
            break;
        }
        case Kind::UpdateGeo:
            enqueue([completion](cb::BackendObserver &observer) {
                observer.geoDatabasesUpdated(completion);
            });
            break;
        case Kind::QueryDns: {
            const QString name = request.first.isEmpty() ? payload.dnsName : request.first;
            const QString result = ok ? payload.dnsJson : QString();
            enqueue([completion, name, result](cb::BackendObserver &observer) {
                observer.dnsQueryFinished(completion, name, result);
            });
            break;
        }
        case Kind::FlushDns: {
            const bool fakeIp = request.flag || payload.fakeIp;
            enqueue([completion, fakeIp](cb::BackendObserver &observer) {
                observer.dnsCacheFlushed(completion, fakeIp);
            });
            break;
        }
        case Kind::FetchProviders: {
            const bool rules = request.kind == Kind::FetchProviders ? request.flag
                                                                   : payload.providerRules;
            const QVector<cb::Provider> providers = ok ? payload.providers
                                                       : QVector<cb::Provider>{};
            enqueue([completion, rules, providers](cb::BackendObserver &observer) {
                observer.providersReceived(completion, rules, cb::makeSpan(providers));
            });
            break;
        }
        case Kind::UpdateProvider:
        case Kind::HealthCheckProvider: {
            const QString message = ok ? payload.message : QString();
            enqueue([completion, message](cb::BackendObserver &observer) {
                observer.providerOperationFinished(completion, message);
            });
            break;
        }
        case Kind::CloseConnection:
        case Kind::CloseAllConnections:
        case Kind::Unknown:
            // No payload of their own; the error channel below is all they say.
            break;
    }

    // The global error channel reports FAILURES. A superseded completion is not
    // an error to show anyone: its own status already says so.
    if (status == cb::CompletionStatus::Failed) {
        enqueue([completion](cb::BackendObserver &observer) { observer.errorOccurred(completion); });
    }
}

void MihomoBackendImpl::publishProviderBusy() {
    const cb::Generation generation = generation_;
    const bool busy = providerBusy_;
    enqueue([generation, busy](cb::BackendObserver &observer) {
        observer.providerBusyChanged(generation, busy);
    });
}

// --------------------------------------------------------------- lifecycle

QString MihomoBackendImpl::discoverBinary() const noexcept { return resolveManagedEngine().path; }

void MihomoBackendImpl::setBinaryPath(const QString &path) noexcept {
    MutationScope guard(this);
    process_.setBinaryPath(path);
}

QString MihomoBackendImpl::binaryPath() const noexcept { return process_.binaryPath(); }

bool MihomoBackendImpl::setExecutionMode(cb::ExecutionMode mode) noexcept {
    MutationScope guard(this);
    return process_.setUseService(mode == cb::ExecutionMode::PrivilegedService);
}

cb::ExecutionMode MihomoBackendImpl::executionMode() const noexcept {
    return process_.isServiceMode() ? cb::ExecutionMode::PrivilegedService
                                    : cb::ExecutionMode::Managed;
}

bool MihomoBackendImpl::usesPrivilegedService() const noexcept {
    return process_.usesPrivilegedService();
}

cb::RequestId MihomoBackendImpl::start(const QString &configPath, const QString &workDir) noexcept {
    MutationScope guard(this);
    // A managed start invalidates outstanding work, so the bump precedes it and
    // the snapshot set is re-issued afterwards.
    bumpGeneration();
    startRequest_ = nextRequest();
    startGeneration_ = generation_;
    const cb::RequestId request = startRequest_;
    process_.start(configPath, workDir);
    scheduleSnapshotReissue();
    return request;
}

cb::RequestId MihomoBackendImpl::stop() noexcept {
    MutationScope guard(this);
    bumpGeneration();
    stopRequest_ = nextRequest();
    stopGeneration_ = generation_;
    process_.stop();
    scheduleSnapshotReissue();
    return stopRequest_;
}

cb::CoreState MihomoBackendImpl::state() const noexcept { return toBackend(process_.state()); }

cb::Ownership MihomoBackendImpl::ownership() const noexcept {
    switch (process_.state()) {
        case CoreState::Starting:
        case CoreState::Running:
        case CoreState::Stopping:
            return cb::Ownership::Managed;
        case CoreState::Stopped:
        case CoreState::Failed:
            break;
    }
    return cb::Ownership::None;
}

cb::Endpoint MihomoBackendImpl::managedEndpoint() const noexcept {
    return toBackend(process_.endpoint());
}

QStringList MihomoBackendImpl::activeConfigPaths() const noexcept {
    return process_.activeConfigPaths();
}

bool MihomoBackendImpl::isRestartPending() const noexcept { return process_.isRestartPending(); }

bool MihomoBackendImpl::isManagedCoreActive() const noexcept {
    const CoreState state = process_.state();
    return state == CoreState::Running || state == CoreState::Starting ||
           (state == CoreState::Stopping && process_.isRestartPending());
}

// -------------------------------------------------------------- attachment

cb::Endpoint MihomoBackendImpl::discoverEndpoint() const noexcept {
    return toBackend(core::discoverEndpoint());
}

bool MihomoBackendImpl::endpointFromConfigFile(const QString &path,
                                               cb::Endpoint *out) const noexcept {
    if (!out) return false;
    // yaml-cpp throws throughout this path; controller_discovery converts every
    // exception to an empty optional, so nothing unwinds across the interface.
    const std::optional<Endpoint> parsed = core::endpointFromConfigFile(path);
    if (!parsed) return false;
    *out = toBackend(*parsed);
    return true;
}

cb::Ownership MihomoBackendImpl::ownershipOf(const Endpoint &endpoint) const {
    if (!endpoint.isValid()) return cb::Ownership::None;
    const Endpoint managed = process_.endpoint();
    if (managed.isValid() && managed.host == endpoint.host && managed.port == endpoint.port)
        return cb::Ownership::Managed;
    return cb::Ownership::Attached;
}

cb::RequestId MihomoBackendImpl::attach(const cb::Endpoint &endpoint) noexcept {
    MutationScope guard(this);
    const cb::RequestId request = nextRequest();
    // setEndpoint already ends in refreshState(), so the re-issue obligation is
    // met without a second one; the flag is what stops invalidating() adding it.
    attachingEndpoint_ = true;
    client_.setEndpoint(fromBackend(endpoint));
    attachingEndpoint_ = false;
    return request;
}

cb::RequestId MihomoBackendImpl::detach() noexcept {
    MutationScope guard(this);
    const cb::RequestId request = nextRequest();
    attachingEndpoint_ = true;  // nothing to re-issue: we are talking to nobody
    // NEVER terminates the controller. Only BackendLifecycle::stop() terminates
    // anything, and only a managed core.
    client_.detach();
    attachingEndpoint_ = false;
    return request;
}

cb::Endpoint MihomoBackendImpl::currentEndpoint() const noexcept {
    return toBackend(client_.endpoint());
}

bool MihomoBackendImpl::isAttached() const noexcept { return client_.endpoint().isValid(); }
bool MihomoBackendImpl::isConnected() const noexcept { return client_.isConnected(); }

cb::Ownership MihomoBackendImpl::attachmentOwnership() const noexcept {
    return ownershipOf(client_.endpoint());
}

bool MihomoBackendImpl::isExternalControllerConnected() const noexcept {
    return isConnected() && attachmentOwnership() == cb::Ownership::Attached;
}

cb::RequestId MihomoBackendImpl::refreshConfig() noexcept {
    MutationScope guard(this);
    return submitRest(Kind::Config, client_.fetchConfigs());
}

// ----------------------------------------------------------------- control

cb::RequestId MihomoBackendImpl::setMode(const QString &mode) noexcept {
    MutationScope guard(this);
    if (mode.isEmpty()) return cb::RequestId::Invalid;
    return submitRest(Kind::Mode, client_.patchMode(mode), mode);
}

cb::RequestId MihomoBackendImpl::setTunEnabled(bool enabled) noexcept {
    MutationScope guard(this);
    if (client_.isTunChangePending()) return cb::RequestId::Invalid;
    // Registered BEFORE the call: MihomoClient answers synchronously when it is
    // not connected, so tunChangeFinished can reach us from inside it.
    const cb::RequestId request = nextRequest();
    tunRequest_ = request;
    tunGeneration_ = generation_;
    tunSuperseded_ = false;
    if (client_.setTunEnabled(enabled) == 0) {
        tunRequest_ = cb::RequestId::Invalid;
        tunSuperseded_ = false;
        return cb::RequestId::Invalid;
    }
    return request;
}

bool MihomoBackendImpl::isTunChangePending() const noexcept {
    return client_.isTunChangePending();
}

cb::RequestId MihomoBackendImpl::selectNode(const QString &group, const QString &node) noexcept {
    MutationScope guard(this);
    if (group.isEmpty() || node.isEmpty()) return cb::RequestId::Invalid;
    return submitRest(Kind::SelectNode, client_.selectNode(group, node), group, node);
}

cb::RequestId MihomoBackendImpl::resetGroupSelection(const QString &group) noexcept {
    MutationScope guard(this);
    if (group.isEmpty()) return cb::RequestId::Invalid;
    return submitRest(Kind::ResetGroup, client_.resetGroupSelection(group), group);
}

cb::RequestId MihomoBackendImpl::testGroupDelay(const QString &group) noexcept {
    MutationScope guard(this);
    if (group.isEmpty()) return cb::RequestId::Invalid;
    return submitRest(Kind::TestGroupDelay, client_.testGroupDelay(group), group);
}

cb::RequestId MihomoBackendImpl::testNodeDelay(const QString &node) noexcept {
    MutationScope guard(this);
    if (node.isEmpty()) return cb::RequestId::Invalid;
    return submitRest(Kind::TestNodeDelay, client_.testNodeDelay(node), node);
}

cb::RequestId MihomoBackendImpl::closeConnection(const QString &id) noexcept {
    MutationScope guard(this);
    if (id.isEmpty()) return cb::RequestId::Invalid;
    return submitRest(Kind::CloseConnection, client_.closeConnection(id), id);
}

cb::RequestId MihomoBackendImpl::closeAllConnections() noexcept {
    MutationScope guard(this);
    return submitRest(Kind::CloseAllConnections, client_.closeAllConnections());
}

cb::RequestId MihomoBackendImpl::updateGeoDatabases() noexcept {
    MutationScope guard(this);
    // Single in-flight: MihomoClient answers 0 for a second call.
    return submitRest(Kind::UpdateGeo, client_.updateGeoDatabases());
}

cb::RequestId MihomoBackendImpl::queryDns(const QString &name, const QString &type) noexcept {
    MutationScope guard(this);
    if (name.isEmpty()) return cb::RequestId::Invalid;
    return submitRest(Kind::QueryDns, client_.queryDns(name, type.isEmpty() ? QStringLiteral("A") : type),
                      name, type);
}

cb::RequestId MihomoBackendImpl::flushDnsCache(bool fakeIp) noexcept {
    MutationScope guard(this);
    return submitRest(Kind::FlushDns, client_.flushDnsCache(fakeIp), {}, {}, fakeIp);
}

// --------------------------------------------------------------- telemetry

cb::RequestId MihomoBackendImpl::refreshVersion() noexcept {
    MutationScope guard(this);
    return submitRest(Kind::Version, client_.fetchVersion());
}

cb::RequestId MihomoBackendImpl::refreshProxies() noexcept {
    MutationScope guard(this);
    return submitRest(Kind::Proxies, client_.fetchProxies());
}

cb::RequestId MihomoBackendImpl::refreshRules() noexcept {
    MutationScope guard(this);
    return submitRest(Kind::Rules, client_.fetchRules());
}

cb::RequestId MihomoBackendImpl::openTrafficStream() noexcept {
    MutationScope guard(this);
    client_.openTrafficStream();
    return nextRequest();
}
cb::RequestId MihomoBackendImpl::closeTrafficStream() noexcept {
    MutationScope guard(this);
    client_.closeTrafficStream();
    return nextRequest();
}
cb::RequestId MihomoBackendImpl::openConnectionsStream() noexcept {
    MutationScope guard(this);
    client_.openConnectionsStream();
    return nextRequest();
}
cb::RequestId MihomoBackendImpl::closeConnectionsStream() noexcept {
    MutationScope guard(this);
    client_.closeConnectionsStream();
    return nextRequest();
}
cb::RequestId MihomoBackendImpl::openLogStream(const QString &level) noexcept {
    MutationScope guard(this);
    client_.openLogStream(level.isEmpty() ? QStringLiteral("info") : level);
    return nextRequest();
}
cb::RequestId MihomoBackendImpl::closeLogStream() noexcept {
    MutationScope guard(this);
    client_.closeLogStream();
    return nextRequest();
}
cb::RequestId MihomoBackendImpl::openMemoryStream() noexcept {
    MutationScope guard(this);
    client_.openMemoryStream();
    return nextRequest();
}
cb::RequestId MihomoBackendImpl::closeMemoryStream() noexcept {
    MutationScope guard(this);
    client_.closeMemoryStream();
    return nextRequest();
}

cb::RequestId MihomoBackendImpl::fetchProviders(bool rules) noexcept {
    MutationScope guard(this);
    const QString key = providers_.fetch(rules);
    if (key.isEmpty()) return cb::RequestId::Invalid;
    // (Generation, operation identity) -> coalescing. A duplicate submission
    // returns the OUTSTANDING request's id; minting one per submission would
    // mint two ids for one issued request (backend-r2, answer 1).
    if (const auto it = providerRequests_.constFind(key); it != providerRequests_.constEnd())
        return it->id;
    Pending request;
    request.id = nextRequest();
    request.generation = generation_;
    request.kind = Kind::FetchProviders;
    request.flag = rules;
    providerRequests_.insert(key, request);
    return request.id;
}

cb::RequestId MihomoBackendImpl::updateProvider(bool rules, const QString &name) noexcept {
    MutationScope guard(this);
    const QString key = providers_.update(rules, name);
    if (key.isEmpty()) return cb::RequestId::Invalid;
    if (const auto it = providerRequests_.constFind(key); it != providerRequests_.constEnd())
        return it->id;
    Pending request;
    request.id = nextRequest();
    request.generation = generation_;
    request.kind = Kind::UpdateProvider;
    request.first = name;
    request.flag = rules;
    providerRequests_.insert(key, request);
    return request.id;
}

cb::RequestId MihomoBackendImpl::healthCheckProvider(const QString &name) noexcept {
    MutationScope guard(this);
    const QString key = providers_.healthCheck(name);
    if (key.isEmpty()) return cb::RequestId::Invalid;
    if (const auto it = providerRequests_.constFind(key); it != providerRequests_.constEnd())
        return it->id;
    Pending request;
    request.id = nextRequest();
    request.generation = generation_;
    request.kind = Kind::HealthCheckProvider;
    request.first = name;
    providerRequests_.insert(key, request);
    return request.id;
}

bool MihomoBackendImpl::isProviderBusy() const noexcept { return providerBusy_; }

// ------------------------------------------------------------ capabilities

cb::BackendIdentity MihomoBackendImpl::identity() const noexcept {
    cb::BackendIdentity identity;
    identity.name = QStringLiteral("clash-qt.mihomo");
    identity.moduleAbiVersion = 0;  // COMPONENT-ABI assigns this in P4
    identity.interfaceRevision = 1;  // backend-r2 is revision 1 of this interface
    return identity;
}

cb::FeatureSet MihomoBackendImpl::features() const noexcept {
    cb::FeatureSet set{};
    set = set | cb::Feature::ManagedLifecycle | cb::Feature::ConfirmedTunChange |
          cb::Feature::DnsQuery | cb::Feature::DnsCacheFlush | cb::Feature::GeoDatabaseUpdate |
          cb::Feature::MemoryStream | cb::Feature::Providers | cb::Feature::ConfigValidation;
    if (process_.isServiceSupported()) set = set | cb::Feature::PrivilegedService;
    return set;
}

bool MihomoBackendImpl::serviceSupported() const noexcept { return process_.isServiceSupported(); }
bool MihomoBackendImpl::serviceAvailable() const noexcept { return process_.isServiceAvailable(); }

void MihomoBackendImpl::setTimings(const CoreTimings &timings) { process_.setTimings(timings); }

cb::BackendTimings MihomoBackendImpl::timings() const noexcept {
    const CoreTimings actual = process_.timings();
    cb::BackendTimings published;
    published.idleDeadlineMs = static_cast<std::uint32_t>(actual.idleDeadlineMs);
    published.serviceIdleDeadlineMs = static_cast<std::uint32_t>(actual.serviceIdleDeadlineMs);
    published.hardCapMs = static_cast<std::uint32_t>(actual.hardCapMs);
    published.probeIntervalMs = static_cast<std::uint32_t>(actual.probeIntervalMs);
    published.probeTimeoutMs = static_cast<std::uint32_t>(actual.probeTimeoutMs);
    published.terminateWaitMs = static_cast<std::uint32_t>(actual.terminateWaitMs);
    return published;
}

cb::RequestId MihomoBackendImpl::requestPrivilegedServiceStatus() noexcept {
    MutationScope guard(this);
    if (!process_.isServiceSupported()) {
        cb::PrivilegedServiceStatus status;
        status.state = cb::ServiceState::Unsupported;
        status.error = {cb::ErrorCode::NotSupported,
                        QCoreApplication::translate(
                            "core::MihomoBackend",
                            "This backend was given no privileged core service.")};
        const cb::Completion completion = completionFor(
            cb::RequestId::Invalid, generation_, cb::CompletionStatus::Rejected, status.error);
        enqueue([completion, status](cb::BackendObserver &observer) {
            observer.privilegedServiceStatus(completion, status);
        });
        return cb::RequestId::Invalid;
    }
    if (serviceStatusRequest_ != cb::RequestId::Invalid) return serviceStatusRequest_;
    serviceStatusRequest_ = nextRequest();
    serviceStatusGeneration_ = generation_;
    process_.requestServiceStatus();
    return serviceStatusRequest_;
}

}  // namespace core
