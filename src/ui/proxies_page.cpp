#include "ui/proxies_page.h"

#include <QApplication>
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
                                 delayColor(index.data(kDelayRole).toInt()));
        }
        right = drawTrailing(painter, line, right, index.data(kTrailingRole).toString(), smallFont,
                             mode_ == Mode::Group ? t.textDim : t.textFaint);

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
    : QWidget(parent), client_(client) {
    summaryLabel_ = new QLabel(this);
    summaryLabel_->setObjectName("pageSummary");

    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    connect(refreshButton, &QPushButton::clicked, this, &ProxiesPage::refresh);

    testButton_ = new QPushButton(tr("Test Latency"), this);
    testButton_->setToolTip(tr("Measure the delay of every node in the selected group"));
    testButton_->setEnabled(false);
    connect(testButton_, &QPushButton::clicked, this, &ProxiesPage::testActiveGroup);

    groupList_ = new QListWidget(this);
    groupList_->setMaximumWidth(280);
    groupList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    groupList_->setItemDelegate(new ProxyItemDelegate(ProxyItemDelegate::Mode::Group, this));
    connect(groupList_, &QListWidget::currentRowChanged, this, &ProxiesPage::onGroupRowChanged);

    nodeList_ = new QListWidget(this);
    nodeList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nodeList_->setItemDelegate(new ProxyItemDelegate(ProxyItemDelegate::Mode::Node, this));
    connect(nodeList_, &QListWidget::activated, this,
            [this](const QModelIndex &index) { onNodeActivated(index.row()); });
    connect(nodeList_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { onNodeActivated(nodeList_->row(item)); });

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
    layout->addWidget(splitter, 1);

    connect(client_, &core::MihomoClient::proxiesUpdated, this, &ProxiesPage::onProxiesUpdated);
}

void ProxiesPage::refresh() { client_->fetchProxies(); }

void ProxiesPage::testActiveGroup() {
    if (!activeGroup_.isEmpty()) client_->testGroupDelay(activeGroup_);
}

void ProxiesPage::onProxiesUpdated(const QVector<core::ProxyGroup> &groups,
                                   const QHash<QString, core::ProxyNode> &nodes) {
    groups_ = groups;
    nodes_ = nodes;

    const QString previous = activeGroup_;
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
        groupList_->setCurrentRow(restored);
        activeGroup_ = groups_[restored].name;
        renderNodes();
    }
}

void ProxiesPage::onGroupRowChanged(int row) {
    if (row < 0 || row >= groups_.size()) return;
    activeGroup_ = groups_[row].name;
    renderNodes();
}

void ProxiesPage::renderNodes() {
    nodeList_->clear();

    const auto it = std::find_if(groups_.begin(), groups_.end(), [this](const core::ProxyGroup &g) {
        return g.name == activeGroup_;
    });
    testButton_->setEnabled(it != groups_.end());
    if (it == groups_.end()) {
        summaryLabel_->clear();
        return;
    }

    for (const QString &member : it->all) {
        const core::ProxyNode node = nodes_.value(member);
        auto *item = new QListWidgetItem(member, nodeList_);
        item->setData(kTrailingRole, node.type);
        item->setData(kDelayRole, node.delay);
        item->setData(kActiveRole, member == it->now);
        if (member == it->now) nodeList_->setCurrentItem(item);
    }

    // Non-Selector groups (URLTest/Fallback) pick their own member; show, don't offer.
    const bool selectable = it->selectable();
    nodeList_->setSelectionMode(selectable ? QAbstractItemView::SingleSelection
                                           : QAbstractItemView::NoSelection);

    QStringList summary{it->name, it->type, tr("%1 nodes").arg(it->all.size())};
    if (!selectable) summary << tr("picks automatically");
    summaryLabel_->setText(summary.join(" · "));
}

void ProxiesPage::onNodeActivated(int row) {
    const auto it = std::find_if(groups_.begin(), groups_.end(), [this](const core::ProxyGroup &g) {
        return g.name == activeGroup_;
    });
    if (it == groups_.end() || !it->selectable()) return;
    if (row < 0 || row >= it->all.size()) return;

    client_->selectNode(activeGroup_, it->all.at(row));
}

}  // namespace ui
