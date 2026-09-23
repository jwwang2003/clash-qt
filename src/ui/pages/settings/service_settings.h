#pragma once
#include "ui/widgets/settings_section.h"
#include "app/app_context.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;
namespace platform { class PrivilegedServiceInstaller; }
namespace core::backend { class BackendBridge; }
namespace ui {
/// This section used to open a SECOND platform::PrivilegedServiceClient
/// beside the one the backend already owns - two live connections to one
/// privileged socket. Status now arrives on the published
/// BackendBridge::privilegedServiceStatus channel, so the component keeps the
/// single connection and this page only asks and renders.
class ServiceSettings : public SettingsSection {
    Q_OBJECT
public:
    /// `backend` must outlive the section; it is not owned.
    ServiceSettings(const app::Context &context, core::backend::BackendBridge *backend,
                    QWidget *parent = nullptr);
    /// True when Install/Repair, Remove and the mode checkbox may act: the
    /// platform supports a helper, this app's core is stopped, no core is
    /// running under the helper for ANY session, no runtime work is in flight
    /// and the installer is idle.
    ///
    /// Public because it is a SAFETY guard, and a guard nothing asserts is a
    /// guard that can go inert unnoticed - which is exactly what happened when
    /// dropping the second client removed the only producer of the
    /// running-core flag. A test
    /// reads this rather than clicking Remove, because driving the real click
    /// path on a machine with an installed helper would uninstall it.
    bool canChangeInstallation() const;

signals:
    void installationBusyChanged(bool busy);
private:
    void refresh();
    void checkStatus();
    bool applyMode(bool enabled);
    app::Context context_;
    core::backend::BackendBridge *backend_;
    platform::PrivilegedServiceInstaller *installer_;
    QCheckBox *enabled_;
    QLabel *status_;
    QPushButton *install_;
    QPushButton *remove_;
    QPushButton *check_;
    QTimer *startupRetry_;
    int startupRetries_ = 0;
    bool installed_ = false;
    bool checked_ = false;
    bool reachable_ = false;
    // "A core is already running under the privileged service, possibly for
    // another app session." Written ONLY from an answered status query, which
    // carries core::backend::PrivilegedServiceStatus::coreRunning - the
    // helper's own report, published on the contract precisely so this page
    // does not need a second PrivilegedServiceClient of its own. An
    // unanswered query leaves it alone: not hearing back is not
    // evidence that nothing is running.
    bool serviceRunning_ = false;
};
}
