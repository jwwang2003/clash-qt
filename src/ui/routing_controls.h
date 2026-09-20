#pragma once

#include <QWidget>

namespace core { class MihomoClient; }
namespace ui {
class ToggleSwitch;

class RoutingControls : public QWidget {
    Q_OBJECT
public:
    explicit RoutingControls(core::MihomoClient *client, QWidget *parent = nullptr);
    bool systemProxyEnabled() const;
    bool systemProxyAvailable() const;
    bool tunEnabled() const { return tunEnabled_; }
    bool tunAvailable() const;

public slots:
    void setSystemProxyState(bool enabled, bool available);
    void requestSystemProxyChange(bool enabled);
    void requestTunChange(bool enabled);
    void setTunEnableBlockedReason(const QString &reason);

signals:
    void stateChanged();
    void systemProxyRequested(bool enabled);
    void tunApplied(bool enabled);
    void errorOccurred(const QString &error);

private:
    void refreshTun();
    core::MihomoClient *client_;
    ToggleSwitch *systemProxy_;
    ToggleSwitch *tun_;
    bool configKnown_ = false;
    bool tunEnabled_ = false;
    bool tunPending_ = false;
    QString tunError_;
    QString tunEnableBlockedReason_;
};

}  // namespace ui
