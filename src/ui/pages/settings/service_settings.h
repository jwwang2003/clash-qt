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
/// DECISION D3: this section used to open a SECOND platform::PrivilegedServiceClient
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
    // GAP (reported): core::backend::PrivilegedServiceStatus publishes the
    // SERVICE's state, version and error, but not the helper's own
    // `state == "running"` flag - "a core is already running under the
    // privileged service, possibly for another app session". The second
    // PrivilegedServiceClient was the only thing that ever set this, so until
    // the contract carries the flag it stays false and the guard it feeds is
    // inert. Left in place, rather than deleted, so re-wiring it is one line.
    bool serviceRunning_ = false;
};
}
