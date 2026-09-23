#include "ui/pages/providers/providers_page.h"

#include <QComboBox>
#include "ui/widgets/combo_box.h"
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>
#include <QTimer>
#include <algorithm>

#include "ui/theme/formatting.h"
#include "ui/theme/theme.h"
#include "core/backend/backend_bridge.h"

namespace ui {
namespace {
bool sameProviders(const QVector<core::backend::Provider> &a, const QVector<core::backend::Provider> &b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto &first, const auto &second) {
        return first.name == second.name && first.type == second.type && first.vehicle == second.vehicle &&
               first.behavior == second.behavior && first.count == second.count && first.updated == second.updated &&
               first.used == second.used && first.total == second.total && first.expires == second.expires;
    });
}
}

ProvidersPage::ProvidersPage(core::backend::BackendBridge *bridge, QWidget *parent)
    : QWidget(parent), bridge_(bridge), model_(new QStandardItemModel(this)),
      filter_(new QSortFilterProxyModel(this)), renderTimer_(new QTimer(this)) {
    renderTimer_->setSingleShot(true);
    connect(renderTimer_, &QTimer::timeout, this, &ProvidersPage::renderProviders);
    kind_ = new ComboBox(this);
    kind_->addItem(tr("Proxy providers"), false);
    kind_->addItem(tr("Rule providers"), true);
    auto *search = new QLineEdit(this);
    search->setPlaceholderText(tr("Filter providers…"));
    search->setClearButtonEnabled(true);
    refresh_ = new QPushButton(tr("Refresh"), this);
    update_ = new QPushButton(tr("Update selected"), this);
    updateAll_ = new QPushButton(tr("Update all"), this);
    health_ = new QPushButton(tr("Health check"), this);
    refresh_->setToolTip(tr("Reload provider status from the controller"));
    update_->setToolTip(tr("Ask the core to reload the selected provider from its source"));
    health_->setToolTip(tr("Test all proxies in the selected provider"));

    filter_->setSourceModel(model_);
    filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    filter_->setFilterKeyColumn(-1);
    view_ = new QTableView(this);
    view_->setModel(filter_);
    view_->setSortingEnabled(true);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->verticalHeader()->hide();
    view_->verticalHeader()->setDefaultSectionSize(QFontMetrics(font()).height() + 12);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setHighlightSections(false);

    status_ = new QLabel(tr("Provider status is loaded from the running core."), this);
    status_->setObjectName("fieldLabel");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    auto *controls = new QHBoxLayout;
    controls->setSpacing(theme::kPageSpacing);
    controls->addWidget(kind_);
    controls->addWidget(search, 1);
    controls->addWidget(refresh_);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(theme::kPageSpacing);
    actions->addWidget(update_);
    actions->addWidget(updateAll_);
    actions->addWidget(health_);
    actions->addStretch();
    auto *layout = theme::pageLayout(this);
    layout->addLayout(controls);
    layout->addLayout(actions);
    layout->addWidget(view_, 1);
    layout->addWidget(status_);

    auto *filterTimer = new QTimer(this);
    filterTimer->setSingleShot(true);
    filterTimer->setInterval(120);
    connect(search, &QLineEdit::textChanged, filterTimer, [filterTimer] { filterTimer->start(); });
    connect(filterTimer, &QTimer::timeout, this, [this, search] {
        filter_->setFilterFixedString(search->text());
    });
    connect(refresh_, &QPushButton::clicked, this, &ProvidersPage::refresh);
    connect(kind_, &QComboBox::currentIndexChanged, this, [this] {
        populate(kind_->currentData().toBool(), {});
        refresh();
    });
    connect(update_, &QPushButton::clicked, this, [this] {
        bridge_->updateProvider(kind_->currentData().toBool(), selectedName());
    });
    connect(updateAll_, &QPushButton::clicked, this, [this] {
        const bool rules = kind_->currentData().toBool();
        for (int row = 0; row < model_->rowCount(); ++row) {
            bridge_->updateProvider(rules, model_->item(row, 0)->text());
        }
    });
    connect(health_, &QPushButton::clicked, this, [this] {
        bridge_->healthCheckProvider(selectedName());
    });
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &ProvidersPage::updateActions);
    connect(bridge_, &core::backend::BackendBridge::providersReceived, this,
            &ProvidersPage::populate);
    connect(bridge_, &core::backend::BackendBridge::providerBusyChanged, this, [this](bool busy) {
        busy_ = busy;
        updateActions();
    });
    // providerError, not errorOccurred: the bridge re-aims a provider failure
    // onto this channel so it keeps landing on this label rather than becoming
    // a main-window status-bar toast it never was.
    connect(bridge_, &core::backend::BackendBridge::providerError, this,
            [this](const QString &message) { status_->setText(message); });
    connect(bridge_, &core::backend::BackendBridge::providerOperationFinished, status_,
            &QLabel::setText);
    connect(bridge_, &core::backend::BackendBridge::endpointChanged, this, [this] {
        populate(kind_->currentData().toBool(), {});
        if (isVisible()) refresh();
    });
    populate(false, {});
}

void ProvidersPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    renderProviders();
    refresh();
}

void ProvidersPage::refresh() {
    status_->setText(tr("Loading providers…"));
    bridge_->fetchProviders(kind_->currentData().toBool());
}

QString ProvidersPage::selectedName() const {
    const auto rows = view_->selectionModel()->selectedRows();
    return rows.isEmpty() ? QString() : rows.first().data().toString();
}

void ProvidersPage::populate(bool rules, const QVector<core::backend::Provider> &providers) {
    if (rules != kind_->currentData().toBool()) return;
    status_->setText(providers.isEmpty() ? tr("No providers are configured in the running core.")
                                         : tr("%n provider(s)", nullptr, providers.size()));
    if (cachedRules_ == rules && sameProviders(cachedProviders_, providers) && !dirty_) return;
    cachedRules_ = rules;
    cachedProviders_ = providers;
    dirty_ = true;
    if (isVisible() && !renderTimer_->isActive()) renderTimer_->start(0);
}

void ProvidersPage::renderProviders() {
    if (!isVisible() || !dirty_) return;
    renderTimer_->stop();
    dirty_ = false;
    const bool rules = cachedRules_;
    const QString selected = selectedName();
    const int sortColumn = filter_->sortColumn();
    const auto sortOrder = filter_->sortOrder();
    view_->setUpdatesEnabled(false);
    filter_->setDynamicSortFilter(false);
    model_->clear();
    model_->setHorizontalHeaderLabels({tr("Name"), tr("Source"), tr("Type"),
                                      rules ? tr("Rules") : tr("Proxies"), tr("Updated"),
                                      rules ? tr("Behavior") : tr("Usage / expiry")});
    model_->setRowCount(cachedProviders_.size());
    int row = 0;
    for (const auto &provider : cachedProviders_) {
        const QString updated = provider.updated.isValid()
                                    ? provider.updated.toLocalTime().toString("yyyy-MM-dd HH:mm")
                                    : tr("Not reported");
        QString details = provider.behavior;
        if (!rules) {
            details = provider.total > 0
                          ? tr("%1 / %2").arg(formatBytes(provider.used), formatBytes(provider.total))
                          : tr("Not reported");
            if (provider.expires.isValid()) {
                details += tr(" · expires %1").arg(provider.expires.toLocalTime().toString("yyyy-MM-dd"));
            }
        }
        auto *count = new QStandardItem;
        count->setData(provider.count, Qt::DisplayRole);
        const QList<QStandardItem *> cells{new QStandardItem(provider.name), new QStandardItem(provider.vehicle),
            new QStandardItem(provider.type), count, new QStandardItem(updated), new QStandardItem(details)};
        for (int column = 0; column < cells.size(); ++column) model_->setItem(row, column, cells[column]);
        ++row;
    }
    filter_->setDynamicSortFilter(true);
    filter_->invalidate();
    filter_->sort(sortColumn, sortOrder);
    for (int row = 0; row < filter_->rowCount(); ++row) {
        if (filter_->index(row, 0).data().toString() == selected) view_->selectRow(row);
    }
    view_->setColumnWidth(0, 180);
    view_->setColumnWidth(1, 90);
    view_->setColumnWidth(2, 90);
    view_->setColumnWidth(3, 70);
    view_->setColumnWidth(4, 150);
    view_->setUpdatesEnabled(true);
    updateActions();
}

void ProvidersPage::updateActions() {
    const bool selected = !selectedName().isEmpty();
    refresh_->setEnabled(!busy_);
    update_->setEnabled(!busy_ && selected);
    updateAll_->setEnabled(!busy_ && model_->rowCount() > 0);
    health_->setVisible(!kind_->currentData().toBool());
    health_->setEnabled(!busy_ && selected);
}

}  // namespace ui
