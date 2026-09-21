#pragma once

#include <QWidget>

#include "core/mihomo/provider_client.h"

class QComboBox;
class QLabel;
class QPushButton;
class QStandardItemModel;
class QSortFilterProxyModel;
class QTableView;
class QTimer;

namespace ui {

class ProvidersPage : public QWidget {
    Q_OBJECT

public:
    explicit ProvidersPage(core::MihomoClient *client, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void refresh();
    void populate(bool rules, const QVector<core::Provider> &providers);
    void renderProviders();
    void updateActions();
    QString selectedName() const;

    core::ProviderClient *providers_;
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
    QVector<core::Provider> cachedProviders_;
    QTimer *renderTimer_;
};

}  // namespace ui
