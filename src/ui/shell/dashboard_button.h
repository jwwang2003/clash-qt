#pragma once

#include <QToolButton>

#include "core/mihomo/mihomo_client.h"
#include "platform/browser/browser_launcher.h"

class QMenu;

namespace ui {

/// Opens the connected controller's web dashboard.
class DashboardButton : public QToolButton {
    Q_OBJECT

public:
    explicit DashboardButton(core::MihomoClient *client, QWidget *parent = nullptr);
    /// `browsers` must outlive the discovery/open work this button starts, which
    /// continues on a worker after the button is gone. Production passes
    /// platform::BrowserLauncher::operations(), which lives for the process.
    DashboardButton(core::MihomoClient *client, platform::BrowserOperations *browsers,
                    QWidget *parent = nullptr);

private:
    void openDashboard();
    void chooseBrowser(const QString &browserId);
    void rebuildMenu();
    void refreshBrowsers();
    void applyConnectionState(bool connected);

    core::MihomoClient *client_;
    platform::BrowserOperations *browserOps_;
    QMenu *menu_;
    QString browserId_;
    QVector<platform::Browser> browsers_;
    bool browsersLoading_ = false;
    bool opening_ = false;
};

}  // namespace ui
