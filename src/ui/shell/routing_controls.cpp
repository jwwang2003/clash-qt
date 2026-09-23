#include "ui/shell/routing_controls.h"

#include <QHBoxLayout>
#include <QSignalBlocker>

#include "app/runtime/routing_controller.h"
#include "ui/widgets/toggle_switch.h"

namespace ui {

using app::runtime::RoutingController;

RoutingControls::RoutingControls(RoutingController *routing, QWidget *parent)
    : QWidget(parent), routing_(routing) {
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
    connect(routing_, &RoutingController::routingStateChanged, this, &RoutingControls::refresh);
    refresh();
}

bool RoutingControls::systemProxyEnabled() const { return routing_->systemProxyEnabled(); }
bool RoutingControls::systemProxyAvailable() const { return routing_->systemProxyAvailable(); }
bool RoutingControls::tunEnabled() const { return routing_->tunEnabled(); }
bool RoutingControls::tunAvailable() const { return routing_->tunAvailable(); }

void RoutingControls::requestSystemProxyChange(bool enabled) {
    // Inert while the change is not allowed, exactly as the disabled switch was.
    if (!routing_->systemProxyAvailable()) {
        refresh();
        return;
    }
    routing_->requestSystemProxy(enabled);
}

void RoutingControls::requestTunChange(bool enabled) {
    // Every guard - not connected, no configuration yet, already pending, the
    // request matches the confirmed value, the known permission block - belongs
    // to the controller, which is what makes all four routing surfaces agree.
    routing_->requestTun(enabled);
}

void RoutingControls::setTunEnableBlockedReason(const QString &reason) {
    routing_->setTunBlockedReason(reason);
}

void RoutingControls::refresh() {
    const bool available = routing_->tunAvailable();
    const QString error = routing_->lastError();
    const QString blocked = routing_->tunBlockedReason();
    {
        const QSignalBlocker proxyBlocker(systemProxy_);
        systemProxy_->setChecked(routing_->systemProxyEnabled());
        systemProxy_->setEnabled(routing_->systemProxyAvailable());
        const QSignalBlocker tunBlocker(tun_);
        tun_->setChecked(routing_->tunEnabled());
        tun_->setEnabled(available);
        if (!error.isEmpty()) tun_->setToolTip(error);
        else if (routing_->tunPending()) tun_->setToolTip(tr("Applying TUN mode…"));
        else if (!available) tun_->setToolTip(tr("Waiting for a connected core’s configuration."));
        else if (!routing_->tunEnabled() && !blocked.isEmpty()) tun_->setToolTip(blocked);
        else tun_->setToolTip(tr("Route traffic through a virtual network interface. The core needs permission to create interfaces and routes."));
    }
    emit stateChanged();
}

}  // namespace ui
