#include "ui/pages/connections/connections_page.h"

#include <QDateTime>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include "ui/widgets/combo_box.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QScrollBar>
#include <QTableView>
#include <QVBoxLayout>
#include <QShowEvent>
#include <QTimer>
#include <algorithm>

#include "core/mihomo/mihomo_client.h"
#include "ui/theme/formatting.h"
#include "ui/theme/theme.h"

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

QString detailsText(const core::Connection &connection) {
    return QStringList{
        QObject::tr("Host: %1").arg(connection.host),
        QObject::tr("Source: %1:%2").arg(connection.sourceIp, connection.sourcePort),
        QObject::tr("Destination: %1:%2").arg(connection.destinationIp, connection.destinationPort),
        QObject::tr("Network: %1 · %2").arg(connection.network, connection.connectionType),
        QObject::tr("Process: %1").arg(connection.process),
        QObject::tr("Process path: %1").arg(connection.processPath),
        QObject::tr("Proxy chain: %1").arg(connection.chains.join(" → ")),
        QObject::tr("Rule: %1 %2").arg(connection.rule, connection.rulePayload),
        QObject::tr("Uploaded: %1 · Downloaded: %2").arg(formatBytes(connection.upload), formatBytes(connection.download)),
        QObject::tr("Upload speed: %1 · Download speed: %2")
            .arg(formatRate(static_cast<quint64>(connection.uploadRate)), formatRate(static_cast<quint64>(connection.downloadRate))),
        QObject::tr("Started: %1").arg(connection.start.toString(Qt::ISODate)),
        QObject::tr("Closed: %1").arg(connection.end.isValid() ? connection.end.toString(Qt::ISODate) : QObject::tr("Active")),
        QObject::tr("ID: %1").arg(connection.id)
    }.join('\n');
}

qlonglong durationSeconds(const core::Connection &connection) {
    if (!connection.start.isValid()) return 0;
    return qMax<qlonglong>(0, connection.start.secsTo(connection.end.isValid()
                                                       ? connection.end : QDateTime::currentDateTime()));
}

QString connectionDuration(const core::Connection &connection) {
    if (!connection.start.isValid()) return QStringLiteral("—");
    const qlonglong seconds = durationSeconds(connection);
    if (seconds < 60) return QString::number(seconds) + "s";
    if (seconds < 3600) return QString("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QString("%1h %2m").arg(seconds / 3600).arg(seconds / 60 % 60);
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

std::optional<core::Connection> ConnectionModel::connectionAt(int row) const {
    if (row < 0 || row >= connections_.size()) return std::nullopt;
    return connections_.at(row);
}

int ConnectionModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(connections_.size());
}

int ConnectionModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ConnectionModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= connections_.size()) return {};
    const core::Connection &connection = connections_.at(index.row());

    // Sorting reads Qt::UserRole so byte and duration columns order numerically.
    if (role == Qt::UserRole) {
        switch (index.column()) {
            case Upload:
                return QVariant::fromValue<qulonglong>(connection.upload);
            case Download:
                return QVariant::fromValue<qulonglong>(connection.download);
            case UploadRate:
                return connection.uploadRate;
            case DownloadRate:
                return connection.downloadRate;
            case Duration:
                return QVariant::fromValue<qlonglong>(durationSeconds(connection));
            default:
                break;
        }
    } else if (role == Qt::ToolTipRole) {
        // Keep complete values accessible when the table elides them.
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
        case UploadRate:
            return formatRate(static_cast<quint64>(connection.uploadRate));
        case DownloadRate:
            return formatRate(static_cast<quint64>(connection.downloadRate));
        case Duration:
            return connectionDuration(connection);
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
        case UploadRate:
            return tr("Up/s");
        case DownloadRate:
            return tr("Down/s");
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
      proxy_(new QSortFilterProxyModel(this)), renderTimer_(new QTimer(this)) {
    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(100);
    connect(renderTimer_, &QTimer::timeout, this, &ConnectionsPage::renderConnections);
    sampleClock_.start();
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(Qt::UserRole);
    proxy_->setFilterKeyColumn(-1);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);

    auto *filterEdit = new QLineEdit(this);
    filterEdit->setPlaceholderText(tr("Filter host, process, rule or chain…"));
    filterEdit->setClearButtonEnabled(true);
    auto *filterTimer = new QTimer(this);
    filterTimer->setSingleShot(true);
    filterTimer->setInterval(120);
    connect(filterEdit, &QLineEdit::textChanged, filterTimer, [filterTimer] { filterTimer->start(); });
    connect(filterTimer, &QTimer::timeout, this, [this, filterEdit] {
        proxy_->setFilterFixedString(filterEdit->text());
    });

    totalLabel_ = new QLabel("↑ 0 B  ↓ 0 B", this);
    totalLabel_->setObjectName("pageSummary");

    auto *closeAllButton = new QPushButton(tr("Close All"), this);
    connect(closeAllButton, &QPushButton::clicked, client_,
            &core::MihomoClient::closeAllConnections);

    view_ = new QTableView(this);
    view_->setModel(proxy_);
    view_->sortByColumn(-1, Qt::AscendingOrder);
    view_->setSortingEnabled(true);
    view_->setAlternatingRowColors(true);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->verticalHeader()->setVisible(false);
    view_->verticalHeader()->setDefaultSectionSize(QFontMetrics(font()).height() + 12);
    connect(view_, &QTableView::customContextMenuRequested, this,
            &ConnectionsPage::showContextMenu);
    connect(view_, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        const auto connection = model_->connectionAt(proxy_->mapToSource(index).row());
        if (connection) showDetails(*connection);
    });

    QHeaderView *header = view_->horizontalHeader();
    header->setSectionsMovable(true);
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(56);

    // Keep numeric columns readable; horizontal scrolling exposes the extra speed columns.
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
        {ConnectionModel::UploadRate, "999.9 KiB/s"},
        {ConnectionModel::DownloadRate, "999.9 KiB/s"},
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
    auto *historyControls = new QHBoxLayout;
    historyBox_ = new ComboBox(this);
    historyBox_->addItems({tr("Active connections"), tr("Closed connections")});
    auto *clearHistory = new QPushButton(tr("Clear History"), this);
    clearHistory->hide();
    historyControls->addWidget(historyBox_);
    historyControls->addStretch();
    historyControls->addWidget(clearHistory);
    historyControls->addWidget(closeAllButton);
    connect(historyBox_, &QComboBox::currentIndexChanged, this, [this, closeAllButton, clearHistory](int index) {
        closeAllButton->setVisible(index == 0);
        clearHistory->setVisible(index == 1);
        currentDirty_ = closedDirty_ = true;
        renderConnections();
    });
    connect(clearHistory, &QPushButton::clicked, this, [this] {
        closed_.clear(); closedDirty_ = true; renderConnections();
    });

    auto *layout = theme::pageLayout(this);
    layout->addLayout(controls);
    layout->addLayout(historyControls);
    layout->addWidget(view_, 1);

    connect(client_, &core::MihomoClient::connectionsUpdated, this,
            &ConnectionsPage::onConnectionsUpdated);
    const auto reset = [this] {
        previous_.clear();
        current_.clear();
        closed_.clear();
        lastSampleMs_ = -1;
        currentDirty_ = closedDirty_ = true;
        renderConnections();
    };
    connect(client_, &core::MihomoClient::endpointChanged, this, reset);
    connect(client_, &core::MihomoClient::connectedChanged, this, [reset](bool connected) {
        if (!connected) reset();
    });
}

void ConnectionsPage::onConnectionsUpdated(const QVector<core::Connection> &connections,
                                           quint64 uploadTotal, quint64 downloadTotal) {
    const qint64 now = sampleClock_.elapsed();
    const double elapsed = lastSampleMs_ < 0 ? 0 : (now - lastSampleMs_) / 1000.0;
    QHash<QString, core::Connection> next;
    current_.clear();
    current_.reserve(connections.size());
    for (core::Connection connection : connections) {
        connection.uploadRate = 0;
        connection.downloadRate = 0;
        const auto previous = previous_.constFind(connection.id);
        if (previous != previous_.cend() && elapsed > 0) {
            if (connection.upload >= previous->upload)
                connection.uploadRate = (connection.upload - previous->upload) / elapsed;
            if (connection.download >= previous->download)
                connection.downloadRate = (connection.download - previous->download) / elapsed;
        }
        next.insert(connection.id, connection);
        current_.append(connection);
    }
    QVector<core::Connection> endedConnections;
    endedConnections.reserve(qMin(previous_.size(), qsizetype(500)));
    const auto endedAt = QDateTime::currentDateTime();
    for (auto it = previous_.cbegin(); it != previous_.cend(); ++it) {
        if (next.contains(it.key())) continue;
        core::Connection ended = it.value();
        ended.end = endedAt;
        ended.uploadRate = ended.downloadRate = 0;
        endedConnections.append(ended);
        if (endedConnections.size() == 500) break;
    }
    if (!endedConnections.isEmpty()) {
        std::reverse(endedConnections.begin(), endedConnections.end());
        const qsizetype retained = qMin(closed_.size(), 500 - endedConnections.size());
        for (qsizetype i = 0; i < retained; ++i) endedConnections.append(closed_[i]);
        closed_ = std::move(endedConnections);
        closedDirty_ = true;
    }
    previous_ = next;
    lastSampleMs_ = now;
    currentDirty_ = true;
    if (isVisible() && !renderTimer_->isActive()) renderTimer_->start();
    totalLabel_->setText(
        QString("↑ %1  ↓ %2").arg(formatBytes(uploadTotal), formatBytes(downloadTotal)));
}

void ConnectionsPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    renderConnections();
}

void ConnectionsPage::renderConnections() {
    if (!isVisible()) return;
    renderTimer_->stop();
    historyBox_->setItemText(0, tr("Active connections (%1)").arg(current_.size()));
    historyBox_->setItemText(1, tr("Closed connections (%1/500)").arg(closed_.size()));
    const bool closed = historyBox_->currentIndex() == 1;
    if (closed ? !closedDirty_ : !currentDirty_) return;
    const QString selected = model_->idAt(proxy_->mapToSource(view_->currentIndex()).row());
    const int scroll = view_->verticalScrollBar()->value();
    const int horizontalScroll = view_->horizontalScrollBar()->value();
    model_->setConnections(closed ? closed_ : current_);
    (closed ? closedDirty_ : currentDirty_) = false;
    if (!selected.isEmpty()) {
        for (int row = 0; row < model_->rowCount(); ++row) {
            if (model_->idAt(row) != selected) continue;
            view_->setCurrentIndex(proxy_->mapFromSource(model_->index(row, 0)));
            break;
        }
    }
    view_->verticalScrollBar()->setValue(scroll);
    view_->horizontalScrollBar()->setValue(horizontalScroll);
}

void ConnectionsPage::showDetails(const core::Connection &connection) {
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Connection Details"));
    dialog->resize(620, 430);
    auto *layout = new QVBoxLayout(dialog);
    auto *text = new QPlainTextEdit(dialog);
    text->setReadOnly(true);
    text->setPlainText(detailsText(connection));
    layout->addWidget(text);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto *copy = buttons->addButton(tr("Copy Details"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, dialog, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->open();
}

void ConnectionsPage::showContextMenu(const QPoint &pos) {
    const QModelIndex index = view_->indexAt(pos);
    if (!index.isValid()) return;

    const auto connection = model_->connectionAt(proxy_->mapToSource(index).row());
    if (!connection) return;
    const core::Connection snapshot = *connection;
    QMenu menu(this);
    menu.addAction(tr("Details…"), this, [this, snapshot] { showDetails(snapshot); });
    menu.addAction(tr("Copy Host"), this, [snapshot] { QApplication::clipboard()->setText(snapshot.host); });
    menu.addAction(tr("Copy Details"), this, [snapshot] { QApplication::clipboard()->setText(detailsText(snapshot)); });
    if (historyBox_->currentIndex() == 0 && !snapshot.id.isEmpty())
        menu.addAction(tr("Close Connection"), this, [this, snapshot] { client_->closeConnection(snapshot.id); });
    menu.exec(view_->viewport()->mapToGlobal(pos));
}

}  // namespace ui
