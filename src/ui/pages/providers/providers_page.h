#pragma once

#include <QVector>
#include <QWidget>

#include "core/backend/types.h"

class QComboBox;
class QLabel;
class QPushButton;
class QStandardItemModel;
class QSortFilterProxyModel;
class QTableView;
class QTimer;

namespace core::backend {
class BackendBridge;
}

namespace ui {

class ProvidersPage : public QWidget {
    Q_OBJECT

public:
    explicit ProvidersPage(core::backend::BackendBridge *bridge, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void refresh();
    void populate(bool rules, const QVector<core::backend::Provider> &providers);
    void renderProviders();
    void updateActions();
    QString selectedName() const;

    core::backend::BackendBridge *bridge_;
    QComboBox *kind_;
    QStandardItemModel *model_;
    QSortFilterProxyModel *filter_;
    QTableView *view_;
    QLabel *status_;
    QPushButton *refresh_;
    QPushButton *update_;
    QPushButton *updateAll_;
    QPushButton *health_;
    bool busy_ = false;
    bool dirty_ = true;
    bool cachedRules_ = false;
    QVector<core::backend::Provider> cachedProviders_;
    QTimer *renderTimer_;
};

}  // namespace ui
