#include "ui/shell/main_window.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include "ui/widgets/combo_box.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QShortcut>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>
#ifdef Q_OS_MACOS
#include <unistd.h>
#endif

#include "core/mihomo/mihomo_client.h"
#include "core/profiles/profile_store.h"
#include "platform/system/hotkeys.h"
#include "ui/pages/connections/connections_page.h"
#include "ui/shell/dashboard_button.h"
#include "ui/theme/formatting.h"
#include "ui/pages/settings/hotkey_settings.h"
#include "ui/pages/logs/logs_page.h"
#include "ui/pages/profiles/profiles_page.h"
#include "ui/pages/proxies/proxies_page.h"
#include "ui/pages/rules/rules_page.h"
#include "ui/pages/settings/settings_page.h"
#include "core/config/enhance/config_enhancer.h"
#include "ui/pages/providers/providers_page.h"
#include "ui/pages/overview/home_page.h"
#include "ui/pages/backups/backup_page.h"
#include "ui/shell/routing_controls.h"

namespace ui {
namespace {

constexpr int kNavWidth = 176;
constexpr int kGlyphRole = Qt::UserRole + 1;

QColor coreStateColor(core::CoreState state) {
    const theme::Tokens &t = theme::tokens();
    switch (state) {
        case core::CoreState::Running:
            return t.success;
        case core::CoreState::Starting:
        case core::CoreState::Stopping:
            return t.warning;
        case core::CoreState::Failed:
            return t.danger;
        case core::CoreState::Stopped:
            break;
    }
    return t.textFaint;
}

}  // namespace

MainWindow::MainWindow(const app::Context &context, QWidget *parent)
    : QMainWindow(parent), context_(context), client_(context.client) {
    buildUi();

    connect(settingsPage_, &SettingsPage::serviceInstallationBusyChanged, this, [this](bool busy) {
        setProperty("serviceInstallationBusy", busy);
        updateCoreState(context_.coreProcess->state());
    });

    connect(client_, &core::MihomoClient::versionReceived, this, [this](const QString &version) {
        versionLabel_->setText(tr("mihomo %1").arg(version));
    });
    connect(client_, &core::MihomoClient::connectedChanged, this, [this](bool connected) {
        modeBox_->setEnabled(connected);
        if (connected) statusBar()->clearMessage();
        if (!connected) {
            versionLabel_->setText(tr("controller offline"));
            trafficLabel_->setText("↑ —  ↓ —");
            memoryLabel_->setText(tr("mem —"));
        }
        updateCoreState(context_.coreProcess->state());
    });
    connect(client_, &core::MihomoClient::modeChanged, this, [this](const QString &mode) {
        if (context_.coreProcess->state() != core::CoreState::Running) return;
        auto overrides = context_.profiles->runtimeOverrides();
        overrides.insert("mode", mode);
        context_.profiles->setRuntimeOverrides(overrides);
    });
    connect(context_.profiles, &core::ProfileStore::enhancementLog, this,
            [this](const QStringList &lines) {
                for (const auto &line : lines) logsPage_->appendCoreLine(line);
            });
    connect(context_.profiles, &core::ProfileStore::errorOccurred, this,
            [this](const QString &message) { statusBar()->showMessage(message, 15000); });
    connect(client_, &core::MihomoClient::trafficSample, this, [this](quint64 up, quint64 down) {
        trafficLabel_->setText(QString("↑ %1  ↓ %2").arg(formatRate(up), formatRate(down)));
    });
    connect(client_, &core::MihomoClient::memorySample, this, [this](quint64 inuse, quint64) {
        memoryLabel_->setText(tr("mem %1").arg(formatBytes(inuse)));
    });
    connect(client_, &core::MihomoClient::configReceived, this,
            [this](const core::BaseConfig &config) {
                QSignalBlocker blocker(modeBox_);
                modeBox_->setCurrentIndex(modeBox_->findData(config.mode));
            });
    connect(client_, &core::MihomoClient::errorOccurred, this, [this](const QString &message) {
        statusBar()->showMessage(message, 8000);
    });

    connect(context_.coreProcess, &core::CoreProcess::stateChanged, this, &MainWindow::updateCoreState);
    connect(client_, &core::MihomoClient::endpointChanged, this, [this] {
        updateCoreState(context_.coreProcess->state());
    });
    connect(context_.profiles, &core::ProfileStore::runtimeBusyChanged, this, [this] {
        updateCoreState(context_.coreProcess->state());
    });
    connect(context_.coreProcess, &core::CoreProcess::logLine, logsPage_, &LogsPage::appendCoreLine);
    connect(context_.coreProcess, &core::CoreProcess::failed, this, [this](const QString &reason) {
        coreLabel_->setToolTip(reason);
        logsPage_->appendCoreLine(reason);
        statusBar()->showMessage(reason, 15000);
    });
    connect(context_.hotkeys, &platform::Hotkeys::triggered, this, &MainWindow::onHotkey);
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
        applyNavIcons();
        updateCoreState(context_.coreProcess->state());
    });
    updateCoreState(context_.coreProcess->state());

    client_->fetchRules();
    client_->fetchConfigs();
    client_->openConnectionsStream();
    client_->openLogStream();
    client_->openMemoryStream();
}

void MainWindow::buildUi() {
    theme::install();
    setWindowTitle("clash-qt");
    resize(1120, 680);
    setMinimumSize(860, 540);
    const QByteArray geometry = QSettings("clash-qt", "clash-qt").value("window/geometry").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);

    proxiesPage_ = new ProxiesPage(client_, this);
    logsPage_ = new LogsPage(client_, this);
    settingsPage_ = new SettingsPage(context_, this);
    auto *settingsShortcut = new QShortcut(QKeySequence::Preferences, this);
    connect(settingsShortcut, &QShortcut::activated, this, [this] {
        nav_->setCurrentRow(pages_->indexOf(settingsPage_));
    });

    nav_ = new QListWidget(this);
    nav_->setObjectName("navList");
    nav_->setAccessibleName(tr("Navigation"));
    nav_->setFixedWidth(kNavWidth);
    nav_->setIconSize(QSize(theme::kNavIconSize, theme::kNavIconSize));
    nav_->setUniformItemSizes(true);
    nav_->setFrameShape(QFrame::NoFrame);
    nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    pages_ = new QStackedWidget(this);
    addPage(theme::Glyph::Home, tr("Home"), new HomePage(client_, this));
    addPage(theme::Glyph::Profiles, tr("Profiles"), new ProfilesPage(context_.profiles, this));
    addPage(theme::Glyph::Proxies, tr("Proxies"), proxiesPage_);
    addPage(theme::Glyph::Connections, tr("Connections"), new ConnectionsPage(client_, this));
    addPage(theme::Glyph::Logs, tr("Logs"), logsPage_);
    addPage(theme::Glyph::Rules, tr("Rules"), new RulesPage(client_, this));
    addPage(theme::Glyph::Providers, tr("Providers"), new ProvidersPage(client_, this));
    addPage(theme::Glyph::Backups, tr("Backups"), new BackupPage(context_, this));
    addPage(theme::Glyph::Settings, tr("Settings"), settingsPage_);

    connect(nav_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    nav_->setCurrentRow(context_.profiles->currentUid().isEmpty() ? 1 : 0);

    for (int row = 0; row < nav_->count(); ++row) {
        auto *jump = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + row)), this);
        connect(jump, &QShortcut::activated, this, [this, row] { nav_->setCurrentRow(row); });
    }

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(nav_);
    layout->addWidget(pages_, 1);
    setCentralWidget(central);

    buildToolBar();

    coreLabel_ = new QLabel(this);
    versionLabel_ = new QLabel(tr("connecting…"), this);
    trafficLabel_ = new QLabel("↑ 0 B/s  ↓ 0 B/s", this);
    memoryLabel_ = new QLabel(tr("mem —"), this);
    statusBar()->addPermanentWidget(coreLabel_);
    statusBar()->addPermanentWidget(versionLabel_);
    statusBar()->addPermanentWidget(trafficLabel_);
    statusBar()->addPermanentWidget(memoryLabel_);
}

void MainWindow::buildToolBar() {
    // Global routing controls stay reachable from every page.
    auto *toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);

    startCoreAction_ = toolbar->addAction(tr("Start Core"), this, &MainWindow::startCore);
    stopCoreAction_ = toolbar->addAction(tr("Stop Core"), this, &MainWindow::stopCore);
    restartCoreAction_ = toolbar->addAction(tr("Restart Core"), this, &MainWindow::startCore);
    toolbar->addSeparator();

    auto *modeLabel = new QLabel(tr("Mode"), toolbar);
    modeLabel->setObjectName("toolbarLabel");
    toolbar->addWidget(modeLabel);

    modeBox_ = new ComboBox(toolbar);
    modeBox_->setEnabled(client_->isConnected());
    modeBox_->setAccessibleName(tr("Routing mode"));
    modeBox_->addItem(tr("Rule"), "rule");
    modeBox_->addItem(tr("Global"), "global");
    modeBox_->addItem(tr("Direct"), "direct");
    connect(modeBox_, &QComboBox::currentIndexChanged, this,
            [this] { client_->patchMode(modeBox_->currentData().toString()); });
    toolbar->addWidget(modeBox_);

    toolbar->addSeparator();
    routingControls_ = new RoutingControls(client_, toolbar);
    toolbar->addWidget(routingControls_);
    connect(routingControls_, &RoutingControls::systemProxyRequested,
            settingsPage_, &SettingsPage::setSystemProxyEnabled);
    connect(settingsPage_, &SettingsPage::systemProxyStateChanged,
            routingControls_, &RoutingControls::setSystemProxyState);
    routingControls_->setSystemProxyState(settingsPage_->systemProxyEnabled(),
                                         settingsPage_->systemProxyAvailable());
    connect(routingControls_, &RoutingControls::tunApplied, this, [this](bool enabled) {
        if (context_.coreProcess->state() != core::CoreState::Running) return;
        auto overrides = context_.profiles->runtimeOverrides();
        auto tun = overrides.value("tun").toObject();
        tun.insert("enable", enabled);
        overrides.insert("tun", tun);
        context_.profiles->setRuntimeOverrides(overrides);
    });
    const auto routingError = [this](const QString &error) {
        statusBar()->showMessage(error, 15000);
        auto *message = new QMessageBox(QMessageBox::Warning, tr("Routing Settings"),
                                        error, QMessageBox::Ok, this);
        message->setAttribute(Qt::WA_DeleteOnClose);
        message->open();
    };
    connect(routingControls_, &RoutingControls::errorOccurred, this, routingError);
    connect(settingsPage_, &SettingsPage::systemProxyError, this, routingError);

    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    toolbar->addWidget(new DashboardButton(client_, toolbar));
}

void MainWindow::addPage(theme::Glyph glyph, const QString &title, QWidget *page) {
    auto *item = new QListWidgetItem(theme::navIcon(glyph), title, nav_);
    item->setData(kGlyphRole, static_cast<int>(glyph));
    pages_->addWidget(page);
}

void MainWindow::applyNavIcons() {
    for (int row = 0; row < nav_->count(); ++row) {
        QListWidgetItem *item = nav_->item(row);
        item->setIcon(theme::navIcon(static_cast<theme::Glyph>(item->data(kGlyphRole).toInt())));
    }
}

void MainWindow::onHotkey(const QString &id) {
    if (id == hotkey::kToggleWindow) {
        if (isVisible()) {
            hide();
            return;
        }
        show();
        raise();
        activateWindow();
    } else if (id == hotkey::kToggleProxy) {
        settingsPage_->toggleSystemProxy();
    } else if (id == hotkey::kCycleMode) {
        if (!modeBox_->isEnabled()) return;
        modeBox_->setCurrentIndex((modeBox_->currentIndex() + 1) % modeBox_->count());
    }
}

void MainWindow::toggleSystemProxy() { settingsPage_->toggleSystemProxy(); }

void MainWindow::refreshRoutingState() {
    settingsPage_->refreshSystemProxy();
    client_->fetchConfigs();
}

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && routingControls_)
        refreshRoutingState();
}

void MainWindow::startCore() {
    if (property("serviceInstallationBusy").toBool()) return;
    if (context_.profiles->currentUid().isEmpty()) {
        QMessageBox::warning(this, tr("Start Core"),
                             tr("Select a profile on the Profiles page before starting the core."));
        return;
    }
    // runtimeConfigReady is what launches the core; generating the config is the whole action.
    context_.profiles->requestRuntimeConfig();
}

void MainWindow::stopCore() {
    context_.profiles->cancelRuntimeGeneration();
    context_.coreProcess->stop();
}

void MainWindow::updateCoreState(core::CoreState state) {
#ifdef Q_OS_MACOS
    // Only gate a known direct child; the service and external controllers
    // may have privileges the desktop process does not.
    const auto managed = context_.coreProcess->endpoint();
    const auto connected = client_->endpoint();
    const bool unprivilegedManaged = geteuid() != 0 && !context_.coreProcess->usesPrivilegedService()
        && state == core::CoreState::Running
        && managed.isValid() && managed.host == connected.host && managed.port == connected.port;
    routingControls_->setTunEnableBlockedReason(unprivilegedManaged
        ? tr("This core is running without TUN privileges. Open Settings → Privileged Service to install and enable service mode, then restart the core. System Proxy remains available.")
        : QString());
#endif
    QString text;
    switch (state) {
        case core::CoreState::Stopped:
            text = client_->isConnected() ? tr("external core connected") : tr("core stopped");
            break;
        case core::CoreState::Starting:
            text = tr("core starting…");
            break;
        case core::CoreState::Stopping:
            text = tr("core stopping…");
            break;
        case core::CoreState::Running:
            text = context_.coreProcess->usesPrivilegedService() ? tr("service core running") : tr("core running");
            break;
        case core::CoreState::Failed:
            text = tr("core failed");
            break;
    }
    const bool preparing = context_.profiles->isRuntimeBusy();
    const bool installingService = property("serviceInstallationBusy").toBool();
    if (preparing) text = tr("preparing configuration…");
    coreLabel_->setText(
        QString("<b style=\"color:%1\">● %2</b>").arg(coreStateColor(state).name(), text));
    if (state != core::CoreState::Failed) coreLabel_->setToolTip(QString());

    const bool live = state == core::CoreState::Starting || state == core::CoreState::Running;
    startCoreAction_->setEnabled(!live && !preparing && !installingService && state != core::CoreState::Stopping);
    stopCoreAction_->setEnabled((live || preparing) && state != core::CoreState::Stopping);
    restartCoreAction_->setEnabled(live && !preparing);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    QSettings("clash-qt", "clash-qt").setValue("window/geometry", saveGeometry());
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        event->accept();
        qApp->quit();
        return;
    }
    hide();
    event->ignore();
}

}  // namespace ui
