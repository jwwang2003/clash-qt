#pragma once

#include <QMainWindow>

#include "app/app_context.h"
#include "core/backend/lifecycle.h"   // core::backend::CoreState, the published enum
#include "ui/theme/theme.h"

class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QStackedWidget;

namespace app::runtime {
class RoutingController;
}

namespace core {
class ProfileStore;
}

namespace core::backend {
class BackendBridge;
}

namespace ui {

class LogsPage;
class ProxiesPage;
class SettingsPage;
class RoutingControls;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// `backend` and `routing` must outlive the window; neither is owned. They
    /// are injected rather than read off app::Context because the published Qt
    /// bridge and the routing coordinator belong to the composition root, which
    /// is also what keeps this header free of component-private includes.
    MainWindow(const app::Context &context, core::backend::BackendBridge *backend,
               app::runtime::RoutingController *routing, QWidget *parent = nullptr);
    const app::Context &context() const { return context_; }
    core::backend::BackendBridge *backend() const { return backend_; }
    app::runtime::RoutingController *routing() const { return routing_; }
    void startCore();
    void stopCore();
    void toggleSystemProxy();
    RoutingControls *routingControls() const { return routingControls_; }
    void refreshRoutingState();

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void buildUi();
    void buildToolBar();
    void addPage(theme::Glyph glyph, const QString &title, QWidget *page);
    void applyNavIcons();
    void onHotkey(const QString &id);
    void updateCoreState(core::backend::CoreState state);

    app::Context context_;
    core::backend::BackendBridge *backend_;
    app::runtime::RoutingController *routing_;
    QListWidget *nav_;
    QStackedWidget *pages_;
    ProxiesPage *proxiesPage_;
    LogsPage *logsPage_;
    SettingsPage *settingsPage_;
    RoutingControls *routingControls_ = nullptr;
    QComboBox *modeBox_;
    QAction *startCoreAction_;
    QAction *stopCoreAction_;
    QAction *restartCoreAction_;
    QLabel *coreLabel_;
    QLabel *versionLabel_;
    QLabel *trafficLabel_;
    QLabel *memoryLabel_;
};

}  // namespace ui
