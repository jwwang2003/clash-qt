#include "ui/routing_controls.h"

#include <QHBoxLayout>
#include <QSignalBlocker>
#include "core/mihomo_client.h"
#include "ui/toggle_switch.h"

namespace ui {

RoutingControls::RoutingControls(core::MihomoClient *client, QWidget *parent)
    : QWidget(parent), client_(client) {
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(4, 0, 4, 0);
    row->setSpacing(12);
    systemProxy_ = new ToggleSwitch(tr("System Proxy"), this);
    systemProxy_->setObjectName("systemProxySwitch");
    systemProxy_->setAccessibleName(tr("System Proxy"));
    systemProxy_->setToolTip(tr("Route apps that use the operating system proxy through the connected core."));
    systemProxy_->setEnabled(false);
    tun_ = new ToggleSwitch(tr("TUN Mode"), this);
    tun_->setObjectName("tunSwitch");
    tun_->setAccessibleName(tr("TUN Mode"));
    row->addWidget(systemProxy_);
    row->addWidget(tun_);
    connect(systemProxy_, &QCheckBox::toggled, this, &RoutingControls::requestSystemProxyChange);
    connect(tun_, &QCheckBox::toggled, this, &RoutingControls::requestTunChange);
    connect(client_, &core::MihomoClient::configReceived, this, [this](const core::BaseConfig &config) {
        configKnown_ = true;
        tunEnabled_ = config.tunEnabled;
        refreshTun();
    });
    const auto disconnected = [this] {
        configKnown_ = false;
        tunEnabled_ = false;
        tunPending_ = false;
        tunError_.clear();
        refreshTun();
    };
    connect(client_, &core::MihomoClient::endpointChanged, this, disconnected);
    connect(client_, &core::MihomoClient::connectedChanged, this, [this, disconnected](bool connected) {
        if (!connected) disconnected();
        else refreshTun();
    });
    connect(client_, &core::MihomoClient::tunChangeFinished, this,
            [this](bool requested, bool actual, const QString &error) {
        tunPending_ = false;
        tunEnabled_ = actual;
        tunError_ = error;
        refreshTun();
        if (!error.isEmpty()) emit errorOccurred(error);
        else if (requested == actual) emit tunApplied(actual);
    });
    refreshTun();
}

bool RoutingControls::systemProxyEnabled() const { return systemProxy_->isChecked(); }
bool RoutingControls::systemProxyAvailable() const { return systemProxy_->isEnabled(); }
bool RoutingControls::tunAvailable() const { return tun_->isEnabled(); }

void RoutingControls::setSystemProxyState(bool enabled, bool available) {
    const QSignalBlocker blocker(systemProxy_);
    systemProxy_->setChecked(enabled);
    systemProxy_->setEnabled(available);
    emit stateChanged();
}

void RoutingControls::requestSystemProxyChange(bool enabled) {
    if (systemProxyAvailable()) emit systemProxyRequested(enabled);
}

void RoutingControls::requestTunChange(bool enabled) {
    if (!client_->isConnected() || !configKnown_ || tunPending_ || enabled == tunEnabled_) {
        refreshTun();
        return;
    }
    if (enabled && !tunEnableBlockedReason_.isEmpty()) {
        tunError_ = tunEnableBlockedReason_;
        refreshTun();
        emit errorOccurred(tunError_);
        return;
    }
    tunPending_ = true;
    tunError_.clear();
    refreshTun();
    client_->setTunEnabled(enabled);
}

void RoutingControls::setTunEnableBlockedReason(const QString &reason) {
    if (tunEnableBlockedReason_ == reason) return;
    tunEnableBlockedReason_ = reason;
    tunError_.clear();
    refreshTun();
}

void RoutingControls::refreshTun() {
    const QSignalBlocker blocker(tun_);
    tun_->setChecked(tunEnabled_);
    tun_->setEnabled(client_->isConnected() && configKnown_ && !tunPending_);
    if (!tunError_.isEmpty()) tun_->setToolTip(tunError_);
    else if (tunPending_) tun_->setToolTip(tr("Applying TUN mode…"));
    else if (!client_->isConnected() || !configKnown_) tun_->setToolTip(tr("Waiting for a connected core’s configuration."));
    else if (!tunEnabled_ && !tunEnableBlockedReason_.isEmpty()) tun_->setToolTip(tunEnableBlockedReason_);
    else tun_->setToolTip(tr("Route traffic through a virtual network interface. The core needs permission to create interfaces and routes."));
    emit stateChanged();
}

}  // namespace ui
