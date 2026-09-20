#include "ui/proxies_page.h"

#include <QApplication>
#include <QComboBox>
#include "ui/combo_box.h"
#include <QLineEdit>
#include <QMenu>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSettings>
#include <QShowEvent>
#include <QTimer>
#include <algorithm>
#include <climits>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include "core/mihomo_client.h"
#include "ui/formatting.h"
#include "ui/theme.h"

namespace ui {
namespace {

constexpr int kSubtitleRole = Qt::UserRole + 1;
constexpr int kTrailingRole = Qt::UserRole + 2;
constexpr int kDelayRole = Qt::UserRole + 3;
constexpr int kActiveRole = Qt::UserRole + 4;

constexpr int kRowPadH = 10;
constexpr int kRowPadV = 6;
constexpr int kMarkerWidth = 16;
constexpr int kGap = 12;
constexpr int kHintWidth = 200;
constexpr int kInkSlack = 2;

bool sameGroups(const QVector<core::ProxyGroup> &a, const QVector<core::ProxyGroup> &b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto &first, const auto &second) {
        return first.name == second.name && first.type == second.type && first.now == second.now &&
               first.all == second.all && first.fixed == second.fixed;
    });
}

bool sameNodes(const QHash<QString, core::ProxyNode> &a, const QHash<QString, core::ProxyNode> &b) {
    if (a.size() != b.size()) return false;
    for (auto it = a.cbegin(); it != a.cend(); ++it) {
        const auto other = b.constFind(it.key());
        if (other == b.cend() || it->name != other->name || it->type != other->type || it->delay != other->delay)
            return false;
    }
    return true;
}

QColor delayColor(int delay) {
    const theme::Tokens &t = theme::tokens();
    if (delay < 0) return t.textFaint;
    if (delay == 0) return t.danger;
    if (delay < 200) return t.success;
    if (delay < 500) return t.warning;
    return t.danger;
}

/// Paints both proxy lists: a group over the member it currently routes to,
/// and a node with its type and latency on the right.
class ProxyItemDelegate : public QStyledItemDelegate {
public:
    enum class Mode { Group, Node };

    ProxyItemDelegate(Mode mode, QObject *parent) : QStyledItemDelegate(parent), mode_(mode) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const theme::Tokens &t = theme::tokens();
        const bool active = index.data(kActiveRole).toBool();
        const bool highlighted = active || (option.state & QStyle::State_Selected);
        const QRect body = option.rect.adjusted(kRowPadH, kRowPadV, -kRowPadH, -kRowPadV);

        QFont nameFont = option.font;
        nameFont.setBold(active || mode_ == Mode::Group);
        const QFontMetrics nameMetrics(nameFont);
        const QFont smallFont = subtitleFont(option.font);
        const QFontMetrics smallMetrics(smallFont);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect line(body.left(), body.top(), body.width(), nameMetrics.height());
        int right = line.right();

        if (mode_ == Mode::Node) {
            QFont delayFont = option.font;
            delayFont.setBold(true);
            const QString delay = formatDelay(index.data(kDelayRole).toInt());
            right = drawTrailing(painter, line, right, delay, delayFont,
                                 highlighted && index.data(kDelayRole).toInt() < 0
                                     ? t.textDim : delayColor(index.data(kDelayRole).toInt()));
        }
        right = drawTrailing(painter, line, right, index.data(kTrailingRole).toString(), smallFont,
                             highlighted ? t.text : (mode_ == Mode::Group ? t.textDim : t.textFaint));

        int left = body.left();
        if (mode_ == Mode::Node) {
            if (active) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(t.accent);
                painter->drawEllipse(QPointF(left + 4, line.center().y() + 1), 3.5, 3.5);
                painter->setBrush(Qt::NoBrush);
            }
            left += kMarkerWidth;
        }

        painter->setFont(nameFont);
        painter->setPen(t.text);
        painter->drawText(QRect(left, line.top(), qMax(0, right - left), line.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                 Qt::ElideRight, qMax(0, right - left)));

        const QString subtitle = index.data(kSubtitleRole).toString();
        if (mode_ == Mode::Group && !subtitle.isEmpty()) {
            const QRect below(body.left(), line.bottom() + 2, body.width(), smallMetrics.height());
            painter->setFont(smallFont);
            painter->setPen(t.textDim);
            painter->drawText(below, Qt::AlignLeft | Qt::AlignVCenter,
                              smallMetrics.elidedText(subtitle, Qt::ElideRight, below.width()));
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        QFont nameFont = option.font;
        nameFont.setBold(true);
        int height = 2 * kRowPadV + QFontMetrics(nameFont).height();
        if (mode_ == Mode::Group) height += 2 + QFontMetrics(subtitleFont(option.font)).height();
        return QSize(kHintWidth, height);
    }

private:
    static QFont subtitleFont(QFont font) {
        if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() - 1.0);
        return font;
    }

    static int drawTrailing(QPainter *painter, const QRect &line, int right, const QString &text,
                            const QFont &font, const QColor &color) {
        if (text.isEmpty()) return right;

        // The advance can be narrower than the glyphs' ink, which slices the
        // first character off a rect sized to it exactly.
        const int width = QFontMetrics(font).horizontalAdvance(text) + kInkSlack;
        painter->setFont(font);
        painter->setPen(color);
        painter->drawText(QRect(right - width, line.top(), width, line.height()),
                          Qt::AlignRight | Qt::AlignVCenter, text);
        return right - width - kGap;
    }

    Mode mode_;
};

}  // namespace

ProxiesPage::ProxiesPage(core::MihomoClient *client, QWidget *parent)
    : QWidget(parent), client_(client), renderTimer_(new QTimer(this)), filterTimer_(new QTimer(this)) {
    renderTimer_->setSingleShot(true);
    connect(renderTimer_, &QTimer::timeout, this, &ProxiesPage::renderPending);
    filterTimer_->setSingleShot(true);
    filterTimer_->setInterval(120);
    connect(filterTimer_, &QTimer::timeout, this, [this] { nodesDirty_ = true; renderPending(); });
    summaryLabel_ = new QLabel(tr("No proxy groups available"), this);
    summaryLabel_->setObjectName("pageSummary");

    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    connect(refreshButton, &QPushButton::clicked, this, &ProxiesPage::refresh);

    testButton_ = new QPushButton(tr("Test Latency"), this);
    testButton_->setToolTip(tr("Measure the delay of every node in the selected group"));
    testButton_->setEnabled(false);
    connect(testButton_, &QPushButton::clicked, this, &ProxiesPage::testActiveGroup);

    groupList_ = new QListWidget(this);
    groupList_->setObjectName("proxyGroups");
    groupList_->setAccessibleName(tr("Proxy groups"));
    groupList_->setMaximumWidth(280);
    groupList_->setUniformItemSizes(true);
    groupList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    groupList_->setItemDelegate(new ProxyItemDelegate(ProxyItemDelegate::Mode::Group, this));
    connect(groupList_, &QListWidget::currentRowChanged, this, &ProxiesPage::onGroupRowChanged);
    groupList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(groupList_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const auto *item = groupList_->itemAt(pos);
        if (!item) return;
        const auto found = std::find_if(groups_.cbegin(), groups_.cend(), [item](const auto &group) {
            return group.name == item->text();
        });
        if (found == groups_.cend()) return;
        const auto group = *found;
        if (group.type != "URLTest" && group.type != "Fallback") return;
        QMenu menu(this);
        menu.addAction(tr("Use Automatic Selection"), this,
                       [this, group] { client_->resetGroupSelection(group.name); });
        menu.exec(groupList_->viewport()->mapToGlobal(pos));
    });

    nodeList_ = new QListWidget(this);
    nodeList_->setObjectName("proxyNodes");
    nodeList_->setAccessibleName(tr("Proxy nodes"));
    nodeList_->setUniformItemSizes(true);
    nodeList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nodeList_->setItemDelegate(new ProxyItemDelegate(ProxyItemDelegate::Mode::Node, this));
    connect(nodeList_, &QListWidget::activated, this,
            [this](const QModelIndex &index) { onNodeActivated(index.row()); });
    connect(nodeList_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { onNodeActivated(nodeList_->row(item)); });

    nodeList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(nodeList_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto *item = nodeList_->itemAt(pos);
        if (!item) return;
        const QString name = item->text();
        QMenu menu(this);
        menu.addAction(tr("Test Node Latency"), this, [this, name] { client_->testNodeDelay(name); });
        menu.exec(nodeList_->viewport()->mapToGlobal(pos));
    });

    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(tr("Filter nodes…"));
    filterEdit_->setClearButtonEnabled(true);
    connect(filterEdit_, &QLineEdit::textChanged, this, [this] { filterTimer_->start(); });
    sortBox_ = new ComboBox(this);
    sortBox_->addItems({tr("Profile order"), tr("Name"), tr("Latency")});
    sortBox_->setCurrentIndex(qBound(0, QSettings("clash-qt", "clash-qt").value("proxies/sort", 0).toInt(), 2));
    connect(sortBox_, &QComboBox::currentIndexChanged, this, [](int index) {
        QSettings("clash-qt", "clash-qt").setValue("proxies/sort", index);
    });
    connect(sortBox_, &QComboBox::currentIndexChanged, this, [this] { nodesDirty_ = true; renderPending(); });
    auto *filters = new QHBoxLayout;
    filters->addWidget(filterEdit_, 1);
    filters->addWidget(sortBox_);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(groupList_);
    splitter->addWidget(nodeList_);
    splitter->setStretchFactor(1, 1);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(theme::kPageSpacing);
    controls->addWidget(summaryLabel_, 1);
    controls->addWidget(refreshButton);
    controls->addWidget(testButton_);

    auto *layout = theme::pageLayout(this);
    layout->addLayout(controls);
    layout->addLayout(filters);
    layout->addWidget(splitter, 1);

    connect(client_, &core::MihomoClient::proxiesUpdated, this, &ProxiesPage::onProxiesUpdated);
}

void ProxiesPage::refresh() { client_->fetchProxies(); }

void ProxiesPage::testActiveGroup() {
    if (!activeGroup_.isEmpty()) client_->testGroupDelay(activeGroup_);
}

void ProxiesPage::onProxiesUpdated(const QVector<core::ProxyGroup> &groups,
                                   const QHash<QString, core::ProxyNode> &nodes) {
    const bool changedGroups = !sameGroups(groups_, groups);
    const bool changedNodes = !sameNodes(nodes_, nodes);
    if (!changedGroups && !changedNodes) return;
    groups_ = groups;
    nodes_ = nodes;
    groupsDirty_ |= changedGroups;
    nodesDirty_ = true;
    if (isVisible() && !renderTimer_->isActive()) renderTimer_->start(0);
}

void ProxiesPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    renderPending();
}

void ProxiesPage::renderPending() {
    if (!isVisible()) return;
    renderTimer_->stop();
    if (groupsDirty_) { renderGroups(); groupsDirty_ = false; }
    if (nodesDirty_) { renderNodes(); nodesDirty_ = false; }
}

void ProxiesPage::renderGroups() {
    const QString previous = activeGroup_;
    const int groupScroll = groupList_->verticalScrollBar()->value();
    {
        QSignalBlocker blocker(groupList_);
        groupList_->clear();
        for (const core::ProxyGroup &group : groups_) {
            auto *item = new QListWidgetItem(group.name, groupList_);
            item->setData(kSubtitleRole,
                          group.now.isEmpty() ? tr("no member") : "→ " + group.now);
            item->setData(kTrailingRole, group.type);
        }
    }

    int restored = 0;
    for (int i = 0; i < groups_.size(); ++i) {
        if (groups_[i].name == previous) {
            restored = i;
            break;
        }
    }
    if (!groups_.isEmpty()) {
        const QSignalBlocker blocker(groupList_);
        groupList_->setCurrentRow(restored);
        activeGroup_ = groups_[restored].name;
    } else {
        activeGroup_.clear();
    }
    groupList_->verticalScrollBar()->setValue(groupScroll);
}

void ProxiesPage::onGroupRowChanged(int row) {
    const auto *item = groupList_->item(row);
    if (!item) return;
    activeGroup_ = item->text();
    nodesDirty_ = true;
    renderPending();
}

void ProxiesPage::renderNodes() {
    const int scroll = nodeList_->verticalScrollBar()->value();
    const QString focused = nodeList_->currentItem() ? nodeList_->currentItem()->text() : QString();
    const QSignalBlocker blocker(nodeList_);
    nodeList_->clear();

    const auto it = std::find_if(groups_.begin(), groups_.end(), [this](const core::ProxyGroup &g) {
        return g.name == activeGroup_;
    });
    testButton_->setEnabled(it != groups_.end());
    if (it == groups_.end()) {
        summaryLabel_->setText(tr("No proxy groups available"));
        return;
    }

    QStringList members;
    const QString filter = filterEdit_->text();
    members.reserve(it->all.size());
    for (const auto &member : it->all)
        if (member.contains(filter, Qt::CaseInsensitive)) members.append(member);
    if (sortBox_->currentIndex() == 1) {
        std::stable_sort(members.begin(), members.end(), [](const QString &a, const QString &b) {
            return a.localeAwareCompare(b) < 0;
        });
    } else if (sortBox_->currentIndex() == 2) {
        std::stable_sort(members.begin(), members.end(), [this](const QString &a, const QString &b) {
            const int first = nodes_.value(a).delay;
            const int second = nodes_.value(b).delay;
            return (first > 0 ? first : INT_MAX) < (second > 0 ? second : INT_MAX);
        });
    }
    for (const QString &member : members) {
        const core::ProxyNode node = nodes_.value(member);
        auto *item = new QListWidgetItem(member, nodeList_);
        item->setData(kTrailingRole, node.type);
        item->setData(kDelayRole, node.delay);
        item->setData(kActiveRole, member == it->now);
        item->setToolTip(tr("%1\n%2 · %3").arg(member, node.type, formatDelay(node.delay)));
        if (member == focused || (focused.isEmpty() && member == it->now))
            nodeList_->setCurrentItem(item);
    }

    // URLTest/Fallback allow a manual pin; clicking the pinned node releases it.
    const bool selectable = it->selectable();
    nodeList_->setSelectionMode(selectable ? QAbstractItemView::SingleSelection
                                           : QAbstractItemView::NoSelection);

    QStringList summary{it->name, it->type, tr("%1 nodes").arg(it->all.size())};
    if (!it->fixed.isEmpty()) summary << tr("pinned: %1").arg(it->fixed);
    else if (it->type != "Selector") summary << tr("picks automatically");
    summaryLabel_->setText(summary.join(" · "));
    nodeList_->verticalScrollBar()->setValue(scroll);
}

void ProxiesPage::onNodeActivated(int row) {
    const auto it = std::find_if(groups_.begin(), groups_.end(), [this](const core::ProxyGroup &g) {
        return g.name == activeGroup_;
    });
    if (it == groups_.end() || !it->selectable()) return;
    if (row < 0 || row >= nodeList_->count()) return;
    const QString node = nodeList_->item(row)->text();
    if (it->type != "Selector" && node == it->fixed) client_->resetGroupSelection(activeGroup_);
    else if (node != it->now || it->type != "Selector") client_->selectNode(activeGroup_, node);
}

}  // namespace ui
