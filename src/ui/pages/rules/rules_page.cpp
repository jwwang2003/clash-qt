#include "ui/pages/rules/rules_page.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

#include "core/mihomo/mihomo_client.h"
#include "ui/theme/theme.h"

namespace ui {

RuleModel::RuleModel(QObject *parent) : QAbstractTableModel(parent) {}

void RuleModel::setRules(const QVector<core::Rule> &rules) {
    beginResetModel();
    rules_ = rules;
    endResetModel();
}

int RuleModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rules_.size());
}

int RuleModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant RuleModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rules_.size() ||
        (role != Qt::DisplayRole && role != Qt::ToolTipRole)) return {};

    const core::Rule &rule = rules_.at(index.row());
    switch (index.column()) {
        case Type:
            return rule.type;
        case Payload:
            return rule.payload;
        case Proxy:
            return rule.proxy;
        default:
            return {};
    }
}

QVariant RuleModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case Type:
            return tr("Type");
        case Payload:
            return tr("Payload");
        case Proxy:
            return tr("Proxy");
        default:
            return {};
    }
}

RulesPage::RulesPage(core::MihomoClient *client, QWidget *parent)
    : QWidget(parent),
      client_(client),
      model_(new RuleModel(this)),
      proxy_(new QSortFilterProxyModel(this)) {
    proxy_->setSourceModel(model_);
    proxy_->setFilterKeyColumn(-1);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);

    auto *filterEdit = new QLineEdit(this);
    filterEdit->setPlaceholderText(tr("Filter type, payload or proxy…"));
    filterEdit->setClearButtonEnabled(true);
    connect(filterEdit, &QLineEdit::textChanged, proxy_,
            &QSortFilterProxyModel::setFilterFixedString);

    view_ = new QTableView(this);
    view_->setModel(proxy_);
    // Routing rules are first-match; keep the controller order until the user sorts.
    view_->sortByColumn(-1, Qt::AscendingOrder);
    view_->setSortingEnabled(true);
    view_->setAlternatingRowColors(true);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->verticalHeader()->setVisible(false);
    view_->verticalHeader()->setDefaultSectionSize(QFontMetrics(font()).height() + 12);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setHighlightSections(false);
    view_->setColumnWidth(RuleModel::Type, 160);
    view_->setColumnWidth(RuleModel::Payload, 380);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(theme::kPageSpacing);
    controls->addWidget(filterEdit, 1);
    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    connect(refreshButton, &QPushButton::clicked, client_, &core::MihomoClient::fetchRules);
    controls->addWidget(refreshButton);
    auto *layout = theme::pageLayout(this);
    layout->addLayout(controls);
    layout->addWidget(view_, 1);

    connect(client_, &core::MihomoClient::rulesUpdated, this,
            [this](const QVector<core::Rule> &rules) { model_->setRules(rules); });
}

}  // namespace ui
