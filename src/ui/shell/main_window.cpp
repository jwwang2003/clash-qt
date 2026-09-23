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
#include <QSystemTrayIcon>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>
#ifdef Q_OS_MACOS
#include <unistd.h>
#endif

#include "app/runtime/routing_controller.h"
#include "core/backend/backend_bridge.h"
#include "core/preferences/preferences.h"
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

namespace cb = core::backend;

constexpr int kNavWidth = 176;
constexpr int kGlyphRole = Qt::UserRole + 1;

QColor coreStateColor(cb::CoreState state) {
    const theme::Tokens &t = theme::tokens();
    switch (state) {
        case cb::CoreState::Running:
            return t.success;
        case cb::CoreState::Starting:
        case cb::CoreState::Stopping:
            return t.warning;
        case cb::CoreState::Failed:
            return t.danger;
        case cb::CoreState::Stopped:
            break;
        default:
            // The published enum reserves values a newer backend may send;
            // "unknown" is Failed's conservative neighbour, never "usable".
            break;
    }
    return t.textFaint;
}

}  // namespace

MainWindow::MainWindow(const app::Context &context, cb::BackendBridge *backend,
                       app::runtime::RoutingController *routing, QWidget *parent)
    : QMainWindow(parent), context_(context), backend_(backend), routing_(routing) {
    buildUi();

    connect(settingsPage_, &SettingsPage::serviceInstallationBusyChanged, this, [this](bool busy) {
        setProperty("serviceInstallationBusy", busy);
        updateCoreState(backend_->coreState());
    });

    connect(backend_, &cb::BackendBridge::versionReceived, this, [this](const QString &version) {
        versionLabel_->setText(tr("mihomo %1").arg(version));
    });
    connect(backend_, &cb::BackendBridge::connectedChanged, this, [this](bool connected) {
        modeBox_->setEnabled(connected);
        if (connected) statusBar()->clearMessage();
        if (!connected) {
            versionLabel_->setText(tr("controller offline"));
            trafficLabel_->setText("↑ —  ↓ —");
            memoryLabel_->setText(tr("mem —"));
        }
        updateCoreState(backend_->coreState());
    });
    // The CONFIRMED mode, not the requested one: an override is persisted only
    // once the controller reported the change actually took.
    connect(routing_, &app::runtime::RoutingController::modeConfirmed, this,
            [this](const QString &mode) {
        if (backend_->coreState() != cb::CoreState::Running) return;
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
    connect(backend_, &cb::BackendBridge::trafficSample, this, [this](quint64 up, quint64 down) {
        trafficLabel_->setText(QString("↑ %1  ↓ %2").arg(formatRate(up), formatRate(down)));
    });
    connect(backend_, &cb::BackendBridge::memorySample, this, [this](quint64 inuse, quint64) {
        memoryLabel_->setText(tr("mem %1").arg(formatBytes(inuse)));
    });
    // The box follows the one confirmed mode every routing surface reads.
    connect(routing_, &app::runtime::RoutingController::routingStateChanged, this, [this] {
        QSignalBlocker blocker(modeBox_);
        modeBox_->setCurrentIndex(modeBox_->findData(routing_->mode()));
    });
    connect(backend_, &cb::BackendBridge::errorOccurred, this, [this](const QString &message) {
        statusBar()->showMessage(message, 8000);
    });

    connect(backend_, &cb::BackendBridge::coreStateChanged, this, &MainWindow::updateCoreState);
    connect(backend_, &cb::BackendBridge::endpointChanged, this, [this] {
        updateCoreState(backend_->coreState());
    });
    connect(context_.profiles, &core::ProfileStore::runtimeBusyChanged, this, [this] {
        updateCoreState(backend_->coreState());
    });
    connect(backend_, &cb::BackendBridge::coreLogLine, logsPage_, &LogsPage::appendCoreLine);
    connect(backend_, &cb::BackendBridge::coreFailed, this, [this](const QString &reason) {
        coreLabel_->setToolTip(reason);
        logsPage_->appendCoreLine(reason);
        statusBar()->showMessage(reason, 15000);
    });
    connect(context_.hotkeys, &platform::Hotkeys::triggered, this, &MainWindow::onHotkey);
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
        applyNavIcons();
        updateCoreState(backend_->coreState());
    });
    updateCoreState(backend_->coreState());

    backend_->refreshRules();
    backend_->refreshConfig();
    backend_->openConnectionsStream();
    backend_->openLogStream(QStringLiteral("info"));
    backend_->openMemoryStream();
}

void MainWindow::buildUi() {
    theme::install();
    setWindowTitle("clash-qt");
    resize(1120, 680);
    setMinimumSize(860, 540);
    const QByteArray geometry = core::preferences::open().value("window/geometry").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);

    proxiesPage_ = new ProxiesPage(backend_, this);
    logsPage_ = new LogsPage(backend_, this);
    settingsPage_ = new SettingsPage(context_, backend_, routing_, this);
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
    addPage(theme::Glyph::Home, tr("Home"), new HomePage(backend_, this));
    addPage(theme::Glyph::Profiles, tr("Profiles"), new ProfilesPage(context_.profiles, this));
    addPage(theme::Glyph::Proxies, tr("Proxies"), proxiesPage_);
    addPage(theme::Glyph::Connections, tr("Connections"), new ConnectionsPage(backend_, this));
    addPage(theme::Glyph::Logs, tr("Logs"), logsPage_);
    addPage(theme::Glyph::Rules, tr("Rules"), new RulesPage(backend_, this));
    addPage(theme::Glyph::Providers, tr("Providers"), new ProvidersPage(backend_, this));
    addPage(theme::Glyph::Backups, tr("Backups"),
            new BackupPage(context_, *context_.backups, this));
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
    modeBox_->setEnabled(backend_->isConnected());
    modeBox_->setAccessibleName(tr("Routing mode"));
    modeBox_->addItem(tr("Rule"), "rule");
    modeBox_->addItem(tr("Global"), "global");
    modeBox_->addItem(tr("Direct"), "direct");
    connect(modeBox_, &QComboBox::currentIndexChanged, this,
            [this] { routing_->requestMode(modeBox_->currentData().toString()); });
    toolbar->addWidget(modeBox_);

    toolbar->addSeparator();
    routingControls_ = new RoutingControls(routing_, toolbar);
    toolbar->addWidget(routingControls_);
    // CONFIRMED, never requested: a read-back that disagreed with the request
    // must not be persisted as the new default.
    connect(routing_, &app::runtime::RoutingController::tunConfirmed, this, [this](bool enabled) {
        if (backend_->coreState() != cb::CoreState::Running) return;
        auto overrides = context_.profiles->runtimeOverrides();
        auto tun = overrides.value("tun").toObject();
        tun.insert("enable", enabled);
        overrides.insert("tun", tun);
        context_.profiles->setRuntimeOverrides(overrides);
    });
    // NOTE: RoutingController::errorOccurred is now the single routing error
    // channel and deliberately has no consumer yet - the composition root wires
    // it in the coordinator package. Neither RoutingControls nor SettingsPage
    // republishes a routing failure, so it can only ever be reported once.

    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    toolbar->addWidget(new DashboardButton(backend_, toolbar));
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
        routing_->toggleSystemProxy();
    } else if (id == hotkey::kCycleMode) {
        if (!modeBox_->isEnabled()) return;
        modeBox_->setCurrentIndex((modeBox_->currentIndex() + 1) % modeBox_->count());
    }
}

void MainWindow::toggleSystemProxy() { routing_->toggleSystemProxy(); }

void MainWindow::refreshRoutingState() {
    routing_->refreshSystemProxy();
    backend_->refreshConfig();
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
    backend_->stopCore();
}

void MainWindow::updateCoreState(cb::CoreState state) {
#ifdef Q_OS_MACOS
    // Only gate a known direct child; the service and external controllers
    // may have privileges the desktop process does not. "Is the attachment the
    // managed child?" is the backend's answer now, not an address comparison
    // this window performs by hand.
    const bool unprivilegedManaged = geteuid() != 0
        && !backend_->backend().usesPrivilegedService()
        && state == cb::CoreState::Running
        && backend_->endpointOwnership() == cb::Ownership::Managed;
    routingControls_->setTunEnableBlockedReason(unprivilegedManaged
        ? tr("This core is running without TUN privileges. Open Settings → Privileged Service to install and enable service mode, then restart the core. System Proxy remains available.")
        : QString());
#endif
    QString text;
    switch (state) {
        case cb::CoreState::Stopped:
            text = backend_->isConnected() ? tr("external core connected") : tr("core stopped");
            break;
        case cb::CoreState::Starting:
            text = tr("core starting…");
            break;
        case cb::CoreState::Stopping:
            text = tr("core stopping…");
            break;
        case cb::CoreState::Running:
            text = backend_->backend().usesPrivilegedService() ? tr("service core running")
                                                              : tr("core running");
            break;
        case cb::CoreState::Failed:
            text = tr("core failed");
            break;
        default:
            text = tr("core failed");
            break;
    }
    const bool preparing = context_.profiles->isRuntimeBusy();
    const bool installingService = property("serviceInstallationBusy").toBool();
    if (preparing) text = tr("preparing configuration…");
    coreLabel_->setText(
        QString("<b style=\"color:%1\">● %2</b>").arg(coreStateColor(state).name(), text));
    if (state != cb::CoreState::Failed) coreLabel_->setToolTip(QString());

    const bool live = state == cb::CoreState::Starting || state == cb::CoreState::Running;
    startCoreAction_->setEnabled(!live && !preparing && !installingService && state != cb::CoreState::Stopping);
    stopCoreAction_->setEnabled((live || preparing) && state != cb::CoreState::Stopping);
    restartCoreAction_->setEnabled(live && !preparing);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    core::preferences::open().setValue("window/geometry", saveGeometry());
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        event->accept();
        qApp->quit();
        return;
    }
    hide();
    event->ignore();
}

}  // namespace ui
