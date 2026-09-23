#pragma once

#include <QAbstractTableModel>
#include <QVector>
#include <QWidget>

#include "core/backend/types.h"

class QSortFilterProxyModel;
class QTableView;

namespace core::backend {
class BackendBridge;
}

namespace ui {

class RuleModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { Type, Payload, Proxy, ColumnCount };

    explicit RuleModel(QObject *parent = nullptr);

    void setRules(const QVector<core::backend::Rule> &rules);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    QVector<core::backend::Rule> rules_;
};

class RulesPage : public QWidget {
    Q_OBJECT

public:
    explicit RulesPage(core::backend::BackendBridge *bridge, QWidget *parent = nullptr);

private:
    core::backend::BackendBridge *bridge_;
    RuleModel *model_;
    QSortFilterProxyModel *proxy_;
    QTableView *view_;
};

}  // namespace ui
