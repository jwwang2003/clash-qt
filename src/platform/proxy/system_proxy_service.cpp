#include "platform/proxy/system_proxy_service.h"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QPointer>
#include <QThread>
#include <QtConcurrentRun>

#include <exception>

namespace platform {
namespace {
bool sameConfig(const ProxyConfig &first, const ProxyConfig &second) {
    return first.host == second.host && first.port == second.port &&
           first.socksPort == second.socksPort && first.bypass == second.bypass;
}
} // namespace

SystemProxyService::SystemProxyService(QObject *parent, Operation operation)
    : QObject(parent), operation_(operation ? std::move(operation) : execute) {}

SystemProxyService *SystemProxyService::instance() {
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    static QPointer<SystemProxyService> service;
    if (!service) service = new SystemProxyService(QCoreApplication::instance());
    return service;
}

SystemProxyResult SystemProxyService::execute(SystemProxyAction action, const ProxyConfig &config) {
    SystemProxyResult result;
    switch (action) {
        case SystemProxyAction::Enable: result.success = SystemProxy::enable(config); break;
        case SystemProxyAction::Disable: result.success = SystemProxy::disable(); break;
        case SystemProxyAction::Restore: result.success = SystemProxy::restoreOwned(); break;
        case SystemProxyAction::Refresh: break;
    }
    // Capture the operation error before readback, on the same worker thread.
    if (!result.success) result.error = SystemProxy::lastError();
    result.state = SystemProxy::state();
    // Restoration has already checked the owned service and every write. Its
    // success must not depend on finding an active primary service afterwards
    // (for example when quitting offline). Keep that read error in the cache.
    if (!result.state.valid && !(action == SystemProxyAction::Restore && result.success)) {
        result.success = false;
        if (!result.error.isEmpty()) result.error += '\n';
        result.error += result.state.error;
    }
    if (result.success && action == SystemProxyAction::Enable &&
        (!result.state.owned || result.state.config.host != config.host ||
         result.state.config.port != config.port || result.state.config.socksPort != config.socksPort)) {
        result.success = false;
        result.error = tr("The operating system did not confirm the requested proxy settings.");
    }
    if (!result.success && result.error.isEmpty()) result.error = tr("The system proxy operation failed.");
    return result;
}

void SystemProxyService::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged(busy);
}

void SystemProxyService::refresh() {
    if (shuttingDown_ || active_ || delivering_) return; // The current job includes readback.
    refreshQueued_ = true;
    processNext();
}

void SystemProxyService::setEnabled(bool enabled, const ProxyConfig &config) {
    if (shuttingDown_) return;
    const Request request{enabled ? SystemProxyAction::Enable : SystemProxyAction::Disable, config};
    if (active_ && active_->action == request.action &&
        (request.action != SystemProxyAction::Enable || sameConfig(active_->config, config))) {
        queuedChange_.reset(); // Latest intent already matches the in-flight operation.
        emit activityChanged();
        return;
    }
    queuedChange_ = request;
    refreshQueued_ = false;
    emit activityChanged();
    processNext();
}

void SystemProxyService::restoreOwned() {
    if (shuttingDown_) return;
    queuedChange_.reset();
    refreshQueued_ = false;
    if (!active_ || active_->action != SystemProxyAction::Restore) restoreQueued_ = true;
    emit activityChanged();
    processNext();
}

void SystemProxyService::shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    queuedChange_.reset();
    refreshQueued_ = false;
    restoreQueued_ = !active_ || active_->action != SystemProxyAction::Restore;
    emit activityChanged();
    processNext();
}

void SystemProxyService::processNext() {
    if (active_ || delivering_ || shutdownDone_) return;
    if (restoreQueued_) {
        restoreQueued_ = false;
        start({SystemProxyAction::Restore, {}});
    } else if (queuedChange_ && !shuttingDown_) {
        const Request request = *queuedChange_;
        queuedChange_.reset();
        start(request);
    } else if (refreshQueued_ && !shuttingDown_) {
        refreshQueued_ = false;
        start({SystemProxyAction::Refresh, {}});
    } else {
        setBusy(false);
        emit activityChanged();
    }
}

void SystemProxyService::start(const Request &request) {
    active_ = request;
    setBusy(true);
    emit activityChanged();
    auto *watcher = new QFutureWatcher<SystemProxyResult>(this);
    connect(watcher, &QFutureWatcher<SystemProxyResult>::finished, this, [this, watcher, request] {
        const SystemProxyResult result = watcher->result();
        watcher->deleteLater();
        active_.reset();
        delivering_ = true;
        // A failed read must not turn a previously confirmed enabled switch off.
        const ProxyConfig previous = state_.config;
        const bool owned = state_.owned;
        state_ = result.state;
        if (!state_.valid) { state_.config = previous; state_.owned = owned; }
        emit stateChanged(state_);
        if (request.action == SystemProxyAction::Enable || request.action == SystemProxyAction::Disable)
            emit changeFinished(request.action == SystemProxyAction::Enable, result.success, result.error);
        if (request.action == SystemProxyAction::Restore) {
            emit restoreFinished(result.success, result.error);
            if (shuttingDown_) {
                shutdownDone_ = true;
                setBusy(false);
                emit shutdownFinished(result.success, result.error);
            }
        }
        delivering_ = false;
        processNext();
    });
    const Operation operation = operation_;
    watcher->setFuture(QtConcurrent::run([operation, request] {
        try {
            return operation(request.action, request.config);
        } catch (const std::exception &error) {
            SystemProxyResult result;
            result.success = false;
            result.error = QString::fromUtf8(error.what());
            result.state.error = result.error;
            return result;
        } catch (...) {
            SystemProxyResult result;
            result.success = false;
            result.error = QObject::tr("The system proxy worker failed unexpectedly.");
            result.state.error = result.error;
            return result;
        }
    }));
}
} // namespace platform
