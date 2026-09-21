#include "ui/pages/settings/settings_page.h"
#include "ui/pages/settings/service_settings.h"

#include <memory>

#include <QCheckBox>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QApplication>
#include <QComboBox>
#include "ui/widgets/combo_box.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include "app/runtime/routing_controller.h"
#include "core/backend/backend_bridge.h"
#include "core/config/enhance/config_enhancer.h"
#include "core/preferences/preferences.h"
#include "core/types.h"
#include "core/profiles/profile_store.h"
#include "platform/proxy/system_proxy.h"
#include "platform/proxy/system_proxy_service.h"
#include "platform/system/autostart.h"
#include "platform/system/hotkeys.h"
#include "ui/pages/settings/chain_editor.h"
#include "ui/pages/settings/hotkey_settings.h"
#include "ui/widgets/settings_section.h"
#include "ui/theme/theme.h"

namespace ui {
namespace {

namespace cb = core::backend;

/// backend::Endpoint carries no behaviour by contract; core::Endpoint spells the
/// same fields and owns the tested URL construction, so the page converts here
/// rather than re-deriving it.
QString httpBase(const cb::Endpoint &endpoint) {
    return core::Endpoint{endpoint.host, endpoint.port, endpoint.secret}.httpBase();
}

constexpr auto kBypassKey = "sysproxy/bypass";
constexpr auto kDefaultBypass = "localhost, 127.0.0.1, ::1";

struct AutostartResult { bool enabled = false; bool valid = false; bool success = true; QString error; };

QSettings settings() { return core::preferences::open(); }

}  // namespace

SettingsPage::SettingsPage(const app::Context &context, cb::BackendBridge *backend,
                           app::runtime::RoutingController *routing, QWidget *parent)
    : QWidget(parent), context_(context), backend_(backend), routing_(routing),
      proxyService_(platform::SystemProxyService::instance()) {
    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, theme::kPageSpacing, 0);
    column->setSpacing(theme::kPageSpacing);
    buildSystemProxy(column);
    buildCore(column);
    auto *service = new ServiceSettings(context_, backend_, this);
    connect(service, &ServiceSettings::installationBusyChanged, this, &SettingsPage::serviceInstallationBusyChanged);
    column->addWidget(service);
    buildRuntime(column);
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

    // One notification for every routing change - the controller already folds
    // the service's stateChanged, busyChanged and activityChanged into it, so
    // subscribing to those as well would only re-render twice.
    connect(routing_, &app::runtime::RoutingController::routingStateChanged, this,
            &SettingsPage::renderSystemProxy);
    // Readback and re-targeting use the cache; no OS process runs in this slot.
    connect(backend_, &cb::BackendBridge::configReceived, this, [this](const cb::BaseConfig &config) {
        corePort_ = config.mixedPort != 0 ? config.mixedPort : config.httpPort;
        coreSocksPort_ = config.mixedPort != 0 ? config.mixedPort : config.socksPort;
        const auto &state = proxyService_->state();
        const bool targetChanged = state.config.host != backend_->endpoint().host ||
            state.config.port != corePort_ || state.config.socksPort != coreSocksPort_;
        if (state.valid && state.owned && targetChanged && backend_->isConnected() && corePort_ != 0)
            applySystemProxy(true);
        else refreshSystemProxy();
    });
    connect(backend_, &cb::BackendBridge::connectedChanged, this,
            [this](bool) { refreshSystemProxy(); });
    refreshSystemProxy();
}

void SettingsPage::buildSystemProxy(QVBoxLayout *column) {
    proxySection_ = new SettingsSection(
        tr("System Proxy"),
        tr("Point the operating system's HTTP and HTTPS proxy at the running core."), this);

    proxyToggle_ = new QCheckBox(tr("Use the connected core as the system proxy"), proxySection_);
    proxyToggle_->setEnabled(false);
    connect(proxyToggle_, &QCheckBox::toggled, this, &SettingsPage::applySystemProxy);

    bypassEdit_ = new QLineEdit(settings().value(kBypassKey, kDefaultBypass).toString(),
                                proxySection_);
    bypassEdit_->setPlaceholderText(tr("Hosts that skip the proxy, comma separated"));
    connect(bypassEdit_, &QLineEdit::editingFinished, this, [this] {
        settings().setValue(kBypassKey, bypassEdit_->text());
        if (proxyToggle_->isChecked() && !routing_->systemProxyPending()) applySystemProxy(true);
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
    startupToggle_->setEnabled(false);
    connect(startupToggle_, &QCheckBox::toggled, this,
            [this](bool enabled) { runAutostartOperation(enabled); });
    startupSection_->addWidget(startupToggle_);
    auto *startCore = new QCheckBox(tr("Start the selected profile when clash-qt opens"), startupSection_);
    startCore->setChecked(settings().value("startup/startCore", false).toBool());
    connect(startCore, &QCheckBox::toggled, this, [](bool enabled) {
        settings().setValue("startup/startCore", enabled);
    });
    startupSection_->addWidget(startCore);
    if (!platform::Autostart::isSupported()) {
        startupSection_->setNotice(
            tr("Registering a login item is not supported on this platform yet, so the switch "
               "is disabled."));
    }
    column->addWidget(startupSection_);
    runAutostartOperation(std::nullopt);
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

void SettingsPage::toggleSystemProxy() { routing_->toggleSystemProxy(); }

bool SettingsPage::systemProxyEnabled() const { return routing_->systemProxyEnabled(); }
bool SettingsPage::systemProxyAvailable() const { return routing_->systemProxyAvailable(); }

void SettingsPage::setSystemProxyEnabled(bool enabled) {
    if (routing_->systemProxyAvailable()) routing_->requestSystemProxy(enabled);
    renderSystemProxy();
}

void SettingsPage::applySystemProxy(bool enabled) {
    // The controller owns every guard the page used to apply by hand - shutting
    // down, not connected, no reported port - and raises the one error for it.
    proxyRequest_ = enabled;
    platform::ProxyConfig config;
    config.host = backend_->endpoint().host;
    config.port = corePort_;
    config.socksPort = coreSocksPort_;
    config.bypass = bypassEdit_->text().trimmed();
    routing_->setProxyTarget(config);
    routing_->requestSystemProxy(enabled);
    renderSystemProxy();
}

void SettingsPage::refreshSystemProxy() {
    renderSystemProxy();
    routing_->refreshSystemProxy();
}

void SettingsPage::restoreSystemProxy() { proxyService_->restoreOwned(); }

void SettingsPage::renderSystemProxy() {
    // CONFIRMED, and the same answer the toolbar and the tray show: a change in
    // flight leaves the switch on the last confirmed value and reads as pending.
    const bool matches = routing_->systemProxyEnabled();
    const bool available = routing_->systemProxyAvailable();
    const bool pending = routing_->systemProxyPending();
    if (!pending) proxyRequest_.reset();
    // Descriptive only: what the OS currently holds, and why a read failed.
    const auto &state = proxyService_->state();
    const bool on = state.config.port != 0;
    const QSignalBlocker blocker(proxyToggle_);
    proxyToggle_->setChecked(matches);
    proxyToggle_->setEnabled(available);
    bypassEdit_->setEnabled(state.supported && !pending && !proxyService_->isShuttingDown());
    QString system;
    if (pending) {
        system = proxyService_->isRestoring() ? tr("Restoring system proxy…") :
            proxyRequest_.has_value()
                ? (*proxyRequest_ ? tr("Applying system proxy…") : tr("Restoring system proxy…"))
                : tr("Checking system proxy…");
    } else if (!state.valid) {
        system = tr("System proxy state is unavailable");
    } else {
        system = on ? tr("System proxy: %1:%2").arg(state.config.host).arg(state.config.port)
                    : tr("System proxy: off");
    }
    const QString port = corePort_ != 0 ? tr("Core port: %1").arg(corePort_)
                                       : tr("Core port: not reported yet");
    proxyState_->setText(system + "   ·   " + port);
    const QString operationError = routing_->lastError();
    proxySection_->setError(operationError.isEmpty() ? state.error : operationError);
    emit systemProxyStateChanged(matches, available);
    emit systemProxyBusyChanged(pending);
}

void SettingsPage::renderAutostart() {
    const QSignalBlocker blocker(startupToggle_);
    startupToggle_->setChecked(startupEnabled_);
    startupToggle_->setEnabled(platform::Autostart::isSupported() && startupKnown_ && !startupBusy_);
    startupToggle_->setText(startupBusy_ ? tr("Checking / updating login item…") : tr("Launch at login"));
}

void SettingsPage::runAutostartOperation(std::optional<bool> enabled) {
    if (startupBusy_) { renderAutostart(); return; }
    startupBusy_ = true;
    startupSection_->setError({});
    renderAutostart();
    auto *watcher = new QFutureWatcher<AutostartResult>(this);
    connect(watcher, &QFutureWatcher<AutostartResult>::finished, this, [this, watcher] {
        const AutostartResult result = watcher->result();
        watcher->deleteLater();
        startupBusy_ = false;
        startupKnown_ = result.valid;
        if (result.valid) startupEnabled_ = result.enabled;
        if (!result.success || !result.valid) startupSection_->setError(result.error);
        renderAutostart();
    });
    watcher->setFuture(QtConcurrent::run([enabled] {
        AutostartResult result;
        if (enabled) {
            result.success = platform::Autostart::setEnabled(*enabled);
            if (!result.success) result.error = platform::Autostart::lastError();
        }
        result.enabled = platform::Autostart::isEnabled();
        const QString readError = platform::Autostart::lastError();
        result.valid = readError.isEmpty();
        if (!readError.isEmpty()) result.error += (result.error.isEmpty() ? QString() : "\n") + readError;
        if (enabled && result.success && result.valid && result.enabled != *enabled) {
            result.success = false;
            result.error = QObject::tr("The login item did not match the requested state after saving.");
        }
        if ((!result.success || !result.valid) && result.error.isEmpty())
            result.error = QObject::tr("Could not read or update the login item.");
        return result;
    }));
}

void SettingsPage::buildCore(QVBoxLayout *column) {
    auto *section = new SettingsSection(tr("Core"), tr("Choose the mihomo executable used for managed profiles."), this);
    auto *binary = new QLineEdit(settings().value("core/binary").toString(), section);
    binary->setPlaceholderText(backend_->backend().discoverBinary());
    binary->setAccessibleName(tr("Mihomo executable"));
    auto *browse = new QPushButton(tr("Browse…"), section);
    auto *row = new QHBoxLayout;
    row->addWidget(binary, 1);
    row->addWidget(browse);
    section->addLayout(row);
    connect(browse, &QPushButton::clicked, this, [this, binary] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Choose mihomo"));
        if (!path.isEmpty()) {
            binary->setText(path);
            settings().setValue("core/binary", path);
            backend_->backend().setBinaryPath(path);
        }
    });
    connect(binary, &QLineEdit::editingFinished, this, [this, binary] {
        const QString path = binary->text().trimmed();
        settings().setValue("core/binary", path);
        backend_->backend().setBinaryPath(path);
    });
    auto *controller = new QLabel(section);
    controller->setTextFormat(Qt::PlainText);
    controller->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const auto updateEndpoint = [this, controller] {
        controller->setText(tr("Controller: %1").arg(httpBase(backend_->endpoint())));
    };
    connect(backend_, &cb::BackendBridge::endpointChanged, this, updateEndpoint);
    updateEndpoint();
    section->addWidget(controller);
    auto *geo = new QPushButton(tr("Update GEO Databases"), section);
    geo->setToolTip(tr("Ask the connected core to download its configured geographic databases."));
    geo->setEnabled(backend_->isConnected());
    auto *geoStatus = new QLabel(section);
    geoStatus->setTextFormat(Qt::PlainText);
    geoStatus->setWordWrap(true);
    section->addWidget(geo);
    section->addWidget(geoStatus);
    connect(geo, &QPushButton::clicked, this, [this, geo, geoStatus] {
        geo->setProperty("updating", true);
        geo->setEnabled(false);
        geoStatus->setText(tr("Updating GEO databases…"));
        backend_->updateGeoDatabases();
    });
    connect(backend_, &cb::BackendBridge::geoDatabasesUpdated, section,
            [this, geo, geoStatus](const QString &error) {
        geo->setProperty("updating", false);
        geo->setEnabled(backend_->isConnected());
        geoStatus->setText(error.isEmpty() ? tr("GEO databases updated.") : error);
    });
    const auto cancelGeo = [geo, geoStatus] {
        if (geo->property("updating").toBool())
            geoStatus->setText(tr("Controller connection changed; update status is unknown."));
        geo->setProperty("updating", false);
        geo->setEnabled(false);
    };
    connect(backend_, &cb::BackendBridge::endpointChanged, section, cancelGeo);
    connect(backend_, &cb::BackendBridge::connectedChanged, section,
            [geo, cancelGeo](bool connected) {
        if (!connected) cancelGeo();
        else geo->setEnabled(!geo->property("updating").toBool());
    });
    column->addWidget(section);
}

void SettingsPage::buildRuntime(QVBoxLayout *column) {
    auto *section = new SettingsSection(tr("Runtime Settings"),
        tr("Overrides for managed profiles. Apply validates and restarts a core started by this app."), this);
    const auto coreFailure = std::make_shared<bool>(false);
    const QJsonObject current = context_.profiles->runtimeOverrides();
    auto *port = new QSpinBox(section);
    port->setRange(1, 65535);
    port->setValue(current.value("mixed-port").toInt(27890));
    auto *mode = new ComboBox(section);
    mode->addItems({"rule", "global", "direct"});
    mode->setCurrentText(current.value("mode").toString("rule"));
    auto *lan = new QCheckBox(tr("Allow connections from the LAN"), section);
    lan->setChecked(current.value("allow-lan").toBool());
    auto *ipv6 = new QCheckBox(tr("Enable IPv6"), section);
    ipv6->setChecked(current.value("ipv6").toBool());
    const auto dirty = std::make_shared<bool>(false);
    const auto edited = [dirty] { *dirty = true; };
    connect(port, &QSpinBox::valueChanged, section, edited);
    connect(mode, &QComboBox::currentTextChanged, section, edited);
    connect(lan, &QCheckBox::toggled, section, edited);
    connect(ipv6, &QCheckBox::toggled, section, edited);
    connect(backend_, &cb::BackendBridge::configReceived, section,
            [this, port, mode, lan, ipv6, dirty](const cb::BaseConfig &config) {
        if (*dirty || backend_->coreState() != cb::CoreState::Running) return;
        const QSignalBlocker portBlocker(port), modeBlocker(mode), lanBlocker(lan), ipv6Blocker(ipv6);
        if (config.mixedPort) port->setValue(config.mixedPort);
        mode->setCurrentText(config.mode);
        lan->setChecked(config.allowLan);
        ipv6->setChecked(config.ipv6);
    });
    auto *form = new QFormLayout;
    form->addRow(tr("HTTP / SOCKS port"), port);
    form->addRow(tr("Routing mode"), mode);
    section->addLayout(form);
    section->addWidget(lan);
    section->addWidget(ipv6);
    auto *buttons = new QHBoxLayout;
    auto *advanced = new QPushButton(tr("DNS / TUN / Advanced…"), section);
    auto *apply = new QPushButton(tr("Apply"), section);
    buttons->addWidget(advanced);
    buttons->addStretch();
    buttons->addWidget(apply);
    section->addLayout(buttons);
    connect(apply, &QPushButton::clicked, this, [this, section, port, mode, lan, ipv6, dirty, coreFailure] {
        auto overrides = context_.profiles->runtimeOverrides();
        overrides.insert("mixed-port", port->value());
        overrides.insert("mode", mode->currentText());
        overrides.insert("allow-lan", lan->isChecked());
        overrides.insert("ipv6", ipv6->isChecked());
        *coreFailure = false;
        section->setError({});
        section->setNotice({});
        if (!context_.profiles->setRuntimeOverrides(overrides)) return;
        *dirty = false;
        if (backend_->coreState() == cb::CoreState::Running)
            context_.profiles->requestRuntimeConfig();
        else section->setNotice(tr("Saved. These settings take effect when you start the core."));
    });
    connect(context_.profiles, &core::ProfileStore::errorOccurred, section,
            [section, coreFailure](const QString &error) {
        *coreFailure = false;
        section->setError(error);
    });
    connect(backend_, &cb::BackendBridge::coreFailed, section,
            [section, coreFailure](const QString &error) {
        *coreFailure = true;
        section->setError(error);
    });
    connect(backend_, &cb::BackendBridge::coreReady, section, [section, coreFailure] {
        // Clear an earlier core failure only after a successful launch. An
        // unrelated profile/save error must remain visible until addressed.
        if (*coreFailure) section->setError({});
        *coreFailure = false;
    });
    connect(advanced, &QPushButton::clicked, this, [this, port, mode, lan, ipv6] {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Advanced Runtime Overrides"));
        dialog.resize(680, 520);
        auto *layout = new QVBoxLayout(&dialog);
        auto *hint = new QLabel(tr("Enter mihomo settings as a JSON object. DNS, TUN, sniffer and other nested settings are merged with the profile. Enable privileged service mode on macOS before using TUN with a managed core."), &dialog);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        auto *editor = new QPlainTextEdit(&dialog);
        QFont editorFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        editorFont.setPointSizeF(qMax(editorFont.pointSizeF(), font().pointSizeF()));
        editor->setFont(editorFont);
        editor->setPlainText(QString::fromUtf8(QJsonDocument(context_.profiles->runtimeOverrides()).toJson()));
        layout->addWidget(editor);
        auto *error = new QLabel(&dialog);
        error->setObjectName("fieldError");
        error->setWordWrap(true);
        layout->addWidget(error);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(context_.profiles, &core::ProfileStore::errorOccurred, &dialog, [error](const QString &text) { error->setText(text); });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this] {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(editor->toPlainText().toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                error->setText(tr("Enter a valid JSON object: %1").arg(parseError.errorString()));
                return;
            }
            if (!context_.profiles->setRuntimeOverrides(document.object())) return;
            const auto saved = document.object();
            port->setValue(saved.value("mixed-port").toInt(27890));
            mode->setCurrentText(saved.value("mode").toString("rule"));
            lan->setChecked(saved.value("allow-lan").toBool());
            ipv6->setChecked(saved.value("ipv6").toBool());
            dialog.accept();
        });
        dialog.exec();
    });
    column->addWidget(section);
}

}  // namespace ui
