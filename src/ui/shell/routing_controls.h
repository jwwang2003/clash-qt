#pragma once

#include <QString>
#include <QWidget>

namespace app::runtime { class RoutingController; }

namespace ui {
class ToggleSwitch;

/// The toolbar's two routing switches: a VIEW of app::runtime::RoutingController
/// and the intent that goes back into it.
///
/// It keeps no routing state of its own. Everything it shows is the
/// controller's CONFIRMED answer, so a change still in flight renders as
/// pending and never as applied. The controller also owns the error channel;
/// this widget reads lastError() for the switch's tooltip and deliberately does
/// not republish it, so one failure is reported once.
class RoutingControls : public QWidget {
    Q_OBJECT
public:
    explicit RoutingControls(app::runtime::RoutingController *routing, QWidget *parent = nullptr);
    bool systemProxyEnabled() const;
    bool systemProxyAvailable() const;
    bool tunEnabled() const;
    bool tunAvailable() const;

public slots:
    void requestSystemProxyChange(bool enabled);
    void requestTunChange(bool enabled);
    void setTunEnableBlockedReason(const QString &reason);
    /// Re-renders both switches from the controller.
    void refresh();

signals:
    void stateChanged();

private:
    app::runtime::RoutingController *routing_;
    ToggleSwitch *systemProxy_;
    ToggleSwitch *tun_;
};

}  // namespace ui
