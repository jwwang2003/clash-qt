#pragma once

#include <QHash>
#include <QVector>
#include <QWidget>

#include "core/types.h"

class QLabel;
class QListWidget;
class QPushButton;

namespace core {
class MihomoClient;
}

namespace ui {

class ProxiesPage : public QWidget {
    Q_OBJECT

public:
    explicit ProxiesPage(core::MihomoClient *client, QWidget *parent = nullptr);

    void refresh();
    void testActiveGroup();

private slots:
    void onProxiesUpdated(const QVector<core::ProxyGroup> &groups,
                          const QHash<QString, core::ProxyNode> &nodes);
    void onGroupRowChanged(int row);
    void onNodeActivated(int row);

private:
    void renderNodes();

    core::MihomoClient *client_;
    QListWidget *groupList_;
    QListWidget *nodeList_;
    QLabel *summaryLabel_;
    QPushButton *testButton_;

    QVector<core::ProxyGroup> groups_;
    QHash<QString, core::ProxyNode> nodes_;
    QString activeGroup_;
};

}  // namespace ui
