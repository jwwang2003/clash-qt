#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QHash>
#include <optional>
#include <QVector>
#include <QWidget>

#include "core/types.h"

class QLabel;
class QComboBox;
class QSortFilterProxyModel;
class QTableView;
class QTimer;

namespace core {
class MihomoClient;
}

namespace ui {

class ConnectionModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Host,
        Network,
        Type,
        Chains,
        Rule,
        Process,
        Upload,
        Download,
        UploadRate,
        DownloadRate,
        Duration,
        ColumnCount
    };

    explicit ConnectionModel(QObject *parent = nullptr);

    void setConnections(const QVector<core::Connection> &connections);
    QString idAt(int row) const;
    std::optional<core::Connection> connectionAt(int row) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    QVector<core::Connection> connections_;
};

class ConnectionsPage : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionsPage(core::MihomoClient *client, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onConnectionsUpdated(const QVector<core::Connection> &connections, quint64 uploadTotal,
                              quint64 downloadTotal);
    void showContextMenu(const QPoint &pos);

private:
    void renderConnections();
    void showDetails(const core::Connection &connection);

    core::MihomoClient *client_;
    ConnectionModel *model_;
    QSortFilterProxyModel *proxy_;
    QTableView *view_;
    QLabel *totalLabel_;
    QComboBox *historyBox_;
    QElapsedTimer sampleClock_;
    qint64 lastSampleMs_ = -1;
    QVector<core::Connection> current_;
    QVector<core::Connection> closed_;
    QHash<QString, core::Connection> previous_;
    QTimer *renderTimer_;
    bool currentDirty_ = false;
    bool closedDirty_ = false;
};

}  // namespace ui
