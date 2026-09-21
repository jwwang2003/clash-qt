#include "ui/pages/settings/service_settings.h"
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include "core/mihomo/process/core_process.h"
#include "core/profiles/profile_store.h"
#include "platform/service/privileged_service_client.h"
#include "platform/service/privileged_service_installer.h"

namespace ui {
ServiceSettings::ServiceSettings(const app::Context &context, QWidget *parent)
    : SettingsSection(tr("Privileged Service"),
        tr("Run the managed core with the privileges needed for TUN. The desktop app stays unprivileged. "
           "Installing or removing the service requires macOS administrator authorization."), parent),
      context_(context), installer_(new platform::PrivilegedServiceInstaller(this)),
      probe_(new platform::PrivilegedServiceClient(this)), startupRetry_(new QTimer(this)) {
    startupRetry_->setSingleShot(true);
    startupRetry_->setInterval(500);
    connect(startupRetry_, &QTimer::timeout, probe_, &platform::PrivilegedServiceClient::requestStatus);
    setAccessibleName(tr("Privileged Service"));
    status_ = new QLabel(this);
    status_->setObjectName("privilegedServiceStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    addWidget(status_);
    enabled_ = new QCheckBox(tr("Use privileged service for managed core"), this);
    enabled_->setObjectName("usePrivilegedService");
    addWidget(enabled_);
    auto *hint = new QLabel(tr("The service uses an administrator-installed copy of the selected mihomo executable. "
        "Repair the service after changing that executable. File providers and external certificate/key files "
        "are not supported in service mode; inline and HTTP(S) subscriptions are supported."), this);
    hint->setWordWrap(true);
    addWidget(hint);
    auto *row = new QHBoxLayout;
    install_ = new QPushButton(tr("Install Service…"), this);
    install_->setObjectName("installPrivilegedService");
    remove_ = new QPushButton(tr("Remove Service…"), this);
    remove_->setObjectName("removePrivilegedService");
    check_ = new QPushButton(tr("Check Status"), this);
    row->addWidget(install_);
    row->addWidget(remove_);
    row->addWidget(check_);
    row->addStretch();
    addLayout(row);
    connect(context_.coreProcess, &core::CoreProcess::stateChanged, this, [this] {
        const auto state = context_.coreProcess->state();
        if (state == core::CoreState::Stopped || state == core::CoreState::Failed)
            QTimer::singleShot(0, this, &ServiceSettings::checkStatus);
        refresh();
    });
    connect(context_.profiles, &core::ProfileStore::runtimeBusyChanged, this, [this] { refresh(); });
    connect(installer_, &platform::PrivilegedServiceInstaller::busyChanged, this, [this](bool busy) {
        refresh();
        emit installationBusyChanged(busy);
    });
    connect(installer_, &platform::PrivilegedServiceInstaller::statusChanged, this,
            [this](bool installed, const QString &error) {
        installed_ = installed;
        checked_ = true;
        if (!error.isEmpty()) setError(error);
        refresh();
    });
    connect(installer_, &platform::PrivilegedServiceInstaller::finished, this,
            [this](bool installed, bool success, const QString &error) {
        installed_ = installed;
        checked_ = true;
        setError(success ? QString() : error);
        if (success) {
            const bool service = installed;
            applyMode(service);
            startupRetries_ = installed ? 20 : 0;
            reachable_ = false;
            setNotice(installed ? tr("Service installed. Start the core, then enable TUN.")
                                : tr("Service removed. Start the core to use normal user mode."));
            checkStatus();
        }
        refresh();
    });
    connect(probe_, &platform::PrivilegedServiceClient::statusReceived, this, [this](const QJsonObject &status) {
        serviceRunning_ = status.value("state").toString() == "running";
        startupRetries_ = 0;
        startupRetry_->stop();
        setError({});
        reachable_ = true;
        probe_->close();
        refresh();
    });
    connect(probe_, &platform::PrivilegedServiceClient::errorOccurred, this, [this](const QString &error) {
        reachable_ = false;
        if (startupRetries_ > 0 && !installer_->isBusy()) {
            --startupRetries_;
            startupRetry_->start();
        } else if (installed_) {
            setError(tr("The installed service is not reachable. Try Check Status or Repair Service. %1").arg(error));
        }
        refresh();
    });
    connect(enabled_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (applyMode(enabled)) {
            setNotice(tr("Saved. Start the core to apply this mode."));
        }
        refresh();
    });
    connect(check_, &QPushButton::clicked, this, &ServiceSettings::checkStatus);
    connect(install_, &QPushButton::clicked, this, [this] {
        setError({});
        probe_->close();
        const QString path = context_.coreProcess->binaryPath().isEmpty()
            ? core::CoreProcess::discoverBinary() : context_.coreProcess->binaryPath();
        installer_->install(path);
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        auto *message = new QMessageBox(QMessageBox::Question, tr("Remove Privileged Service"),
            tr("Remove clash-qt’s installed helper, core copy, and service runtime data? Your app profiles remain available."),
            QMessageBox::Yes | QMessageBox::Cancel, this);
        message->setDefaultButton(QMessageBox::Cancel);
        message->setAttribute(Qt::WA_DeleteOnClose);
        connect(message, &QMessageBox::finished, this, [this](int result) {
            if (result != QMessageBox::Yes || installer_->isBusy()) return;
            const auto state = context_.coreProcess->state();
            if ((state != core::CoreState::Stopped && state != core::CoreState::Failed)
                || context_.profiles->isRuntimeBusy() || serviceRunning_) return;
            probe_->close();
            installer_->uninstall();
        });
        message->open();
    });
    refresh();
    QTimer::singleShot(0, this, &ServiceSettings::checkStatus);
}

bool ServiceSettings::applyMode(bool enabled) {
    if (!context_.coreProcess->setUseService(enabled)) return false;
    QSettings("clash-qt", "clash-qt").setValue("core/useService", enabled);
    if (!enabled) {
        auto overrides = context_.profiles->runtimeOverrides();
        auto tun = overrides.value("tun").toObject();
        tun.insert("enable", false);
        overrides.insert("tun", tun);
        if (!context_.profiles->setRuntimeOverrides(overrides)) {
            setError(tr("Service mode was disabled, but TUN defaults could not be saved. Turn TUN off before restarting in normal user mode."));
        }
    }
    return true;
}

void ServiceSettings::checkStatus() {
    if (!platform::PrivilegedServiceInstaller::isSupported() || installer_->isBusy()) return;
    setError({});
    installer_->refresh();
    probe_->requestStatus();
}

void ServiceSettings::refresh() {
    const bool supported = platform::PrivilegedServiceInstaller::isSupported();
    const auto state = context_.coreProcess->state();
    const bool stopped = state == core::CoreState::Stopped || state == core::CoreState::Failed;
    const bool available = supported && stopped && !serviceRunning_ && !context_.profiles->isRuntimeBusy() && !installer_->isBusy();
    const QSignalBlocker blocker(enabled_);
    enabled_->setChecked(context_.coreProcess->isServiceMode());
    enabled_->setEnabled(available && installed_);
    install_->setEnabled(available);
    remove_->setEnabled(available && installed_);
    check_->setEnabled(supported && !installer_->isBusy());
    install_->setText(installed_ ? tr("Repair Service…") : tr("Install Service…"));
    if (!supported) {
        status_->setText(tr("Privileged service installation is currently supported on macOS."));
    } else if (installer_->isBusy()) {
        status_->setText(tr("Waiting for administrator authorization / updating service…"));
    } else if (startupRetries_ > 0 || startupRetry_->isActive()) {
        status_->setText(tr("Starting privileged service…"));
    } else if (context_.coreProcess->usesPrivilegedService()) {
        status_->setText(state == core::CoreState::Starting ? tr("Service core starting…")
            : state == core::CoreState::Stopping ? tr("Service core stopping…")
            : tr("Service core active"));
    } else if (!checked_) {
        status_->setText(tr("Checking installation…"));
    } else {
        status_->setText(!installed_ ? tr("Service not installed")
            : reachable_ ? tr("Service installed and reachable") : tr("Service installed; not reachable yet"));
    }
    if (supported && stopped && serviceRunning_)
        setNotice(tr("A service core is still running. Stop the app session that owns it, then check status."));
    else if (supported && !stopped)
        setNotice(tr("Stop the managed core before installing, repairing, removing, or changing service mode."));
    else if (supported && installed_)
        setNotice(tr("Start the core to use the selected mode, then enable TUN."));
    else
        setNotice({});
}
}
