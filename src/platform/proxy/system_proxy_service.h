#pragma once

#include <functional>
#include <optional>
#include <QObject>

#include "platform/proxy/system_proxy.h"

namespace platform {

enum class SystemProxyAction { Refresh, Enable, Disable, Restore };

struct SystemProxyResult {
    SystemProxyState state;
    bool success = true;
    QString error;
};

/// Serializes OS proxy commands on background workers. All methods/signals and
/// the cache belong to the creating (GUI) thread; backend work never touches UI.
class SystemProxyService : public QObject {
    Q_OBJECT
public:
    using Operation = std::function<SystemProxyResult(SystemProxyAction, const ProxyConfig &)>;
    explicit SystemProxyService(QObject *parent = nullptr, Operation operation = {});
    static SystemProxyService *instance();
    const SystemProxyState &state() const { return state_; }
    bool isBusy() const { return busy_; }
    bool isShuttingDown() const { return shuttingDown_; }
    bool isChanging() const {
        return (shuttingDown_ && !shutdownDone_) || restoreQueued_ || queuedChange_.has_value() ||
               (active_ && active_->action != SystemProxyAction::Refresh);
    }
    bool isRestoring() const {
        return (shuttingDown_ && !shutdownDone_) || restoreQueued_ ||
               (active_ && active_->action == SystemProxyAction::Restore);
    }

public slots:
    void refresh();
    void setEnabled(bool enabled, const platform::ProxyConfig &config = {});
    void restoreOwned();
    /// Discards queued changes, finishes the active job, then restores last.
    /// Keep the application event loop alive until shutdownFinished is emitted.
    void shutdown();

signals:
    void stateChanged(const platform::SystemProxyState &state);
    void busyChanged(bool busy);
    void activityChanged();
    void changeFinished(bool requested, bool success, const QString &error);
    void restoreFinished(bool success, const QString &error);
    void shutdownFinished(bool success, const QString &error);

private:
    struct Request { SystemProxyAction action; ProxyConfig config; };
    void processNext();
    void start(const Request &request);
    void setBusy(bool busy);
    static SystemProxyResult execute(SystemProxyAction action, const ProxyConfig &config);

    Operation operation_;
    SystemProxyState state_;
    std::optional<Request> active_;
    std::optional<Request> queuedChange_;
    bool refreshQueued_ = false;
    bool restoreQueued_ = false;
    bool busy_ = false;
    bool delivering_ = false;
    bool shuttingDown_ = false;
    bool shutdownDone_ = false;
};
} // namespace platform
