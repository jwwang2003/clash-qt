#include "ui/shell/dashboard_button.h"

#include <QMenu>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QTimer>
#include <QMessageBox>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>

#include "core/backend/backend_bridge.h"
#include "core/preferences/preferences.h"
#include "core/types.h"
#include "platform/browser/browser_launcher.h"

namespace {

/// backend::Endpoint is deliberately behaviour-free (backend contract section 9),
/// so URL construction belongs to whoever owns the transport - here, the widget.
/// core::Endpoint already spells the same three fields and carries the tested
/// bracket/IPv6 handling, so the helper borrows it rather than re-deriving it.
QString httpBase(const core::backend::Endpoint &endpoint) {
    return core::Endpoint{endpoint.host, endpoint.port, endpoint.secret}.httpBase();
}

/// The bundled dashboards are hash-routed, so their router reads connection
/// details from the query that follows the fragment rather than the one before
/// it. Without `hostname` they fall through to an empty setup form.
QUrl dashboardUrl(const core::backend::Endpoint &endpoint) {
    QUrlQuery query;
    query.addQueryItem("hostname", endpoint.host);
    query.addQueryItem("port", QString::number(endpoint.port));
    if (!endpoint.secret.isEmpty()) query.addQueryItem("secret", endpoint.secret);

    QUrl url(httpBase(endpoint) + "/ui/");
    url.setFragment("/setup?" + query.toString(QUrl::FullyEncoded), QUrl::TolerantMode);
    return url;
}

}  // namespace

namespace ui {
namespace {

constexpr auto kBrowserKey = "dashboard/browser";

QSettings settings() { return core::preferences::open(); }

}  // namespace

DashboardButton::DashboardButton(core::backend::BackendBridge *backend, QWidget *parent)
    : DashboardButton(backend, platform::BrowserLauncher::operations(), parent) {}

DashboardButton::DashboardButton(core::backend::BackendBridge *backend,
                                 platform::BrowserOperations *browsers, QWidget *parent)
    : QToolButton(parent), backend_(backend),
      browserOps_(browsers ? browsers : platform::BrowserLauncher::operations()),
      menu_(new QMenu(this)) {
    setText(tr("Dashboard"));
    setPopupMode(QToolButton::MenuButtonPopup);
    setMenu(menu_);
    browserId_ = settings().value(kBrowserKey).toString();

    connect(this, &QToolButton::clicked, this, &DashboardButton::openDashboard);
    connect(menu_, &QMenu::aboutToShow, this, [this] {
        rebuildMenu();
        refreshBrowsers();
    });
    connect(backend_, &core::backend::BackendBridge::connectedChanged, this,
            &DashboardButton::applyConnectionState);
    connect(backend_, &core::backend::BackendBridge::endpointChanged, this, [this] {
        applyConnectionState(backend_->isConnected());
    });
    applyConnectionState(backend_->isConnected());
    QTimer::singleShot(0, this, &DashboardButton::refreshBrowsers);
}

void DashboardButton::openDashboard() {
    const core::backend::Endpoint endpoint = backend_->endpoint();
    if (!core::backend::isValid(endpoint)) return;
    if (opening_ || !backend_->isConnected()) return;
    opening_ = true;
    applyConnectionState(true);
    const QUrl url = dashboardUrl(endpoint);
    const QString browser = browserId_;
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
        const bool opened = watcher->result();
        watcher->deleteLater();
        opening_ = false;
        applyConnectionState(backend_->isConnected());
        if (!opened) {
            auto *message = new QMessageBox(QMessageBox::Warning, tr("Dashboard"),
                tr("No browser could open the dashboard."), QMessageBox::Ok, this);
            message->setAttribute(Qt::WA_DeleteOnClose);
            message->open();
        }
    });
    watcher->setFuture(QtConcurrent::run([browsers = browserOps_, url, browser] {
        return browsers->open(url, browser);
    }));
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
    if (browsersLoading_ && browsers_.isEmpty())
        menu_->addAction(tr("Finding browsers…"))->setEnabled(false);
    if (!browsers_.isEmpty()) menu_->addSeparator();

    for (const platform::Browser &browser : browsers_) {
        QAction *action = menu_->addAction(
            browser.isDefault ? tr("%1 (default)").arg(browser.name) : browser.name);
        action->setCheckable(true);
        action->setChecked(browser.id == browserId_);
        connect(action, &QAction::triggered, this,
                [this, id = browser.id] { chooseBrowser(id); });
    }
}

void DashboardButton::refreshBrowsers() {
    if (browsersLoading_) return;
    browsersLoading_ = true;
    auto *watcher = new QFutureWatcher<QVector<platform::Browser>>(this);
    connect(watcher, &QFutureWatcher<QVector<platform::Browser>>::finished, this, [this, watcher] {
        browsers_ = watcher->result();
        watcher->deleteLater();
        browsersLoading_ = false;
        // Updating only after discovery completes keeps opening the menu immediate.
        rebuildMenu();
    });
    watcher->setFuture(QtConcurrent::run([browsers = browserOps_] { return browsers->available(); }));
}

void DashboardButton::applyConnectionState(bool connected) {
    setEnabled(connected && !opening_);
    setText(opening_ ? tr("Opening…") : tr("Dashboard"));
    setToolTip(connected ? tr("Open %1/ui/ (requires a dashboard installed on the core)")
                              .arg(httpBase(backend_->endpoint()))
                         : tr("Connect to a core to open its dashboard."));
}

}  // namespace ui
