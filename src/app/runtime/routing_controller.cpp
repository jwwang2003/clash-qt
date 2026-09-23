#include "app/runtime/routing_controller.h"

#include <QTimer>

#include "platform/proxy/system_proxy_service.h"

namespace app::runtime {

namespace cb = core::backend;

RoutingController::RoutingController(cb::MihomoBackend &backend,
                                     platform::SystemProxyService *proxy, RestoreDelays delays,
                                     QObject *parent)
    : QObject(parent), backend_(backend), proxy_(proxy), delays_(delays) {
    if (proxy_) {
        connect(proxy_, &platform::SystemProxyService::stateChanged, this,
                [this](const platform::SystemProxyState &) { Q_EMIT routingStateChanged(); });
        connect(proxy_, &platform::SystemProxyService::activityChanged, this,
                &RoutingController::routingStateChanged);
        connect(proxy_, &platform::SystemProxyService::busyChanged, this, [this](bool busy) {
            if (!busy) proxyRequest_.reset();
            Q_EMIT routingStateChanged();
        });
        connect(proxy_, &platform::SystemProxyService::changeFinished, this,
                [this](bool requested, bool success, const QString &error) {
                    if (!success) {
                        lastError_ = error.isEmpty()
                                         ? (requested ? tr("Could not set the system proxy.")
                                                      : tr("Could not clear the system proxy."))
                                         : error;
                        Q_EMIT routingStateChanged();
                        Q_EMIT errorOccurred(lastError_);
                        return;
                    }
                    lastError_.clear();
                    Q_EMIT routingStateChanged();
                    // Read back, never echo the request.
                    Q_EMIT systemProxyConfirmed(systemProxyEnabled());
                });
        connect(proxy_, &platform::SystemProxyService::restoreFinished, this,
                [this](bool success, const QString &error) {
                    // main.cpp:256-259, verbatim including the message, except
                    // that it is raised on this object's own error channel
                    // instead of being emitted through MihomoClient's.
                    // Deliberately NOT suppressed while shutting down: main.cpp
                    // did not suppress it either, and SystemProxyService emits
                    // restoreFinished on the shutdown path too.
                    if (!success) {
                        lastError_ = tr("Could not restore the system proxy: %1").arg(error);
                        Q_EMIT errorOccurred(lastError_);
                    }
                    Q_EMIT routingStateChanged();
                });
    }
    backend_.addObserver(this);
}

RoutingController::~RoutingController() { backend_.removeObserver(this); }

RestoreDelays RoutingController::delays() const { return delays_; }

// ------------------------------------------------------------ system proxy

void RoutingController::setProxyTarget(const platform::ProxyConfig &target) {
    target_ = target;
    Q_EMIT routingStateChanged();
}

platform::ProxyConfig RoutingController::proxyTarget() const { return target_; }

bool RoutingController::systemProxyEnabled() const {
    if (!proxy_) return false;
    const platform::SystemProxyState &state = proxy_->state();
    // settings_page.cpp:260-261's `matches`: the OS has a proxy AND it is the
    // one we would set. A proxy someone else set is not our switch being on.
    return state.config.port != 0 && state.config.port == target_.port &&
           state.config.host == target_.host;
}

bool RoutingController::systemProxyPending() const {
    if (!proxy_) return false;
    const platform::SystemProxyState &state = proxy_->state();
    // settings_page.cpp:258. A background read must not look like a change, but
    // an initial read with no cached state yet must.
    return proxy_->isChanging() || (!state.valid && proxy_->isBusy());
}

bool RoutingController::systemProxyAvailable() const {
    if (!proxy_) return false;
    const platform::SystemProxyState &state = proxy_->state();
    return state.supported && state.valid && !systemProxyPending() && !proxy_->isShuttingDown() &&
           (systemProxyEnabled() || (backend_.isConnected() && target_.port != 0));
}

void RoutingController::requestSystemProxy(bool enabled) {
    if (!proxy_ || proxy_->isShuttingDown()) {
        Q_EMIT routingStateChanged();
        return;
    }
    if (enabled && (!backend_.isConnected() || target_.port == 0)) {
        lastError_ = tr("Connect to a core with a reported proxy port first.");
        Q_EMIT routingStateChanged();
        Q_EMIT errorOccurred(lastError_);
        return;
    }
    lastError_.clear();
    proxyRequest_ = enabled;
    proxy_->setEnabled(enabled, target_);
    Q_EMIT routingStateChanged();
}

void RoutingController::toggleSystemProxy() {
    // settings_page.cpp:215-217: the hotkey drove a toggle that was disabled
    // when the change was not allowed, so it was inert then. Same here.
    if (!systemProxyAvailable()) return;
    requestSystemProxy(!systemProxyEnabled());
}

void RoutingController::refreshSystemProxy() {
    if (proxy_) proxy_->refresh();
}

// --------------------------------------------------------------------- TUN

bool RoutingController::tunEnabled() const { return tunEnabled_; }
bool RoutingController::tunPending() const { return tunPending_; }

bool RoutingController::tunAvailable() const {
    return backend_.isConnected() && configKnown_ && !tunPending_;
}

QString RoutingController::tunBlockedReason() const { return tunBlockedReason_; }

void RoutingController::setTunBlockedReason(const QString &reason) {
    if (tunBlockedReason_ == reason) return;
    tunBlockedReason_ = reason;
    Q_EMIT routingStateChanged();
}

void RoutingController::requestTun(bool enabled) {
    // routing_controls.cpp:78-81. Nothing is sent when the controller has not
    // reported a configuration, a change is already in flight, or the request
    // matches the confirmed state.
    if (!backend_.isConnected() || !configKnown_ || tunPending_ || enabled == tunEnabled_) {
        Q_EMIT routingStateChanged();
        return;
    }
    if (enabled && !tunBlockedReason_.isEmpty()) {
        lastError_ = tunBlockedReason_;
        Q_EMIT routingStateChanged();
        Q_EMIT errorOccurred(lastError_);
        return;
    }
    const cb::RequestId id = backend_.setTunEnabled(enabled);
    if (id == cb::RequestId::Invalid) {
        // Rejected outright: no completion will arrive, so nothing may be left
        // pending. The confirmed value does not move.
        Q_EMIT routingStateChanged();
        return;
    }
    tunPending_ = true;
    lastError_.clear();
    Q_EMIT routingStateChanged();
}

void RoutingController::tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept {
    if (cb::isSuperseded(result.generation, lastObserved_)) return;
    observe(result.generation);
    tunPending_ = false;
    // `actual` is the read-back. Publishing `requested` here is precisely the
    // "pending change reported as success" defect.
    tunEnabled_ = result.actual;
    // A supersession is not a user-visible failure: the change was abandoned
    // because the controller moved on, not because it was rejected. Until the
    // contract made every completion carry a status, this arrived
    // indistinguishable from a protocol error and was shown to the user as one.
    if (result.status == cb::CompletionStatus::Superseded) {
        Q_EMIT routingStateChanged();
        return;
    }
    if (result.error.isFailure()) {
        lastError_ = result.error.message;
        Q_EMIT routingStateChanged();
        Q_EMIT errorOccurred(lastError_);
        return;
    }
    lastError_.clear();
    Q_EMIT routingStateChanged();
    // routing_controls.cpp:51. A read-back that disagrees with the request is
    // not an applied change and must not be reported as one.
    if (result.requested == result.actual) Q_EMIT tunConfirmed(result.actual);
}

// -------------------------------------------------------------------- mode

QStringList RoutingController::modes() {
    // main_window.cpp:220-222, in that order: the cycle hotkey walks it.
    return {QStringLiteral("rule"), QStringLiteral("global"), QStringLiteral("direct")};
}

QString RoutingController::mode() const { return mode_; }
bool RoutingController::modePending() const { return modePending_; }

void RoutingController::requestMode(const QString &mode) {
    if (mode.isEmpty() || modePending_ || mode == mode_) {
        Q_EMIT routingStateChanged();
        return;
    }
    const cb::RequestId id = backend_.setMode(mode);
    if (id == cb::RequestId::Invalid) {
        Q_EMIT routingStateChanged();
        return;
    }
    requestedMode_ = mode;
    modePending_ = true;
    Q_EMIT routingStateChanged();
}

void RoutingController::cycleMode() {
    const QStringList known = modes();
    const int current = known.indexOf(mode_);
    // An unknown or unreported mode starts the cycle at the first entry rather
    // than refusing to move.
    requestMode(known.at(current < 0 ? 0 : (current + 1) % known.size()));
}

void RoutingController::modeChanged(const cb::Completion &completion,
                                    const QString &mode) noexcept {
    if (!admit(completion)) {
        if (completion.status != cb::CompletionStatus::Ok && modePending_) {
            modePending_ = false;
            Q_EMIT routingStateChanged();
        }
        return;
    }
    modePending_ = false;
    requestedMode_.clear();
    if (mode_ == mode) {
        Q_EMIT routingStateChanged();
        return;
    }
    mode_ = mode;
    Q_EMIT routingStateChanged();
    Q_EMIT modeConfirmed(mode_);
}

// ------------------------------------------------- confirmed state sources

void RoutingController::configReceived(const cb::Completion &completion,
                                       const cb::BaseConfig &config) noexcept {
    if (!admit(completion)) return;
    configKnown_ = true;
    // The controller's own report is the confirmed value for both controls.
    // A change still in flight is not overwritten by a snapshot that predates
    // its completion.
    if (!tunPending_) tunEnabled_ = config.tunEnabled;
    if (!modePending_ && !config.mode.isEmpty()) mode_ = config.mode;
    Q_EMIT routingStateChanged();
}

void RoutingController::clearControllerState() {
    configKnown_ = false;
    tunEnabled_ = false;
    tunPending_ = false;
    modePending_ = false;
    requestedMode_.clear();
    mode_.clear();
    lastError_.clear();
}

// ------------------------------------------------------- group D: restores

void RoutingController::coreStateChanged(cb::Generation generation, cb::CoreState state,
                                         cb::Ownership) noexcept {
    observe(generation);
    // main.cpp:260-268.
    if (state != cb::CoreState::Stopped && state != cb::CoreState::Failed) return;
    QTimer::singleShot(delays_.coreStoppedMs, this, [this] {
        // The re-validation, not the delay, is the behaviour: a restart that
        // has already re-entered Starting must keep its proxy.
        const cb::CoreState now = backend_.state();
        if (now == cb::CoreState::Stopped || now == cb::CoreState::Failed) restoreProxy();
    });
}

void RoutingController::connectedChanged(cb::Generation generation, bool connected) noexcept {
    observe(generation);
    if (connected) {
        Q_EMIT routingStateChanged();
        return;
    }
    clearControllerState();
    Q_EMIT routingStateChanged();
    // main.cpp:269-273.
    QTimer::singleShot(delays_.disconnectedMs, this, [this] {
        // Re-validated: a controller that came back inside the grace window
        // keeps its proxy.
        if (!backend_.isConnected()) restoreProxy();
    });
}

void RoutingController::endpointChanged(cb::Generation generation, const cb::Endpoint &,
                                        cb::Ownership) noexcept {
    observe(generation);
    clearControllerState();
    Q_EMIT routingStateChanged();
}

void RoutingController::restoreProxy() {
    ++restores_;
    Q_EMIT proxyRestored();
    // restoreOwned() only. The application never performs a blanket restore of
    // whatever the OS happens to hold; it gives back only what it took.
    if (proxy_) proxy_->restoreOwned();
}

int RoutingController::restoreCount() const { return restores_; }

QString RoutingController::lastError() const { return lastError_; }

cb::Generation RoutingController::lastObservedGeneration() const { return lastObserved_; }

bool RoutingController::admit(const cb::Completion &completion) noexcept {
    if (completion.status == cb::CompletionStatus::Superseded) return false;
    if (cb::isSuperseded(completion.generation, lastObserved_)) return false;
    observe(completion.generation);
    return completion.isOk();
}

void RoutingController::observe(cb::Generation generation) noexcept {
    if (lastObserved_ < generation) lastObserved_ = generation;
}

}  // namespace app::runtime
