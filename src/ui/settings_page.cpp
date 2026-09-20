#include "ui/settings_page.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include "core/enhance/config_enhancer.h"
#include "core/mihomo_client.h"
#include "platform/proxy/system_proxy.h"
#include "platform/system/autostart.h"
#include "platform/system/hotkeys.h"
#include "ui/chain_editor.h"
#include "ui/hotkey_settings.h"
#include "ui/settings_section.h"
#include "ui/theme.h"

namespace ui {
namespace {

constexpr auto kBypassKey = "sysproxy/bypass";
constexpr auto kDefaultBypass = "localhost, 127.0.0.1, ::1";

QSettings settings() { return QSettings("clash-qt", "clash-qt"); }

QString failureText(const QString &what, const QString &reason) {
    return reason.isEmpty() ? QObject::tr("%1 The platform reported no reason.").arg(what)
                            : QObject::tr("%1 %2").arg(what, reason);
}

}  // namespace

SettingsPage::SettingsPage(const app::Context &context, QWidget *parent)
    : QWidget(parent), context_(context) {
    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, theme::kPageSpacing, 0);
    column->setSpacing(theme::kPageSpacing);
    buildSystemProxy(column);
    buildStartup(column);
    buildHotkeys(column);
    buildChain(column);
    column->addStretch(1);

    auto *body = new QWidget;
    body->setLayout(column);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("settingsScroll");
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *layout = theme::pageLayout(this);
    layout->addWidget(scroll, 1);

    // The proxy has to point at whatever port the core actually listens on.
    connect(context_.client, &core::MihomoClient::configReceived, this,
            [this](const core::BaseConfig &config) {
                corePort_ = config.mixedPort != 0 ? config.mixedPort : config.httpPort;
                refreshSystemProxy();
            });
    refreshSystemProxy();
}

void SettingsPage::buildSystemProxy(QVBoxLayout *column) {
    proxySection_ = new SettingsSection(
        tr("System Proxy"),
        tr("Point the operating system's HTTP and HTTPS proxy at the running core."), this);

    proxyToggle_ = new QCheckBox(tr("Route system traffic through clash-qt"), proxySection_);
    connect(proxyToggle_, &QCheckBox::toggled, this, &SettingsPage::applySystemProxy);

    bypassEdit_ = new QLineEdit(settings().value(kBypassKey, kDefaultBypass).toString(),
                                proxySection_);
    bypassEdit_->setPlaceholderText(tr("Hosts that skip the proxy, comma separated"));
    connect(bypassEdit_, &QLineEdit::editingFinished, this, [this] {
        settings().setValue(kBypassKey, bypassEdit_->text());
        if (proxyToggle_->isChecked()) applySystemProxy(true);
    });

    auto *bypassLabel = new QLabel(tr("Bypass"), proxySection_);
    bypassLabel->setObjectName("fieldLabel");

    auto *bypassRow = new QHBoxLayout;
    bypassRow->setSpacing(theme::kPageSpacing);
    bypassRow->addWidget(bypassLabel);
    bypassRow->addWidget(bypassEdit_, 1);

    proxyState_ = new QLabel(proxySection_);
    proxyState_->setObjectName("fieldLabel");
    proxyState_->setWordWrap(true);

    proxySection_->addWidget(proxyToggle_);
    proxySection_->addLayout(bypassRow);
    proxySection_->addWidget(proxyState_);
    if (!platform::SystemProxy::isSupported()) {
        proxySection_->setNotice(
            tr("Changing the system proxy is not supported on this platform yet, so the switch "
               "is disabled."));
    }
    column->addWidget(proxySection_);
}

void SettingsPage::buildStartup(QVBoxLayout *column) {
    startupSection_ =
        new SettingsSection(tr("Startup"), tr("Start clash-qt when you log in."), this);

    startupToggle_ = new QCheckBox(tr("Launch at login"), startupSection_);
    startupToggle_->setChecked(platform::Autostart::isEnabled());
    startupToggle_->setEnabled(platform::Autostart::isSupported());
    connect(startupToggle_, &QCheckBox::toggled, this, [this](bool enabled) {
        startupSection_->setError(QString());
        if (platform::Autostart::setEnabled(enabled)) return;

        startupSection_->setError(failureText(tr("Could not change the login item."),
                                              platform::Autostart::lastError()));
        QSignalBlocker blocker(startupToggle_);
        startupToggle_->setChecked(platform::Autostart::isEnabled());
    });

    startupSection_->addWidget(startupToggle_);
    if (!platform::Autostart::isSupported()) {
        startupSection_->setNotice(
            tr("Registering a login item is not supported on this platform yet, so the switch "
               "is disabled."));
    }
    column->addWidget(startupSection_);
}

void SettingsPage::buildHotkeys(QVBoxLayout *column) {
    auto *section = new SettingsSection(
        tr("Hotkeys"),
        tr("System-wide shortcuts, in effect whether or not clash-qt has focus."), this);
    section->addWidget(new HotkeySettings(context_.hotkeys, section));
    if (!platform::Hotkeys::isSupported()) {
        section->setNotice(
            tr("System-wide hotkeys are not supported on this platform yet, so the fields are "
               "disabled."));
    }
    column->addWidget(section);
}

void SettingsPage::buildChain(QVBoxLayout *column) {
    auto *section = new SettingsSection(
        tr("Enhancement Chain"),
        tr("Merges and scripts applied to the active profile, in order. Double-click a step to "
           "open its file."),
        this);
    section->addWidget(new ChainEditor(context_.enhancer, section));
    connect(context_.enhancer, &core::ConfigEnhancer::errorOccurred, section,
            [section](const QString &message) { section->setError(message); });
    column->addWidget(section);
}

void SettingsPage::toggleSystemProxy() {
    if (proxyToggle_->isEnabled()) proxyToggle_->toggle();
}

void SettingsPage::applySystemProxy(bool enabled) {
    proxySection_->setError(QString());

    bool ok = false;
    if (enabled) {
        platform::ProxyConfig config;
        config.port = corePort_;
        config.bypass = bypassEdit_->text().trimmed();
        ok = platform::SystemProxy::enable(config);
    } else {
        ok = platform::SystemProxy::disable();
    }

    if (!ok) {
        proxySection_->setError(failureText(enabled ? tr("Could not set the system proxy.")
                                                    : tr("Could not clear the system proxy."),
                                            platform::SystemProxy::lastError()));
    }
    refreshSystemProxy();
}

void SettingsPage::refreshSystemProxy() {
    const bool supported = platform::SystemProxy::isSupported();
    const platform::ProxyConfig current = platform::SystemProxy::current();
    const bool on = platform::SystemProxy::isEnabled();

    QSignalBlocker blocker(proxyToggle_);
    proxyToggle_->setChecked(on);
    proxyToggle_->setEnabled(supported && corePort_ != 0);
    bypassEdit_->setEnabled(supported);

    const QString system = on ? tr("System proxy: %1:%2").arg(current.host).arg(current.port)
                              : tr("System proxy: off");
    const QString port = corePort_ != 0 ? tr("Core port: %1").arg(corePort_)
                                        : tr("Core port: not reported yet");
    proxyState_->setText(system + "   ·   " + port);
}

}  // namespace ui
