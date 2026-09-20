#include "ui/profiles_page.h"

#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/formatting.h"
#include "ui/theme.h"

namespace ui {
namespace {

constexpr int kCardMargin = 4;
constexpr int kAccentWidth = 3;
constexpr int kPad = 12;
constexpr int kPadV = 8;
constexpr int kEditorWidth = 140;
constexpr int kBarWidth = 160;
constexpr int kBarHeight = 6;

const QVector<int> &intervalPresets() {
    static const QVector<int> presets{0, 30, 60, 180, 360, 720, 1440};
    return presets;
}

QString intervalLabel(int minutes) {
    if (minutes <= 0) return QObject::tr("Manual");
    if (minutes % 1440 == 0) return QObject::tr("Every %1 d").arg(minutes / 1440);
    if (minutes % 60 == 0) return QObject::tr("Every %1 h").arg(minutes / 60);
    return QObject::tr("Every %1 min").arg(minutes);
}

QColor usageColor(double ratio) {
    const theme::Tokens &t = theme::tokens();
    if (ratio >= 0.9) return t.danger;
    if (ratio >= 0.75) return t.warning;
    return t.success;
}

int drawChip(QPainter *painter, int x, const QRect &line, const QString &label, const QFont &font,
             const QColor &foreground, const QColor &background) {
    const QFontMetrics metrics(font);
    const QRect chip(x, line.center().y() - (metrics.height() + 2) / 2,
                     metrics.horizontalAdvance(label) + 12, metrics.height() + 2);

    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(background);
    painter->drawRoundedRect(chip, 3, 3);
    painter->setFont(font);
    painter->setPen(foreground);
    painter->drawText(chip, Qt::AlignCenter, label);
    painter->restore();

    return chip.right() + 6;
}

}  // namespace

ProfileModel::ProfileModel(QObject *parent) : QAbstractListModel(parent) {}

void ProfileModel::setProfiles(const QVector<core::Profile> &profiles, const QString &currentUid) {
    beginResetModel();
    profiles_ = profiles;
    currentUid_ = currentUid;
    endResetModel();
}

core::Profile ProfileModel::profileAt(const QModelIndex &index) const {
    if (index.row() < 0 || index.row() >= profiles_.size()) return {};
    return profiles_.at(index.row());
}

int ProfileModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(profiles_.size());
}

QVariant ProfileModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) return {};

    const core::Profile &profile = profiles_.at(index.row());
    switch (role) {
        case Qt::DisplayRole:
            return profile.name;
        case Qt::ToolTipRole:
            return profile.remote ? profile.url : profile.filePath;
        case ProfileRole:
            return QVariant::fromValue(profile);
        case CurrentRole:
            return profile.uid == currentUid_;
        case IntervalRole:
            return profile.updateIntervalMinutes;
        default:
            return {};
    }
}

bool ProfileModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (!index.isValid() || role != IntervalRole) return false;

    core::Profile &profile = profiles_[index.row()];
    const int minutes = value.toInt();
    if (profile.updateIntervalMinutes == minutes) return false;

    profile.updateIntervalMinutes = minutes;
    emit dataChanged(index, index, {IntervalRole});
    emit updateIntervalEdited(profile.uid, minutes);
    return true;
}

Qt::ItemFlags ProfileModel::flags(const QModelIndex &index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

ProfileDelegate::ProfileDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void ProfileDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const {
    const core::Profile profile = index.data(ProfileModel::ProfileRole).value<core::Profile>();
    const bool current = index.data(ProfileModel::CurrentRole).toBool();
    const bool selected = option.state.testFlag(QStyle::State_Selected);

    // The card paints itself, so it reads the same tokens as the stylesheet
    // instead of drifting from it.
    const theme::Tokens &t = theme::tokens();
    const QColor accent = t.accent;
    const QColor base = t.surface;
    const QColor text = t.text;
    const QColor dim = t.textDim;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect cardRect =
        option.rect.adjusted(kCardMargin, kCardMargin, -kCardMargin, -kCardMargin);
    QPainterPath card;
    card.addRoundedRect(QRectF(cardRect).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);

    // The rail and the ACTIVE chip mark the current profile; the fill and the
    // border stay at the weight every other card carries.
    painter->fillPath(card, current    ? t.accentFaint
                            : selected ? theme::blend(base, accent, 0.08)
                                       : base);
    if (current) {
        painter->save();
        painter->setClipPath(card);
        painter->fillRect(QRect(cardRect.left(), cardRect.top(), kAccentWidth, cardRect.height()),
                          accent);
        painter->restore();
    }
    painter->setPen(QPen(current || selected ? accent : t.border, 1));
    painter->drawPath(card);

    const QRect content = cardRect.adjusted(kAccentWidth + kPad, kPadV, -kPad, -kPadV);
    const QFontMetrics metrics(option.font);

    QFont nameFont = option.font;
    nameFont.setBold(true);
    const QFontMetrics nameMetrics(nameFont);

    QFont chipFont = option.font;
    chipFont.setBold(true);
    if (chipFont.pointSizeF() > 0) chipFont.setPointSizeF(chipFont.pointSizeF() - 1.0);

    // The update-interval editor floats over the top-right of the card; text stays clear of it.
    const int reserved = profile.remote ? kEditorWidth + kPad : 0;
    QRect line(content.left(), content.top(), content.width() - reserved,
               nameMetrics.height() + 2);

    const QString name =
        nameMetrics.elidedText(profile.name, Qt::ElideRight, qMax(80, line.width() - 160));
    painter->setFont(nameFont);
    painter->setPen(text);
    painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter, name);

    int chipX = line.left() + nameMetrics.horizontalAdvance(name) + 10;
    if (current) {
        chipX = drawChip(painter, chipX, line, tr("ACTIVE"), chipFont, t.accentText, accent);
    }
    drawChip(painter, chipX, line, profile.remote ? tr("REMOTE") : tr("LOCAL"), chipFont, dim,
             theme::blend(base, text, 0.12));

    line = QRect(content.left(), line.bottom() + 4, content.width() - reserved, metrics.height());
    QStringList meta;
    meta << (profile.updated.isValid()
                 ? tr("updated %1 ago").arg(formatDuration(profile.updated))
                 : tr("never updated"));
    const QString source = profile.remote ? redactUrl(profile.url) : profile.filePath;
    if (!source.isEmpty()) meta << source;

    painter->setFont(option.font);
    painter->setPen(dim);
    painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                      metrics.elidedText(meta.join(" · "), Qt::ElideRight, line.width()));

    if (!profile.subscription.isEmpty()) {
        const core::SubscriptionInfo &info = profile.subscription;
        const quint64 used = info.upload + info.download;
        const double ratio =
            info.total > 0
                ? qBound(0.0, static_cast<double>(used) / static_cast<double>(info.total), 1.0)
                : 0.0;

        line = QRect(content.left(), line.bottom() + 6, content.width(), metrics.height());
        const QRect track(line.left(), line.center().y() - kBarHeight / 2, kBarWidth, kBarHeight);

        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::blend(base, text, 0.15));
        painter->drawRoundedRect(track, kBarHeight / 2.0, kBarHeight / 2.0);
        if (ratio > 0.0) {
            painter->setBrush(usageColor(ratio));
            painter->drawRoundedRect(
                QRect(track.left(), track.top(),
                      qMax(kBarHeight, static_cast<int>(track.width() * ratio)), track.height()),
                kBarHeight / 2.0, kBarHeight / 2.0);
        }

        QStringList usage;
        usage << (info.total > 0
                      ? tr("%1 of %2").arg(formatBytes(used), formatBytes(info.total))
                      : tr("%1 used").arg(formatBytes(used)));
        if (info.expire.isValid()) {
            usage << tr("expires %1").arg(info.expire.toString("yyyy-MM-dd"));
        }

        const QRect label(track.right() + kPad, line.top(), line.right() - track.right() - kPad,
                          line.height());
        painter->setBrush(Qt::NoBrush);
        painter->setPen(dim);
        painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(usage.join(" · "), Qt::ElideRight, label.width()));
    }

    painter->restore();
}

QSize ProfileDelegate::sizeHint(const QStyleOptionViewItem &option,
                                const QModelIndex &index) const {
    const QFontMetrics metrics(option.font);
    int height = 2 * (kCardMargin + kPadV) + metrics.height() + 2 + 4 + metrics.height();
    if (!index.data(ProfileModel::ProfileRole).value<core::Profile>().subscription.isEmpty()) {
        height += 6 + metrics.height();
    }
    return QSize(option.rect.width(), height);
}

QWidget *ProfileDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &,
                                       const QModelIndex &) const {
    auto *box = new QComboBox(parent);
    box->setToolTip(tr("How often this subscription is refreshed"));
    for (int minutes : intervalPresets()) box->addItem(intervalLabel(minutes), minutes);

    auto *self = const_cast<ProfileDelegate *>(this);
    connect(box, &QComboBox::activated, self, [self, box] { emit self->commitData(box); });
    return box;
}

void ProfileDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const {
    auto *box = qobject_cast<QComboBox *>(editor);
    const int minutes = index.data(ProfileModel::IntervalRole).toInt();

    int row = box->findData(minutes);
    if (row < 0) {
        box->addItem(intervalLabel(minutes), minutes);
        row = box->count() - 1;
    }
    box->setCurrentIndex(row);
}

void ProfileDelegate::setModelData(QWidget *editor, QAbstractItemModel *model,
                                   const QModelIndex &index) const {
    auto *box = qobject_cast<QComboBox *>(editor);
    model->setData(index, box->currentData().toInt(), ProfileModel::IntervalRole);
}

void ProfileDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                                           const QModelIndex &) const {
    const QRect cardRect =
        option.rect.adjusted(kCardMargin, kCardMargin, -kCardMargin, -kCardMargin);
    const int height = editor->sizeHint().height();
    const int nameCenter = cardRect.top() + kPadV + (QFontMetrics(option.font).height() + 2) / 2;
    editor->setGeometry(cardRect.right() - kPad - kEditorWidth, nameCenter - height / 2,
                        kEditorWidth, height);
}

ProfilesPage::ProfilesPage(core::ProfileStore *store, QWidget *parent)
    : QWidget(parent), store_(store), model_(new ProfileModel(this)) {
    urlEdit_ = new QLineEdit(this);
    urlEdit_->setPlaceholderText(tr("Subscription URL…"));
    urlEdit_->setClearButtonEnabled(true);
    connect(urlEdit_, &QLineEdit::returnPressed, this, &ProfilesPage::importUrl);

    auto *urlButton = new QPushButton(tr("Import from URL"), this);
    connect(urlButton, &QPushButton::clicked, this, &ProfilesPage::importUrl);

    auto *fileButton = new QPushButton(tr("Import from File"), this);
    connect(fileButton, &QPushButton::clicked, this, &ProfilesPage::importFile);

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName("errorBanner");
    errorLabel_->setWordWrap(true);
    errorLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    errorLabel_->hide();

    view_ = new QListView(this);
    view_->setModel(model_);
    view_->setItemDelegate(new ProfileDelegate(view_));
    view_->setFrameShape(QFrame::NoFrame);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QListView::customContextMenuRequested, this, &ProfilesPage::showContextMenu);
    connect(view_, &QListView::activated, this, [this](const QModelIndex &index) {
        store_->selectProfile(model_->profileAt(index).uid);
    });

    auto *controls = new QHBoxLayout;
    controls->setSpacing(theme::kPageSpacing);
    controls->addWidget(urlEdit_, 1);
    controls->addWidget(urlButton);
    controls->addWidget(fileButton);

    auto *layout = theme::pageLayout(this);
    layout->addLayout(controls);
    layout->addWidget(errorLabel_);
    layout->addWidget(view_, 1);

    connect(store_, &core::ProfileStore::profilesChanged, this, &ProfilesPage::onProfilesChanged);
    connect(store_, &core::ProfileStore::profileUpdated, this,
            [this] { onProfilesChanged(store_->profiles(), store_->currentUid()); });
    connect(store_, &core::ProfileStore::errorOccurred, this, &ProfilesPage::onErrorOccurred);
    // Queued: setUpdateInterval re-emits profilesChanged, which would delete the combo box
    // whose signal is still on the stack.
    connect(
        model_, &ProfileModel::updateIntervalEdited, this,
        [this](const QString &uid, int minutes) { store_->setUpdateInterval(uid, minutes); },
        Qt::QueuedConnection);

    // The store is loaded before this page exists, so its first signal is already gone.
    onProfilesChanged(store_->profiles(), store_->currentUid());
}

void ProfilesPage::onProfilesChanged(const QVector<core::Profile> &profiles,
                                     const QString &currentUid) {
    const QString selected = model_->profileAt(view_->currentIndex()).uid;
    model_->setProfiles(profiles, currentUid);
    errorLabel_->hide();

    for (int row = 0; row < model_->rowCount(); ++row) {
        const QModelIndex index = model_->index(row);
        const core::Profile profile = model_->profileAt(index);
        if (profile.remote) view_->openPersistentEditor(index);
        if (profile.uid == selected) view_->setCurrentIndex(index);
    }
}

void ProfilesPage::onErrorOccurred(const QString &message) {
    errorLabel_->setText(message);
    errorLabel_->show();
}

void ProfilesPage::showContextMenu(const QPoint &pos) {
    const QModelIndex index = view_->indexAt(pos);
    if (!index.isValid()) return;

    const core::Profile profile = model_->profileAt(index);

    QMenu menu(this);
    QAction *select = menu.addAction(tr("Select"));
    select->setEnabled(profile.uid != model_->currentUid());
    QAction *update = menu.addAction(tr("Update"));
    update->setEnabled(profile.remote);
    QAction *rename = menu.addAction(tr("Rename…"));
    QAction *copyUrl = menu.addAction(tr("Copy Subscription URL"));
    copyUrl->setEnabled(profile.remote);
    menu.addSeparator();
    QAction *remove = menu.addAction(tr("Remove…"));

    const QAction *chosen = menu.exec(view_->viewport()->mapToGlobal(pos));
    if (chosen == select) {
        store_->selectProfile(profile.uid);
    } else if (chosen == update) {
        store_->updateProfile(profile.uid);
    } else if (chosen == copyUrl) {
        QGuiApplication::clipboard()->setText(profile.url);
    } else if (chosen == rename) {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, tr("Rename Profile"), tr("Name:"),
                                                   QLineEdit::Normal, profile.name, &accepted);
        if (accepted && !name.trimmed().isEmpty()) {
            store_->renameProfile(profile.uid, name.trimmed());
        }
    } else if (chosen == remove) {
        const auto answer = QMessageBox::question(
            this, tr("Remove Profile"),
            tr("Remove “%1”? Its downloaded config is deleted as well.").arg(profile.name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) store_->removeProfile(profile.uid);
    }
}

void ProfilesPage::importUrl() {
    const QString url = urlEdit_->text().trimmed();
    if (url.isEmpty()) return;

    store_->importFromUrl(url);
    urlEdit_->clear();
}

void ProfilesPage::importFile() {
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Import Profile"), QString(),
                                     tr("Clash config (*.yaml *.yml);;All files (*)"));
    if (!path.isEmpty()) store_->importFromFile(path);
}

}  // namespace ui
