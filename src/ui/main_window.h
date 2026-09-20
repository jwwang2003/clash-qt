#pragma once

#include <QMainWindow>

#include "app_context.h"
#include "core/process/core_process.h"
#include "ui/theme.h"

class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QStackedWidget;

namespace core {
class MihomoClient;
class ProfileStore;
}

namespace ui {

class LogsPage;
class ProxiesPage;
class SettingsPage;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const app::Context &context, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void buildToolBar();
    void addPage(theme::Glyph glyph, const QString &title, QWidget *page);
    void applyNavIcons();
    void onHotkey(const QString &id);
    void startCore();
    void updateCoreState(core::CoreState state);

    app::Context context_;
    core::MihomoClient *client_;
    QListWidget *nav_;
    QStackedWidget *pages_;
    ProxiesPage *proxiesPage_;
    LogsPage *logsPage_;
    SettingsPage *settingsPage_;
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
