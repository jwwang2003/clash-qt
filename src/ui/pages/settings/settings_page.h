#pragma once

#include <QWidget>
#include <optional>

#include "app/app_context.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace app::runtime { class RoutingController; }
namespace core::backend { class BackendBridge; }
namespace platform { class SystemProxyService; }

namespace ui {

class SettingsSection;

class SettingsPage : public QWidget {
    Q_OBJECT

public:
    /// `backend` and `routing` must outlive the page; neither is owned.
    ///
    /// Every system-proxy DECISION - is it on, may it change, is a change in
    /// flight - is app::runtime::RoutingController's confirmed answer, shared
    /// with the toolbar and the tray. The platform service is still read for
    /// the descriptive status line only (the OS-held host:port, whether the
    /// read is valid, the platform's own error text and "restoring"), because
    /// the controller publishes none of those; it is the same instance the
    /// controller was constructed with, so the two cannot disagree.
    SettingsPage(const app::Context &context, core::backend::BackendBridge *backend,
                 app::runtime::RoutingController *routing, QWidget *parent = nullptr);
    bool systemProxyEnabled() const;
    bool systemProxyAvailable() const;

public slots:
    /// Flips the system proxy; also where the "toggle system proxy" hotkey lands.
    void toggleSystemProxy();
    void setSystemProxyEnabled(bool enabled);
    void refreshSystemProxy();
    void restoreSystemProxy();

signals:
    void systemProxyStateChanged(bool enabled, bool available);
    // NOTE: systemProxyError is gone. RoutingController::errorOccurred is the
    // single routing error channel now; republishing here would report one
    // failure twice once the composition root wires that channel up.
    void systemProxyBusyChanged(bool busy);
    void serviceInstallationBusyChanged(bool busy);

private:
    void buildSystemProxy(QVBoxLayout *column);
    void buildCore(QVBoxLayout *column);
    void buildRuntime(QVBoxLayout *column);
    void buildStartup(QVBoxLayout *column);
    void buildHotkeys(QVBoxLayout *column);
    void buildChain(QVBoxLayout *column);
    void applySystemProxy(bool enabled);
    void renderSystemProxy();
    void runAutostartOperation(std::optional<bool> enabled);
    void renderAutostart();

    app::Context context_;
    core::backend::BackendBridge *backend_;
    app::runtime::RoutingController *routing_;
    platform::SystemProxyService *proxyService_;
    std::optional<bool> proxyRequest_;
    bool startupBusy_ = false;
    bool startupKnown_ = false;
    bool startupEnabled_ = false;
    SettingsSection *proxySection_;
    SettingsSection *startupSection_;
    QCheckBox *proxyToggle_;
    QCheckBox *startupToggle_;
    QLineEdit *bypassEdit_;
    QLabel *proxyState_;
    quint16 corePort_ = 0;
    quint16 coreSocksPort_ = 0;
};

}  // namespace ui
