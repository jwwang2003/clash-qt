#pragma once

#include <QWidget>
#include <optional>

#include "app/app_context.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace platform { class SystemProxyService; }

namespace ui {

class SettingsSection;

class SettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(const app::Context &context, QWidget *parent = nullptr);
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
    void systemProxyError(const QString &error);
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
    platform::SystemProxyService *proxyService_;
    std::optional<bool> proxyRequest_;
    QString proxyOperationError_;
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
