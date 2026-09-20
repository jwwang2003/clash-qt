#include "ui/connections_page.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

#include "core/mihomo_client.h"
#include "ui/formatting.h"
#include "ui/theme.h"

namespace ui {
namespace {

/// The stylesheet's header and cell padding. Cells also carry the style's focus
/// margin, so they need the wider of the two.
constexpr int kHeaderPadding = 22;
constexpr int kCellPadding = 24;

/// Wide enough for the header label and the widest value the column shows.
int fittedWidth(const QFontMetrics &headerMetrics, const QString &title,
                const QFontMetrics &cellMetrics, const QString &sample) {
    return qMax(headerMetrics.horizontalAdvance(title) + kHeaderPadding,
                cellMetrics.horizontalAdvance(sample) + kCellPadding);
}

}  // namespace

ConnectionModel::ConnectionModel(QObject *parent) : QAbstractTableModel(parent) {}

void ConnectionModel::setConnections(const QVector<core::Connection> &connections) {
    beginResetModel();
    connections_ = connections;
    endResetModel();
}

QString ConnectionModel::idAt(int row) const {
    if (row < 0 || row >= connections_.size()) return {};
    return connections_.at(row).id;
}

int ConnectionModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(connections_.size());
}

int ConnectionModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ConnectionModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) return {};
    const core::Connection &connection = connections_.at(index.row());

    // Sorting reads Qt::UserRole so byte and duration columns order numerically.
    if (role == Qt::UserRole) {
        switch (index.column()) {
            case Upload:
                return QVariant::fromValue<qulonglong>(connection.upload);
            case Download:
                return QVariant::fromValue<qulonglong>(connection.download);
            case Duration:
                return QVariant::fromValue<qlonglong>(
                    connection.start.secsTo(QDateTime::currentDateTime()));
            default:
                break;
        }
    } else if (role == Qt::ToolTipRole) {
        // The long text columns are elided to keep all nine on screen, so the
        // full value has to stay reachable.
        switch (index.column()) {
            case Host:
            case Chains:
            case Rule:
            case Process:
                break;
            default:
                return {};
        }
    } else if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
        case Host:
            return connection.host;
        case Network:
            return connection.network;
        case Type:
            return connection.connectionType;
        case Chains:
            return connection.chains.join(" → ");
        case Rule:
            return connection.rulePayload.isEmpty()
                       ? connection.rule
                       : QString("%1(%2)").arg(connection.rule, connection.rulePayload);
        case Process:
            return connection.process;
        case Upload:
            return formatBytes(connection.upload);
        case Download:
            return formatBytes(connection.download);
        case Duration:
            return formatDuration(connection.start);
        default:
            return {};
    }
}

QVariant ConnectionModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case Host:
            return tr("Host");
        case Network:
            return tr("Network");
        case Type:
            return tr("Type");
        case Chains:
            return tr("Chains");
        case Rule:
            return tr("Rule");
        case Process:
            return tr("Process");
        case Upload:
            return tr("Upload");
        case Download:
            return tr("Download");
        case Duration:
            return tr("Duration");
        default:
            return {};
    }
}

ConnectionsPage::ConnectionsPage(core::MihomoClient *client, QWidget *parent)
    : QWidget(parent),
      client_(client),
      model_(new ConnectionModel(this)),
      proxy_(new QSortFilterProxyModel(this)) {
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(Qt::UserRole);
    proxy_->setFilterKeyColumn(ConnectionModel::Host);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);

    auto *filterEdit = new QLineEdit(this);
    filterEdit->setPlaceholderText(tr("Filter host…"));
    filterEdit->setClearButtonEnabled(true);
    connect(filterEdit, &QLineEdit::textChanged, proxy_,
            &QSortFilterProxyModel::setFilterFixedString);

    totalLabel_ = new QLabel("↑ 0 B  ↓ 0 B", this);
    totalLabel_->setObjectName("pageSummary");

    auto *closeAllButton = new QPushButton(tr("Close All"), this);
    connect(closeAllButton, &QPushButton::clicked, client_,
            &core::MihomoClient::closeAllConnections);

    view_ = new QTableView(this);
    view_->setModel(proxy_);
    view_->setSortingEnabled(true);
    view_->setAlternatingRowColors(true);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->verticalHeader()->setVisible(false);
    view_->verticalHeader()->setDefaultSectionSize(QFontMetrics(font()).height() + 12);
    connect(view_, &QTableView::customContextMenuRequested, this,
            &ConnectionsPage::showContextMenu);

    QHeaderView *header = view_->horizontalHeader();
    header->setSectionsMovable(true);
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(56);

    // All nine columns have to fit the default window: the short ones are sized
    // to their own worst case, the long ones get a readable slice and elide,
    // and Host takes whatever is left.
    const QFontMetrics headerMetrics(header->font());
    const QFontMetrics cellMetrics(view_->font());
    const struct {
        int column;
        const char *sample;
    } fitted[] = {
        {ConnectionModel::Network, "udp"},
        {ConnectionModel::Type, "HTTPS"},
        {ConnectionModel::Upload, "999.9 MB"},
        {ConnectionModel::Download, "999.9 MB"},
        {ConnectionModel::Duration, "99h 59m"},
    };
    for (const auto &[column, sample] : fitted) {
        const QString title = model_->headerData(column, Qt::Horizontal).toString();
        view_->setColumnWidth(column, fittedWidth(headerMetrics, title, cellMetrics,
                                                  QString::fromLatin1(sample)));
    }
    const struct {
        int column;
        int chars;
    } elided[] = {
        {ConnectionModel::Chains, 14},
        {ConnectionModel::Rule, 15},
        {ConnectionModel::Process, 11},
    };
    for (const auto &[column, chars] : elided) {
        view_->setColumnWidth(column, cellMetrics.averageCharWidth() * chars + kCellPadding);
    }
    header->setSectionResizeMode(ConnectionModel::Host, QHeaderView::Stretch);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(theme::kPageSpacing);
    controls->addWidget(filterEdit, 1);
    controls->addWidget(totalLabel_);
    controls->addWidget(closeAllButton);

    auto *layout = theme::pageLayout(this);
    layout->addLayout(controls);
    layout->addWidget(view_, 1);

    connect(client_, &core::MihomoClient::connectionsUpdated, this,
            &ConnectionsPage::onConnectionsUpdated);
}

void ConnectionsPage::onConnectionsUpdated(const QVector<core::Connection> &connections,
                                           quint64 uploadTotal, quint64 downloadTotal) {
    model_->setConnections(connections);
    totalLabel_->setText(
        QString("↑ %1  ↓ %2").arg(formatBytes(uploadTotal), formatBytes(downloadTotal)));
}

void ConnectionsPage::showContextMenu(const QPoint &pos) {
    const QModelIndex index = view_->indexAt(pos);
    if (!index.isValid()) return;

    const QString id = model_->idAt(proxy_->mapToSource(index).row());
    if (id.isEmpty()) return;

    QMenu menu(this);
    menu.addAction(tr("Close Connection"), this, [this, id] { client_->closeConnection(id); });
    menu.exec(view_->viewport()->mapToGlobal(pos));
}

}  // namespace ui
