#include "ui/main_window.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>

#include "core/mihomo_client.h"
#include "core/profile/profile_store.h"
#include "platform/system/hotkeys.h"
#include "ui/connections_page.h"
#include "ui/dashboard_button.h"
#include "ui/formatting.h"
#include "ui/hotkey_settings.h"
#include "ui/logs_page.h"
#include "ui/profiles_page.h"
#include "ui/proxies_page.h"
#include "ui/rules_page.h"
#include "ui/settings_page.h"

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

    connect(client_, &core::MihomoClient::versionReceived, this, [this](const QString &version) {
        versionLabel_->setText(tr("mihomo %1").arg(version));
    });
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
    connect(context_.coreProcess, &core::CoreProcess::logLine, logsPage_, &LogsPage::appendCoreLine);
    connect(context_.coreProcess, &core::CoreProcess::failed, this, [this](const QString &reason) {
        coreLabel_->setToolTip(reason);
        logsPage_->appendCoreLine(reason);
        QMessageBox::warning(this, tr("Core Failed"),
                             tr("The mihomo core could not run:\n\n%1").arg(reason));
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

    proxiesPage_ = new ProxiesPage(client_, this);
    logsPage_ = new LogsPage(client_, this);
    settingsPage_ = new SettingsPage(context_, this);

    nav_ = new QListWidget(this);
    nav_->setObjectName("navList");
    nav_->setFixedWidth(kNavWidth);
    nav_->setIconSize(QSize(theme::kNavIconSize, theme::kNavIconSize));
    nav_->setUniformItemSizes(true);
    nav_->setFrameShape(QFrame::NoFrame);
    nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    pages_ = new QStackedWidget(this);
    // Profiles first: without one selected there is nothing for the other
    // pages to show.
    addPage(theme::Glyph::Profiles, tr("Profiles"), new ProfilesPage(context_.profiles, this));
    addPage(theme::Glyph::Proxies, tr("Proxies"), proxiesPage_);
    addPage(theme::Glyph::Connections, tr("Connections"), new ConnectionsPage(client_, this));
    addPage(theme::Glyph::Logs, tr("Logs"), logsPage_);
    addPage(theme::Glyph::Rules, tr("Rules"), new RulesPage(client_, this));
    addPage(theme::Glyph::Settings, tr("Settings"), settingsPage_);

    connect(nav_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    nav_->setCurrentRow(0);

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
    // Only what affects the whole app lives here: the core's lifecycle, the
    // routing mode and its dashboard. Everything else belongs to a page.
    auto *toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);

    startCoreAction_ = toolbar->addAction(tr("Start Core"), this, &MainWindow::startCore);
    stopCoreAction_ = toolbar->addAction(tr("Stop Core"), context_.coreProcess, &core::CoreProcess::stop);
    restartCoreAction_ =
        toolbar->addAction(tr("Restart Core"), context_.coreProcess, &core::CoreProcess::restart);
    toolbar->addSeparator();

    auto *modeLabel = new QLabel(tr("Mode"), toolbar);
    modeLabel->setObjectName("toolbarLabel");
    toolbar->addWidget(modeLabel);

    modeBox_ = new QComboBox(toolbar);
    modeBox_->addItem(tr("Rule"), "rule");
    modeBox_->addItem(tr("Global"), "global");
    modeBox_->addItem(tr("Direct"), "direct");
    connect(modeBox_, &QComboBox::currentIndexChanged, this,
            [this] { client_->patchMode(modeBox_->currentData().toString()); });
    toolbar->addWidget(modeBox_);

    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    toolbar->addWidget(new DashboardButton(context_.coreProcess, toolbar));
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
        modeBox_->setCurrentIndex((modeBox_->currentIndex() + 1) % modeBox_->count());
    }
}

void MainWindow::startCore() {
    if (context_.profiles->currentUid().isEmpty()) {
        QMessageBox::warning(this, tr("Start Core"),
                             tr("Select a profile on the Profiles page before starting the core."));
        return;
    }
    // runtimeConfigReady is what launches the core; generating the config is the whole action.
    context_.profiles->generateRuntimeConfig();
}

void MainWindow::updateCoreState(core::CoreState state) {
    QString text;
    switch (state) {
        case core::CoreState::Stopped:
            text = tr("core stopped");
            break;
        case core::CoreState::Starting:
            text = tr("core starting…");
            break;
        case core::CoreState::Stopping:
            text = tr("core stopping…");
            break;
        case core::CoreState::Running:
            text = tr("core running");
            break;
        case core::CoreState::Failed:
            text = tr("core failed");
            break;
    }
    coreLabel_->setText(
        QString("<b style=\"color:%1\">● %2</b>").arg(coreStateColor(state).name(), text));
    if (state != core::CoreState::Failed) coreLabel_->setToolTip(QString());

    const bool live = state == core::CoreState::Starting || state == core::CoreState::Running;
    startCoreAction_->setEnabled(!live && state != core::CoreState::Stopping);
    stopCoreAction_->setEnabled(live);
    restartCoreAction_->setEnabled(live);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // The app keeps running in the tray, so closing the window only hides it.
    hide();
    event->ignore();
}

}  // namespace ui
