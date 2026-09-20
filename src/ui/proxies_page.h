#pragma once

#include <QHash>
#include <QVector>
#include <QWidget>

#include "core/types.h"

class QLabel;
class QLineEdit;
class QComboBox;
class QListWidget;
class QPushButton;
class QTimer;

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

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onProxiesUpdated(const QVector<core::ProxyGroup> &groups,
                          const QHash<QString, core::ProxyNode> &nodes);
    void onGroupRowChanged(int row);
    void onNodeActivated(int row);

private:
    void renderNodes();
    void renderPending();
    void renderGroups();

    core::MihomoClient *client_;
    QListWidget *groupList_;
    QListWidget *nodeList_;
    QLabel *summaryLabel_;
    QPushButton *testButton_;
    QLineEdit *filterEdit_;
    QComboBox *sortBox_;

    QVector<core::ProxyGroup> groups_;
    QHash<QString, core::ProxyNode> nodes_;
    QString activeGroup_;
    QTimer *renderTimer_;
    QTimer *filterTimer_;
    bool groupsDirty_ = false;
    bool nodesDirty_ = false;
};

}  // namespace ui
