#include "ui/pages/profiles/profiles_page.h"

#include <QClipboard>
#include <QComboBox>
#include "ui/widgets/combo_box.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QFontDatabase>
#include <QPlainTextEdit>
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
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

#include "ui/pages/profiles/effective_config_view.h"
#include "ui/pages/profiles/preset_editor.h"
#include "ui/theme/formatting.h"
#include "ui/theme/theme.h"

namespace ui {
namespace {

constexpr int kCardMargin = 4;
constexpr int kAccentWidth = 5;
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
            return profile.remote ? redactUrl(profile.url) : profile.filePath;
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

    // The chosen profile stays distinct even when keyboard focus moves to
    // another row or its interval editor.
    painter->fillPath(card, current    ? theme::blend(base, accent, 0.22)
                            : selected ? theme::blend(base, accent, 0.10)
                                       : base);
    if (current) {
        painter->save();
        painter->setClipPath(card);
        painter->fillRect(QRect(cardRect.left(), cardRect.top(), kAccentWidth, cardRect.height()),
                          accent);
        painter->restore();
    }
    painter->setPen(QPen(current || selected ? accent : t.border, current ? 2 : 1));
    painter->drawPath(card);

    const QRect content = cardRect.adjusted(kAccentWidth + kPad, kPadV, -kPad, -kPadV);
    const QFontMetrics metrics(option.font);

    QFont nameFont = option.font;
    nameFont.setBold(true);
    const QFontMetrics nameMetrics(nameFont);

    QFont chipFont = option.font;
    chipFont.setBold(true);
    if (chipFont.pointSizeF() > 0) chipFont.setPointSizeF(chipFont.pointSizeF() - 1.0);

    // Reserve a separate right-hand column for the centered interval editor.
    const int reserved = profile.remote ? kEditorWidth + kPad : 0;
    QRect line(content.left(), content.top(), content.width() - reserved,
               nameMetrics.height() + 2);

    const QFontMetrics chipMetrics(chipFont);
    // Put the selected marker first so long names cannot push it out of view.
    if (current) {
        const int next = drawChip(painter, line.left(), line, tr("✓ SELECTED"), chipFont,
                                 t.accentText, accent);
        line.setLeft(next + 4);
    }
    const int chipSpace = chipMetrics.horizontalAdvance(profile.remote ? tr("REMOTE") : tr("LOCAL")) + 28;
    const QString name = nameMetrics.elidedText(profile.name, Qt::ElideRight,
                                               qMax(0, line.width() - chipSpace));
    painter->setFont(nameFont);
    painter->setPen(text);
    painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter, name);

    const int chipX = line.left() + nameMetrics.horizontalAdvance(name) + 10;
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

        line = QRect(content.left(), line.bottom() + 6, content.width() - reserved, metrics.height());
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
    auto *box = new ComboBox(parent);
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
    const int top = cardRect.top() + (cardRect.height() - height) / 2;
    editor->setGeometry(cardRect.right() - kPad - kEditorWidth + 1, top,
                        kEditorWidth, height);
}

ProfilesPage::ProfilesPage(core::ProfileStore *store, QWidget *parent)
    : QWidget(parent), store_(store), model_(new ProfileModel(this)) {
    urlEdit_ = new QLineEdit(this);
    urlEdit_->setObjectName("subscriptionUrl");
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
    errorLabel_->setTextFormat(Qt::PlainText);
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
    emptyLabel_ = new QLabel(tr("No profiles yet. Import a subscription URL or a local YAML file to get started."), this);
    emptyLabel_->setObjectName("noticeBanner");
    emptyLabel_->setWordWrap(true);
    layout->addWidget(emptyLabel_);
    auto *actions = new QHBoxLayout;
    auto *select = new QPushButton(tr("Select Profile"), this);
    auto *edit = new QPushButton(tr("Edit YAML…"), this);
    auto *updateAll = new QPushButton(tr("Update All"), this);
    auto *create = new QPushButton(tr("New Local…"), this);
    select->setEnabled(false);
    edit->setEnabled(false);
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this, select, edit](const QModelIndex &current) {
                select->setEnabled(current.isValid());
                edit->setEnabled(current.isValid());
                updatePresetScope();
            });
    connect(select, &QPushButton::clicked, this, [this] {
        store_->selectProfile(model_->profileAt(view_->currentIndex()).uid);
    });
    connect(edit, &QPushButton::clicked, this, [this] {
        editProfile(model_->profileAt(view_->currentIndex()));
    });
    connect(updateAll, &QPushButton::clicked, this, [this] {
        for (const auto &profile : store_->profiles())
            if (profile.remote) store_->updateProfile(profile.uid);
    });
    connect(create, &QPushButton::clicked, this, [this] {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, tr("New Local Profile"), tr("Name:"),
            QLineEdit::Normal, tr("Local profile"), &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        store_->createLocalProfileAsync(name, "proxies: []\nproxy-groups: []\nrules:\n  - MATCH,DIRECT\n");
    });
    actions->addWidget(create);
    actions->addWidget(select);
    actions->addWidget(edit);
    actions->addStretch();
    actions->addWidget(updateAll);
    layout->addLayout(actions);

    // The presets and the preview sit below the profile list rather than beside
    // it: both are about the profile the list has selected, and the legacy
    // controls above keep the positions they had.
    presetEditor_ = new PresetEditor(store_, this);
    preview_ = new EffectiveConfigView(store_, this);

    auto *details = new QTabWidget(this);
    details->setObjectName("profileDetails");
    details->addTab(presetEditor_, tr("Presets"));
    details->addTab(preview_, tr("Effective Config"));

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName("profileSplitter");
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(view_);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    // One preview request per accepted edit and per scope change, and no
    // request at all until the page is shown: composing is work, and a preview
    // nobody is looking at is work for nobody.
    connect(presetEditor_, &PresetEditor::presetsCommitted, preview_,
            &EffectiveConfigView::requestPreview);
    connect(presetEditor_, &PresetEditor::scopeChanged, preview_,
            &EffectiveConfigView::requestPreview);

    connect(store_, &core::ProfileStore::profileCreated, this, [this](const QString &uid) {
        for (const auto &profile : store_->profiles()) if (profile.uid == uid) { editProfile(profile); break; }
    }, Qt::QueuedConnection);
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
    emptyLabel_->setVisible(profiles.isEmpty());
    errorLabel_->hide();

    for (int row = 0; row < model_->rowCount(); ++row) {
        const QModelIndex index = model_->index(row);
        const core::Profile profile = model_->profileAt(index);
        if (profile.remote) view_->openPersistentEditor(index);
        if (profile.uid == (selected.isEmpty() ? currentUid : selected)) view_->setCurrentIndex(index);
    }

    // Also when the selection did not move -- the list may have been emptied,
    // and then the per-profile scope has nothing left to edit.
    updatePresetScope();
}

void ProfilesPage::updatePresetScope() {
    const core::Profile profile = model_->profileAt(view_->currentIndex());
    presetEditor_->setProfile(profile.uid, profile.name);
}

void ProfilesPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (previewRequested_) return;
    previewRequested_ = true;
    preview_->requestPreview(presetEditor_->scopeUid());
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
    QAction *edit = menu.addAction(tr("Edit YAML…"));
    QAction *editUrl = menu.addAction(tr("Edit Subscription URL…"));
    editUrl->setEnabled(profile.remote);
    QAction *copyUrl = menu.addAction(tr("Copy Subscription URL"));
    copyUrl->setEnabled(profile.remote);
    menu.addSeparator();
    QAction *remove = menu.addAction(tr("Remove…"));

    const QAction *chosen = menu.exec(view_->viewport()->mapToGlobal(pos));
    if (chosen == select) {
        store_->selectProfile(profile.uid);
    } else if (chosen == update) {
        store_->updateProfile(profile.uid);
    } else if (chosen == editUrl) {
        bool accepted = false;
        const QString url = QInputDialog::getText(
            this, tr("Edit Subscription URL"),
            tr("Subscription URL:\nThe cached configuration is kept until the next update."),
            QLineEdit::Normal, profile.url, &accepted);
        if (accepted) store_->setSubscriptionUrl(profile.uid, url);
    } else if (chosen == copyUrl) {
        QGuiApplication::clipboard()->setText(profile.url);
    } else if (chosen == edit) {
        editProfile(profile);
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
    if (!path.isEmpty()) store_->importFromFileAsync(path);
}

void ProfilesPage::editProfile(const core::Profile &profile) {
    if (profile.uid.isEmpty()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Edit %1").arg(profile.name));
    dialog.resize(760, 560);
    auto *layout = new QVBoxLayout(&dialog);
    if (profile.remote) {
        auto *hint = new QLabel(tr("Subscription updates replace this file. Use an enhancement step for changes that should survive updates."), &dialog);
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }
    auto *editor = new QPlainTextEdit(&dialog);
    QFont editorFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    editorFont.setPointSizeF(qMax(editorFont.pointSizeF(), font().pointSizeF()));
    editor->setFont(editorFont);
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(editor);
    auto *error = new QLabel(&dialog);
    error->setObjectName("fieldError");
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(store_, &core::ProfileStore::errorOccurred, &dialog, [error](const QString &message) { error->setText(message); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(store_, &core::ProfileStore::reloaded, &dialog, &QDialog::reject);
    bool saving = false;
    connect(store_, &core::ProfileStore::profileContentSaved, &dialog, [&](const QString &uid, bool success) {
        if (!saving || uid != profile.uid) return;
        saving = false;
        if (success) { dialog.accept(); return; }
        editor->setEnabled(true);
        buttons->button(QDialogButtonBox::Save)->setEnabled(true);
        buttons->button(QDialogButtonBox::Cancel)->setEnabled(true);
        if (error->text() == tr("Saving…")) error->setText(tr("Save was canceled or the profile is no longer available."));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this] {
        saving = true;
        editor->setEnabled(false);
        buttons->button(QDialogButtonBox::Save)->setEnabled(false);
        buttons->button(QDialogButtonBox::Cancel)->setEnabled(false);
        error->setText(tr("Saving…"));
        store_->saveProfileContentAsync(profile.uid, editor->toPlainText());
    });
    editor->setEnabled(false);
    buttons->button(QDialogButtonBox::Save)->setEnabled(false);
    error->setText(tr("Loading…"));
    using ReadResult = QPair<QString, QString>;
    auto *reader = new QFutureWatcher<ReadResult>(&dialog);
    connect(reader, &QFutureWatcher<ReadResult>::finished, &dialog, [reader, editor, buttons, error] {
        const auto result = reader->result();
        reader->deleteLater();
        error->setText(result.second);
        if (!result.second.isEmpty()) return;
        editor->setPlainText(result.first);
        editor->setEnabled(true);
        buttons->button(QDialogButtonBox::Save)->setEnabled(true);
    });
    reader->setFuture(QtConcurrent::run([path = profile.filePath]() -> ReadResult {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {{}, file.errorString()};
        const auto contents = file.read(2 * 1024 * 1024 + 1);
        if (contents.size() > 2 * 1024 * 1024 || contents.count('\n') > 20000)
            return {{}, ProfilesPage::tr("This file is too large for the inline editor (2 MiB / 20,000 lines).")};
        if (file.error() != QFileDevice::NoError) return {{}, file.errorString()};
        return {QString::fromUtf8(contents), {}};
    }));
    dialog.exec();
}

}  // namespace ui
