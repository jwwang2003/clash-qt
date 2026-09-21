#include "support/backend/recording_observer.h"

#include "support/backend/fake_backend.h"

namespace testsupport::backend {

RecordingObserver::RecordingObserver(FakeBackend *backend, QString name)
    : backend_(backend), name_(std::move(name)) {}

RecordingObserver::~RecordingObserver() = default;

void RecordingObserver::witness() noexcept {
    ++callbackCount;
    // Contract section 7: the backend must not invoke an observer from inside a
    // mutating call.
    if (backend_ && backend_->isInsideMutatingCall()) sawReentrantDelivery = true;
    if (onNextEvent) {
        auto action = onNextEvent;
        onNextEvent = nullptr;
        action();
    }
}

void RecordingObserver::observe(cb::Generation generation) noexcept {
    if (lastObserved_ < generation) lastObserved_ = generation;
}

bool RecordingObserver::admit(const QString &channel, const cb::Completion &completion,
                              const QString &payload) noexcept {
    const CompletionEvent event{channel, completion, payload};
    if (cb::isSuperseded(completion.generation, lastObserved_)) {
        rejectedByGeneration.push_back(event);
        return false;
    }
    if (completion.status == cb::CompletionStatus::Superseded) {
        supersededByBackend.push_back(event);
        return false;
    }
    observe(completion.generation);
    accepted.push_back(event);
    return true;
}

bool RecordingObserver::acceptedChannel(const QString &channel) const {
    return acceptedCount(channel) > 0;
}

int RecordingObserver::acceptedCount(const QString &channel) const {
    int count = 0;
    for (const CompletionEvent &event : accepted)
        if (event.channel == channel) ++count;
    return count;
}

void RecordingObserver::clear() {
    states.clear();
    accepted.clear();
    rejectedByGeneration.clear();
    supersededByBackend.clear();
    stops.clear();
    tunChanges.clear();
    failures.clear();
    logLines.clear();
    endpoints.clear();
    connects.clear();
    readyEndpoints.clear();
    stoppedCount = 0;
    liveStateClears = 0;
    callbackCount = 0;
}

void RecordingObserver::coreStateChanged(cb::Generation generation, cb::CoreState state,
                                         cb::Ownership ownership) noexcept {
    witness();
    observe(generation);
    states.push_back(StateEvent{generation, state, ownership});
}

void RecordingObserver::coreReady(const cb::Completion &completion,
                                  const cb::Endpoint &endpoint) noexcept {
    witness();
    if (!admit(QStringLiteral("ready"), completion, endpoint.host)) return;
    readyEndpoints.push_back(endpoint);
}

void RecordingObserver::coreLogLine(cb::Generation generation, const QString &line) noexcept {
    witness();
    observe(generation);
    logLines.append(line);
}

void RecordingObserver::coreFailed(const cb::Completion &completion) noexcept {
    witness();
    if (!admit(QStringLiteral("failed"), completion, completion.error.message)) return;
    failures.push_back(completion);
}

void RecordingObserver::coreStopped(cb::Generation generation) noexcept {
    witness();
    observe(generation);
    ++stoppedCount;
}

void RecordingObserver::stopCompleted(const cb::StopCompleted &result) noexcept {
    witness();
    observe(result.generation);
    stops.push_back(result);
}

void RecordingObserver::endpointChanged(cb::Generation generation, const cb::Endpoint &endpoint,
                                        cb::Ownership ownership) noexcept {
    witness();
    (void)ownership;
    observe(generation);
    endpoints.push_back(endpoint);
}

void RecordingObserver::connectedChanged(cb::Generation generation, bool connected) noexcept {
    witness();
    observe(generation);
    connects.push_back(connected);
}

void RecordingObserver::configReceived(const cb::Completion &completion,
                                       const cb::BaseConfig &config) noexcept {
    witness();
    admit(QStringLiteral("config"), completion, config.mode);
}

void RecordingObserver::modeChanged(const cb::Completion &completion,
                                    const QString &mode) noexcept {
    witness();
    admit(QStringLiteral("mode"), completion, mode);
}

void RecordingObserver::tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept {
    witness();
    observe(result.generation);
    tunChanges.push_back(result);
}

void RecordingObserver::nodeSelected(const cb::Completion &completion, const QString &group,
                                     const QString &node) noexcept {
    witness();
    admit(QStringLiteral("node"), completion, group + QLatin1Char('/') + node);
}

void RecordingObserver::versionReceived(const cb::Completion &completion,
                                        const QString &version) noexcept {
    witness();
    admit(QStringLiteral("version"), completion, version);
}

void RecordingObserver::proxiesUpdated(const cb::Completion &completion,
                                       cb::Span<cb::ProxyGroup> groups,
                                       cb::Span<cb::ProxyNode> nodes) noexcept {
    witness();
    if (groups.isEmpty() && nodes.isEmpty()) ++liveStateClears;
    admit(QStringLiteral("proxies"), completion, QString::number(groups.size()));
}

void RecordingObserver::rulesUpdated(const cb::Completion &completion,
                                     cb::Span<cb::Rule> rules) noexcept {
    witness();
    admit(QStringLiteral("rules"), completion, QString::number(rules.size()));
}

void RecordingObserver::providersReceived(const cb::Completion &completion, bool rules,
                                          cb::Span<cb::Provider> providers) noexcept {
    witness();
    admit(rules ? QStringLiteral("providers/rules") : QStringLiteral("providers/proxies"),
          completion, QString::number(providers.size()));
}

void RecordingObserver::errorOccurred(const cb::Completion &completion) noexcept {
    witness();
    admit(QStringLiteral("error"), completion, completion.error.message);
}

}  // namespace testsupport::backend
