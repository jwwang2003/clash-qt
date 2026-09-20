#include "ui/chain_editor.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "core/enhance/config_enhancer.h"
#include "ui/theme.h"

namespace ui {
namespace {

constexpr int kUidRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;

enum Column { Step, Kind, File, ColumnCount };

constexpr int kMinRows = 3;
constexpr int kMaxRows = 8;
constexpr int kListFrame = 4;

}  // namespace

ChainEditor::ChainEditor(core::ConfigEnhancer *enhancer, QWidget *parent)
    : QWidget(parent), enhancer_(enhancer) {
    list_ = new QTreeWidget(this);
    list_->setColumnCount(ColumnCount);
    list_->setHeaderLabels({tr("Step"), tr("Kind"), tr("File")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setAllColumnsShowFocus(true);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->header()->setStretchLastSection(false);
    list_->header()->setHighlightSections(false);
    list_->header()->setSectionResizeMode(Step, QHeaderView::Stretch);
    list_->setColumnWidth(Kind, 90);
    list_->setColumnWidth(File, 180);

    connect(list_, &QTreeWidget::currentItemChanged, this, [this] { updateActions(); });
    connect(list_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (column != Step) return;
        enhancer_->setEnabled(item->data(Step, kUidRole).toString(),
                              item->checkState(Step) == Qt::Checked);
    });
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem *item, int) {
        const QString path = item->data(Step, kPathRole).toString();
        if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    emptyLabel_ = new QLabel(
        tr("No steps yet. A merge patches the generated config; a script rewrites it."), this);
    emptyLabel_->setObjectName("sectionHint");
    emptyLabel_->setWordWrap(true);

    auto *addMerge = new QPushButton(tr("Add Merge"), this);
    connect(addMerge, &QPushButton::clicked, this, [this] { addStep(false); });

    auto *addScript = new QPushButton(tr("Add Script"), this);
    connect(addScript, &QPushButton::clicked, this, [this] { addStep(true); });

    auto *importButton = new QPushButton(tr("Import…"), this);
    connect(importButton, &QPushButton::clicked, this, &ChainEditor::importStep);

    renameButton_ = new QPushButton(tr("Rename…"), this);
    connect(renameButton_, &QPushButton::clicked, this, &ChainEditor::renameStep);

    removeButton_ = new QPushButton(tr("Remove"), this);
    connect(removeButton_, &QPushButton::clicked, this, &ChainEditor::removeStep);

    upButton_ = new QPushButton(tr("Move Up"), this);
    connect(upButton_, &QPushButton::clicked, this, [this] { move(-1); });

    downButton_ = new QPushButton(tr("Move Down"), this);
    connect(downButton_, &QPushButton::clicked, this, [this] { move(1); });

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(theme::kPageSpacing);
    buttons->addWidget(addMerge);
    buttons->addWidget(addScript);
    buttons->addWidget(importButton);
    buttons->addStretch(1);
    buttons->addWidget(renameButton_);
    buttons->addWidget(removeButton_);
    buttons->addWidget(upButton_);
    buttons->addWidget(downButton_);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kPageSpacing);
    layout->addWidget(emptyLabel_);
    layout->addWidget(list_);
    layout->addLayout(buttons);

    connect(enhancer_, &core::ConfigEnhancer::chainChanged, this, [this] { reload(); });
    reload();
}

QString ChainEditor::selectedUid() const {
    const QTreeWidgetItem *item = list_->currentItem();
    return item ? item->data(Step, kUidRole).toString() : QString();
}

void ChainEditor::reload() {
    const QString selected = selectedUid();
    const QVector<core::ChainItem> chain = enhancer_->chain();

    {
        QSignalBlocker blocker(list_);
        list_->clear();
        for (const core::ChainItem &step : chain) {
            auto *item = new QTreeWidgetItem(list_);
            item->setText(Step, step.name);
            item->setText(Kind, step.kind == core::ChainKind::Script ? tr("Script") : tr("Merge"));
            item->setText(File, QFileInfo(step.filePath).fileName());
            item->setToolTip(File, step.filePath);
            item->setData(Step, kUidRole, step.uid);
            item->setData(Step, kPathRole, step.filePath);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Step, step.enabled ? Qt::Checked : Qt::Unchecked);
            if (step.uid == selected) list_->setCurrentItem(item);
        }
    }

    list_->setVisible(!chain.isEmpty());
    emptyLabel_->setVisible(chain.isEmpty());
    if (!chain.isEmpty()) {
        // Hug the rows: a three-step chain should not sit in a half-empty box.
        list_->setFixedHeight(list_->header()->sizeHint().height() +
                              qBound(kMinRows, chain.size(), kMaxRows) *
                                  list_->sizeHintForRow(0) +
                              kListFrame);
    }
    updateActions();
}

void ChainEditor::updateActions() {
    const int row = list_->indexOfTopLevelItem(list_->currentItem());
    renameButton_->setEnabled(row >= 0);
    removeButton_->setEnabled(row >= 0);
    upButton_->setEnabled(row > 0);
    downButton_->setEnabled(row >= 0 && row < list_->topLevelItemCount() - 1);
}

void ChainEditor::addStep(bool script) {
    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, script ? tr("Add Script") : tr("Add Merge"), tr("Name:"),
                              QLineEdit::Normal, QString(), &accepted);
    if (!accepted || name.trimmed().isEmpty()) return;

    if (script) {
        enhancer_->addScript(name.trimmed());
    } else {
        enhancer_->addMerge(name.trimmed());
    }
    reload();
}

void ChainEditor::importStep() {
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Import Enhancement"), QString(),
                                     tr("Merge or script (*.yaml *.yml *.js);;All files (*)"));
    if (path.isEmpty()) return;

    // The suffix is the only thing that tells a merge fragment from a script.
    const core::ChainKind kind = QFileInfo(path).suffix().compare("js", Qt::CaseInsensitive) == 0
                                     ? core::ChainKind::Script
                                     : core::ChainKind::Merge;
    enhancer_->importItem(path, kind);
    reload();
}

void ChainEditor::renameStep() {
    QTreeWidgetItem *item = list_->currentItem();
    if (!item) return;

    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Rename Step"), tr("Name:"),
                                               QLineEdit::Normal, item->text(Step), &accepted);
    if (!accepted || name.trimmed().isEmpty()) return;

    enhancer_->renameItem(item->data(Step, kUidRole).toString(), name.trimmed());
    reload();
}

void ChainEditor::removeStep() {
    QTreeWidgetItem *item = list_->currentItem();
    if (!item) return;

    const auto answer = QMessageBox::question(
        this, tr("Remove Step"),
        tr("Remove “%1”? Its file is deleted as well.").arg(item->text(Step)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    enhancer_->removeItem(item->data(Step, kUidRole).toString());
    reload();
}

void ChainEditor::move(int delta) {
    QTreeWidgetItem *item = list_->currentItem();
    if (!item) return;

    const int row = list_->indexOfTopLevelItem(item);
    enhancer_->moveItem(item->data(Step, kUidRole).toString(), row + delta);
    reload();
}

}  // namespace ui
