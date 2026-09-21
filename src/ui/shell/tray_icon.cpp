#include "ui/shell/tray_icon.h"

#include <QApplication>
#include <QActionGroup>
#include <QClipboard>
#include <QMenu>
#include <QPainter>
#include <QPixmap>

#include "ui/theme/formatting.h"
#include "ui/shell/main_window.h"
#include "ui/shell/proxy_environment.h"
#include "ui/shell/tray_proxy_menu.h"
#include "ui/shell/routing_controls.h"
#include "core/mihomo/mihomo_client.h"
#include "core/profiles/profile_store.h"
#include "platform/proxy/system_proxy.h"

namespace ui {
namespace {

QIcon trayIcon() {
    const qreal scale = qApp->devicePixelRatio();
    QPixmap pixmap(QSize(22, 22) * scale);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(qApp->palette().color(QPalette::WindowText), 2));
    painter.drawEllipse(3, 3, 16, 16);
    painter.drawLine(7, 11, 15, 11);
    QIcon icon(pixmap);
    icon.setIsMask(true);  // macOS tints template icons for light/dark menu bars.
    return icon;
}

}  // namespace

TrayIcon::TrayIcon(MainWindow *window, QObject *parent)
    : QSystemTrayIcon(trayIcon(), parent), window_(window), menu_(new QMenu) {
    menu_->addAction(QObject::tr("Show Window"), window_, [this] {
        window_->show();
        window_->raise();
        window_->activateWindow();
    });
    const auto context = window->context();
    auto *start = menu_->addAction(tr("Start Core"), window_, &MainWindow::startCore);
    auto *stop = menu_->addAction(tr("Stop Core"), window_, &MainWindow::stopCore);
    auto *restart = menu_->addAction(tr("Restart Core"), window_, &MainWindow::startCore);
    const auto updateState = [=](core::CoreState state) {
        const bool live = state == core::CoreState::Starting || state == core::CoreState::Running;
        const bool preparing = context.profiles->isRuntimeBusy();
        start->setEnabled(!live && !preparing && state != core::CoreState::Stopping);
        stop->setEnabled((live || preparing) && state != core::CoreState::Stopping);
        restart->setEnabled(live && !preparing);
    };
    connect(context.coreProcess, &core::CoreProcess::stateChanged, this, updateState);
    connect(context.profiles, &core::ProfileStore::runtimeBusyChanged, this, [context, updateState] {
        updateState(context.coreProcess->state());
    });
    updateState(context.coreProcess->state());
    menu_->addSeparator();
    auto *modeMenu = menu_->addMenu(tr("Routing Mode"));
    auto *modes = new QActionGroup(this);
    modes->setExclusive(true);
    for (const auto &mode : {QString("rule"), QString("global"), QString("direct")}) {
        QString label = mode;
        label[0] = label[0].toUpper();
        auto *action = modeMenu->addAction(label);
        action->setData(mode);
        action->setCheckable(true);
        modes->addAction(action);
        connect(action, &QAction::triggered, context.client, [context, mode] { context.client->patchMode(mode); });
    }
    modeMenu->setEnabled(context.client->isConnected());
    connect(context.client, &core::MihomoClient::connectedChanged, modeMenu, &QMenu::setEnabled);
    connect(context.client, &core::MihomoClient::configReceived, this, [modes](const core::BaseConfig &config) {
        for (auto *action : modes->actions()) action->setChecked(action->data() == config.mode);
    });
    menu_->addMenu(new TrayProxyMenu(context.client, menu_));
    auto *profiles = menu_->addMenu(tr("Profiles"));
    connect(profiles, &QMenu::aboutToShow, this, [context, profiles] {
        profiles->clear();
        for (const auto &profile : context.profiles->profiles()) {
            auto *action = profiles->addAction(QString(profile.name).replace('&', "&&"));
            action->setCheckable(true);
            action->setChecked(profile.uid == context.profiles->currentUid());
            connect(action, &QAction::triggered, context.profiles,
                    [context, uid = profile.uid] { context.profiles->selectProfile(uid); });
        }
        if (profiles->isEmpty()) profiles->addAction(tr("No profiles"))->setEnabled(false);
    });
    auto *routing = window_->routingControls();
    auto *proxy = menu_->addAction(tr("System Proxy"));
    auto *tun = menu_->addAction(tr("TUN Mode"));
    proxy->setCheckable(true);
    tun->setCheckable(true);
    connect(proxy, &QAction::triggered, routing, &RoutingControls::requestSystemProxyChange);
    connect(tun, &QAction::triggered, routing, &RoutingControls::requestTunChange);
    const auto updateRouting = [routing, proxy, tun] {
        proxy->setChecked(routing->systemProxyEnabled());
        proxy->setEnabled(routing->systemProxyAvailable());
        tun->setChecked(routing->tunEnabled());
        tun->setEnabled(routing->tunAvailable());
    };
    connect(routing, &RoutingControls::stateChanged, this, updateRouting);
    connect(menu_, &QMenu::aboutToShow, window_, &MainWindow::refreshRoutingState);
    updateRouting();
    auto *shellMenu = menu_->addMenu(tr("Copy Shell Commands"));
    shellMenu->setToolTip(tr("Copy proxy environment variables for the connected core. No commands are executed."));
    auto *posix = shellMenu->addAction(tr("POSIX (bash / zsh)"));
    auto *powershell = shellMenu->addAction(tr("PowerShell"));
    shellMenu->setEnabled(false);
    connect(context.client, &core::MihomoClient::configReceived, this,
            [context, shellMenu, posix, powershell](const core::BaseConfig &config) {
        posix->setData(proxyEnvironment(context.client->endpoint(), config, Shell::Posix));
        powershell->setData(proxyEnvironment(context.client->endpoint(), config, Shell::PowerShell));
        shellMenu->setEnabled(context.client->isConnected() && !posix->data().toString().isEmpty());
    });
    connect(context.client, &core::MihomoClient::endpointChanged, this, [shellMenu, posix, powershell] {
        posix->setData(QString());
        powershell->setData(QString());
        shellMenu->setEnabled(false);
    });
    connect(context.client, &core::MihomoClient::connectedChanged, this,
            [shellMenu, posix, powershell](bool connected) {
        if (!connected) {
            posix->setData(QString());
            powershell->setData(QString());
        }
        shellMenu->setEnabled(connected && !posix->data().toString().isEmpty());
    });
    for (auto *action : {posix, powershell})
        connect(action, &QAction::triggered, this, [action] {
            if (!action->data().toString().isEmpty())
                QGuiApplication::clipboard()->setText(action->data().toString());
        });
    menu_->addSeparator();
    menu_->addAction(QObject::tr("Quit"), qApp, &QApplication::quit);

    setContextMenu(menu_);
    setToolTip("clash-qt");

    connect(this, &QSystemTrayIcon::activated, this, [this](ActivationReason reason) {
        if (reason != Trigger && reason != DoubleClick) return;
        if (window_->isVisible()) {
            window_->hide();
        } else {
            window_->show();
            window_->raise();
            window_->activateWindow();
        }
    });
}

TrayIcon::~TrayIcon() { delete menu_; }

void TrayIcon::setTraffic(quint64 up, quint64 down) {
    setToolTip(QString("clash-qt\n↑ %1\n↓ %2").arg(formatRate(up), formatRate(down)));
}

}  // namespace ui
