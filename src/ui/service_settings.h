#pragma once
#include "ui/settings_section.h"
#include "app_context.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;
namespace platform { class PrivilegedServiceInstaller; class PrivilegedServiceClient; }
namespace ui {
class ServiceSettings : public SettingsSection {
    Q_OBJECT
public:
    explicit ServiceSettings(const app::Context &context, QWidget *parent = nullptr);
signals:
    void installationBusyChanged(bool busy);
private:
    void refresh();
    void checkStatus();
    bool applyMode(bool enabled);
    app::Context context_;
    platform::PrivilegedServiceInstaller *installer_;
    platform::PrivilegedServiceClient *probe_;
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
    bool serviceRunning_ = false;
};
}
