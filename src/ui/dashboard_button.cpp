#include "ui/dashboard_button.h"

#include <QMenu>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>

#include "platform/browser_launcher.h"

namespace {

/// The bundled dashboards are hash-routed, so their router reads connection
/// details from the query that follows the fragment rather than the one before
/// it. Without `hostname` they fall through to an empty setup form.
QUrl dashboardUrl(const core::Endpoint &endpoint) {
    QUrlQuery query;
    query.addQueryItem("hostname", endpoint.host);
    query.addQueryItem("port", QString::number(endpoint.port));
    if (!endpoint.secret.isEmpty()) query.addQueryItem("secret", endpoint.secret);

    QUrl url(endpoint.httpBase() + "/ui/");
    url.setFragment("/setup?" + query.toString(QUrl::FullyEncoded), QUrl::TolerantMode);
    return url;
}

}  // namespace

namespace ui {
namespace {

constexpr auto kBrowserKey = "dashboard/browser";

QSettings settings() { return QSettings("clash-qt", "clash-qt"); }

}  // namespace

DashboardButton::DashboardButton(core::CoreProcess *coreProcess, QWidget *parent)
    : QToolButton(parent), coreProcess_(coreProcess), menu_(new QMenu(this)) {
    setText(tr("Dashboard"));
    setPopupMode(QToolButton::MenuButtonPopup);
    setMenu(menu_);
    browserId_ = settings().value(kBrowserKey).toString();

    connect(this, &QToolButton::clicked, this, &DashboardButton::openDashboard);
    connect(menu_, &QMenu::aboutToShow, this, &DashboardButton::rebuildMenu);
    connect(coreProcess_, &core::CoreProcess::stateChanged, this,
            &DashboardButton::applyCoreState);
    applyCoreState(coreProcess_->state());
}

void DashboardButton::openDashboard() {
    const core::Endpoint endpoint = coreProcess_->endpoint();
    if (!endpoint.isValid()) return;
    platform::BrowserLauncher::open(dashboardUrl(endpoint), browserId_);
}

void DashboardButton::chooseBrowser(const QString &browserId) {
    browserId_ = browserId;
    settings().setValue(kBrowserKey, browserId);
    openDashboard();
}

void DashboardButton::rebuildMenu() {
    menu_->clear();

    QAction *systemAction = menu_->addAction(tr("System Default Browser"));
    systemAction->setCheckable(true);
    systemAction->setChecked(browserId_.isEmpty());
    connect(systemAction, &QAction::triggered, this, [this] { chooseBrowser(QString()); });

    // Enumeration can come back empty; the system-default entry then stands
    // alone rather than the menu opening blank.
    const QVector<platform::Browser> browsers = platform::BrowserLauncher::available();
    if (!browsers.isEmpty()) menu_->addSeparator();

    for (const platform::Browser &browser : browsers) {
        QAction *action = menu_->addAction(
            browser.isDefault ? tr("%1 (default)").arg(browser.name) : browser.name);
        action->setCheckable(true);
        action->setChecked(browser.id == browserId_);
        connect(action, &QAction::triggered, this,
                [this, id = browser.id] { chooseBrowser(id); });
    }
}

void DashboardButton::applyCoreState(core::CoreState state) {
    const bool running = state == core::CoreState::Running;
    setEnabled(running);
    setToolTip(running ? tr("Open %1/ui/").arg(coreProcess_->endpoint().httpBase())
                       : tr("The web dashboard is served by a core started from here. A core "
                            "running outside this app does not serve /ui."));
}

}  // namespace ui
