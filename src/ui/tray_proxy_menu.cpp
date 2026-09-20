#include "ui/tray_proxy_menu.h"

#include <QActionGroup>
#include "core/mihomo_client.h"

namespace ui {

TrayProxyMenu::TrayProxyMenu(core::MihomoClient *client, QWidget *parent)
    : QMenu(tr("Proxy Groups"), parent), client_(client) {
    setEnabled(client_->isConnected());
    connect(client_, &core::MihomoClient::connectedChanged, this, &QMenu::setEnabled);
    connect(client_, &core::MihomoClient::proxiesUpdated, this,
            [this](const QVector<core::ProxyGroup> &groups, const QHash<QString, core::ProxyNode> &) {
        groups_ = groups;
    });
    connect(this, &QMenu::aboutToShow, this, &TrayProxyMenu::rebuild);
}

void TrayProxyMenu::rebuild() {
    qDeleteAll(submenus_);
    submenus_.clear();
    clear();
    for (const auto &group : groups_) {
        auto *menu = new QMenu(QString(group.name).replace('&', "&&"), this);
        menu->setToolTipsVisible(true);
        menu->setStyleSheet("QMenu { menu-scrollable: 1; }");
        addMenu(menu);
        submenus_.append(menu);
        auto *choices = new QActionGroup(menu);
        choices->setExclusive(true);
        if (group.type == "URLTest" || group.type == "Fallback") {
            auto *automatic = menu->addAction(tr("Automatic selection"));
            automatic->setCheckable(true);
            automatic->setChecked(group.fixed.isEmpty());
            connect(automatic, &QAction::triggered, this, [this, name = group.name] { reset(name); });
            menu->addSeparator();
        }
        for (const QString &node : group.all) {
            auto *action = menu->addAction(QString(node).replace('&', "&&"));
            action->setToolTip(node);
            action->setData(node);
            action->setCheckable(true);
            action->setChecked(node == group.now);
            action->setEnabled(group.selectable());
            choices->addAction(action);
            connect(action, &QAction::triggered, this,
                    [this, name = group.name, node] { choose(name, node); });
        }
        if (group.all.isEmpty()) menu->addAction(tr("No nodes"))->setEnabled(false);
    }
    if (groups_.isEmpty()) addAction(tr("No proxy groups"))->setEnabled(false);
}

void TrayProxyMenu::choose(const QString &name, const QString &node) {
    if (!client_->isConnected()) return;
    for (const auto &group : groups_)
        if (group.name == name && group.selectable() && group.all.contains(node)) {
            client_->selectNode(name, node);
            return;
        }
}

void TrayProxyMenu::reset(const QString &name) {
    if (!client_->isConnected()) return;
    for (const auto &group : groups_)
        if (group.name == name && (group.type == "URLTest" || group.type == "Fallback")) {
            client_->resetGroupSelection(name);
            return;
        }
}

}  // namespace ui
