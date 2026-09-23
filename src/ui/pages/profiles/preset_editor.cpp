#include "ui/pages/profiles/preset_editor.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include "core/config/config_composer.h"
#include "core/profiles/profile_store.h"
#include "ui/theme/theme.h"
#include "ui/widgets/combo_box.h"

namespace ui {
namespace {

constexpr int kIdRole = Qt::UserRole + 1;

constexpr int kPresetRows = 4;
constexpr int kOperationRows = 4;
constexpr int kListFrame = 4;
constexpr int kValueHeight = 84;

enum PresetColumn { PresetName, PresetOperations, PresetColumnCount };
enum OperationColumn { OperationKind, OperationPath, OperationValue, OperationColumnCount };

/// The five kinds the contract defines, in the order it lists them.
const QVector<QString> &operationKinds() {
    static const QVector<QString> kinds{
        QStringLiteral("merge"),  QStringLiteral("replace"), QStringLiteral("remove"),
        QStringLiteral("prepend"), QStringLiteral("append")};
    return kinds;
}

QJsonArray chainOf(const QJsonObject &document, const QString &uid) {
    if (uid.isEmpty()) return document.value("global").toArray();
    return document.value("profiles").toObject().value(uid).toArray();
}

QJsonObject withChain(const QJsonObject &document, const QString &uid, const QJsonArray &chain) {
    QJsonObject next = document;
    next.insert("version", 1);
    if (uid.isEmpty()) {
        next.insert("global", chain);
        return next;
    }
    QJsonObject profiles = next.value("profiles").toObject();
    // An emptied chain drops its uid rather than leaving a dead key behind.
    if (chain.isEmpty()) {
        profiles.remove(uid);
    } else {
        profiles.insert(uid, chain);
    }
    next.insert("profiles", profiles);
    return next;
}

QString compactJson(const QJsonValue &value) {
    // Wrapped in an array because QJsonDocument only serialises an object or an
    // array; the operation value is frequently a scalar.
    const QByteArray wrapped =
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).trimmed();
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}

QString prettyJson(const QJsonValue &value) {
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented))
            .trimmed();
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented))
            .trimmed();
    return compactJson(value);
}

}  // namespace

PresetEditor::PresetEditor(core::ProfileStore *store, QWidget *parent)
    : QWidget(parent), store_(store) {
    scopeBox_ = new ComboBox(this);
    scopeBox_->setObjectName("presetScope");
    scopeBox_->setToolTip(tr("Global presets apply to every profile, before the selected "
                             "profile's own presets."));
    scopeBox_->addItem(tr("Global presets"), QString());
    scopeBox_->addItem(tr("This profile"), QString());

    presetList_ = new QTreeWidget(this);
    presetList_->setObjectName("presetList");
    presetList_->setColumnCount(PresetColumnCount);
    presetList_->setHeaderLabels({tr("Preset"), tr("Operations")});
    presetList_->setRootIsDecorated(false);
    presetList_->setUniformRowHeights(true);
    presetList_->setAllColumnsShowFocus(true);
    presetList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    presetList_->setSelectionMode(QAbstractItemView::SingleSelection);
    presetList_->header()->setStretchLastSection(false);
    presetList_->header()->setHighlightSections(false);
    presetList_->header()->setSectionResizeMode(PresetName, QHeaderView::Stretch);
    presetList_->setColumnWidth(PresetOperations, 90);
    presetList_->setMinimumHeight(kPresetRows * presetList_->fontMetrics().lineSpacing() * 2 +
                                  kListFrame);

    operationList_ = new QTreeWidget(this);
    operationList_->setObjectName("operationList");
    operationList_->setColumnCount(OperationColumnCount);
    operationList_->setHeaderLabels({tr("Kind"), tr("Path"), tr("Value")});
    operationList_->setRootIsDecorated(false);
    operationList_->setUniformRowHeights(true);
    operationList_->setAllColumnsShowFocus(true);
    operationList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    operationList_->setSelectionMode(QAbstractItemView::SingleSelection);
    operationList_->header()->setStretchLastSection(true);
    operationList_->header()->setHighlightSections(false);
    operationList_->setColumnWidth(OperationKind, 90);
    operationList_->setColumnWidth(OperationPath, 200);
    operationList_->setMinimumHeight(
        kOperationRows * operationList_->fontMetrics().lineSpacing() * 2 + kListFrame);

    emptyLabel_ = new QLabel(
        tr("No presets in this scope yet. A preset is an ordered list of operations applied to "
           "the generated configuration; presets apply in the order shown."),
        this);
    emptyLabel_->setObjectName("sectionHint");
    emptyLabel_->setWordWrap(true);

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName("fieldError");
    errorLabel_->setTextFormat(Qt::PlainText);
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();

    // Named inline rather than through a modal prompt: the name belongs to the
    // same form as the operation being edited, and the whole editor stays
    // drivable without a dialog.
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setObjectName("presetName");
    nameEdit_->setPlaceholderText(tr("Preset name"));

    addPresetButton_ = new QPushButton(tr("Add Preset"), this);
    addPresetButton_->setObjectName("addPreset");
    renamePresetButton_ = new QPushButton(tr("Rename…"), this);
    renamePresetButton_->setObjectName("renamePreset");
    removePresetButton_ = new QPushButton(tr("Remove"), this);
    removePresetButton_->setObjectName("removePreset");
    presetUpButton_ = new QPushButton(tr("Move Up"), this);
    presetUpButton_->setObjectName("presetUp");
    presetDownButton_ = new QPushButton(tr("Move Down"), this);
    presetDownButton_->setObjectName("presetDown");

    kindBox_ = new ComboBox(this);
    kindBox_->setObjectName("operationKind");
    for (const QString &kind : operationKinds()) kindBox_->addItem(kind, kind);

    pathEdit_ = new QLineEdit(this);
    pathEdit_->setObjectName("operationPath");
    pathEdit_->setPlaceholderText(tr("/dns/enable"));
    pathEdit_->setToolTip(tr("RFC 6901 pointer into the configuration map, with ~0 for “~” and "
                             "~1 for “/”. prepend and append apply to /rules."));

    valueLabel_ = new QLabel(tr("Value (JSON)"), this);
    valueEdit_ = new QPlainTextEdit(this);
    valueEdit_->setObjectName("operationValue");
    QFont valueFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    valueFont.setPointSizeF(qMax(valueFont.pointSizeF(), font().pointSizeF()));
    valueEdit_->setFont(valueFont);
    valueEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    valueEdit_->setMaximumHeight(kValueHeight);
    valueEdit_->setPlaceholderText(tr("true · 7890 · \"strict\" · [\"MATCH,DIRECT\"] · {\"enable\": true}"));

    addOperationButton_ = new QPushButton(tr("Add Operation"), this);
    addOperationButton_->setObjectName("addOperation");
    applyOperationButton_ = new QPushButton(tr("Apply to Selected"), this);
    applyOperationButton_->setObjectName("applyOperation");
    removeOperationButton_ = new QPushButton(tr("Remove Operation"), this);
    removeOperationButton_->setObjectName("removeOperation");
    operationUpButton_ = new QPushButton(tr("Move Up"), this);
    operationUpButton_->setObjectName("operationUp");
    operationDownButton_ = new QPushButton(tr("Move Down"), this);
    operationDownButton_->setObjectName("operationDown");

    auto *scopeRow = new QHBoxLayout;
    scopeRow->setSpacing(theme::kPageSpacing);
    scopeRow->addWidget(new QLabel(tr("Scope"), this));
    scopeRow->addWidget(scopeBox_, 1);

    auto *presetButtons = new QHBoxLayout;
    presetButtons->setSpacing(theme::kPageSpacing);
    presetButtons->addWidget(nameEdit_, 1);
    presetButtons->addWidget(addPresetButton_);
    presetButtons->addWidget(renamePresetButton_);
    presetButtons->addWidget(removePresetButton_);
    presetButtons->addStretch(1);
    presetButtons->addWidget(presetUpButton_);
    presetButtons->addWidget(presetDownButton_);

    auto *formRow = new QHBoxLayout;
    formRow->setSpacing(theme::kPageSpacing);
    formRow->addWidget(kindBox_);
    formRow->addWidget(pathEdit_, 1);

    auto *operationButtons = new QHBoxLayout;
    operationButtons->setSpacing(theme::kPageSpacing);
    operationButtons->addWidget(addOperationButton_);
    operationButtons->addWidget(applyOperationButton_);
    operationButtons->addWidget(removeOperationButton_);
    operationButtons->addStretch(1);
    operationButtons->addWidget(operationUpButton_);
    operationButtons->addWidget(operationDownButton_);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kPageSpacing);
    layout->addLayout(scopeRow);
    layout->addWidget(emptyLabel_);
    layout->addWidget(presetList_, 1);
    layout->addLayout(presetButtons);
    layout->addWidget(operationList_, 1);
    layout->addLayout(formRow);
    layout->addWidget(valueLabel_);
    layout->addWidget(valueEdit_);
    layout->addLayout(operationButtons);
    layout->addWidget(errorLabel_);

    // currentIndexChanged, not activated: the scope also moves when a profile
    // stops being available, and the lists must follow either way. `loading_`
    // covers the rebuilds that are not a scope change.
    connect(scopeBox_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        reload();
        emit scopeChanged(scopeUid());
    });
    connect(presetList_, &QTreeWidget::currentItemChanged, this, [this] {
        if (loading_) return;
        loadPresetForm();
        reloadOperations();
        loadOperationForm();
        updateActions();
    });
    connect(presetList_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *item, int column) {
                if (loading_ || column != PresetName) return;
                const QString id = item->data(PresetName, kIdRole).toString();
                const bool enabled = item->checkState(PresetName) == Qt::Checked;
                // Deferred: the commit rebuilds this tree, and the item whose
                // signal is on the stack would be deleted underneath it.
                QTimer::singleShot(0, this, [this, id, enabled] { setPresetEnabled(id, enabled); });
            });
    connect(operationList_, &QTreeWidget::currentItemChanged, this, [this] {
        if (loading_) return;
        loadOperationForm();
        updateActions();
    });
    connect(kindBox_, &QComboBox::currentIndexChanged, this, [this] { updateActions(); });

    connect(addPresetButton_, &QPushButton::clicked, this, &PresetEditor::addPreset);
    connect(renamePresetButton_, &QPushButton::clicked, this, &PresetEditor::renamePreset);
    connect(removePresetButton_, &QPushButton::clicked, this, &PresetEditor::removePreset);
    connect(presetUpButton_, &QPushButton::clicked, this, [this] { movePreset(-1); });
    connect(presetDownButton_, &QPushButton::clicked, this, [this] { movePreset(1); });
    connect(addOperationButton_, &QPushButton::clicked, this, &PresetEditor::addOperation);
    connect(applyOperationButton_, &QPushButton::clicked, this, &PresetEditor::applyOperation);
    connect(removeOperationButton_, &QPushButton::clicked, this, &PresetEditor::removeOperation);
    connect(operationUpButton_, &QPushButton::clicked, this, [this] { moveOperation(-1); });
    connect(operationDownButton_, &QPushButton::clicked, this, [this] { moveOperation(1); });

    // Someone else's write to the document -- a recovery, another surface --
    // reaches the lists the same way this editor's own writes do.
    connect(store_, &core::ProfileStore::presetsChanged, this, [this] {
        document_ = store_->presetDocument();
        reload();
    });

    document_ = store_->presetDocument();
    reload();
}

void PresetEditor::setProfile(const QString &uid, const QString &name) {
    if (uid == profileUid_ && name == profileName_) return;

    const bool showingProfile = scopeBox_->currentIndex() == 1;
    profileUid_ = uid;
    profileName_ = name;

    loading_ = true;
    scopeBox_->setItemText(1, name.isEmpty() ? tr("This profile") : tr("Profile: %1").arg(name));
    scopeBox_->setItemData(1, uid);
    auto *model = qobject_cast<QStandardItemModel *>(scopeBox_->model());
    if (model && model->item(1)) model->item(1)->setEnabled(!uid.isEmpty());
    if (showingProfile && uid.isEmpty()) scopeBox_->setCurrentIndex(0);
    loading_ = false;

    if (!showingProfile) {
        updateActions();
        return;
    }
    // The per-profile scope now points at a different chain, so the lists and
    // anything showing a preview of them are out of date.
    reload();
    emit scopeChanged(scopeUid());
}

QString PresetEditor::scopeUid() const {
    return scopeBox_->currentIndex() == 1 ? profileUid_ : QString();
}

void PresetEditor::reload() {
    const QString selected = selectedPresetId();

    loading_ = true;
    presetList_->clear();
    const QJsonArray chain = chainOf(document_, scopeUid());
    for (const QJsonValue &entry : chain) {
        const QJsonObject preset = entry.toObject();
        auto *item = new QTreeWidgetItem(presetList_);
        item->setText(PresetName, preset.value("name").toString());
        item->setData(PresetName, kIdRole, preset.value("id").toString());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(PresetName, preset.value("enabled").toBool(true) ? Qt::Checked
                                                                            : Qt::Unchecked);
        item->setText(PresetOperations, QString::number(preset.value("operations").toArray().size()));
        item->setToolTip(PresetName, preset.value("id").toString());
    }
    emptyLabel_->setVisible(chain.isEmpty());
    loading_ = false;

    if (!selected.isEmpty()) selectPreset(selected);
    if (!presetList_->currentItem() && presetList_->topLevelItemCount() > 0)
        presetList_->setCurrentItem(presetList_->topLevelItem(0));

    reloadOperations();
    updateActions();
}

void PresetEditor::reloadOperations() {
    const int row = selectedOperationRow();

    loading_ = true;
    operationList_->clear();
    const int index = presetIndex(selectedPresetId());
    if (index >= 0) {
        const QJsonArray operations =
            chainOf(document_, scopeUid()).at(index).toObject().value("operations").toArray();
        for (const QJsonValue &entry : operations) {
            const QJsonObject operation = entry.toObject();
            auto *item = new QTreeWidgetItem(operationList_);
            item->setText(OperationKind, operation.value("op").toString());
            item->setText(OperationPath, operation.value("path").toString());
            item->setText(OperationValue, compactJson(operation.value("value")));
        }
    }
    loading_ = false;

    if (row >= 0 && row < operationList_->topLevelItemCount()) selectOperation(row);
}

void PresetEditor::loadPresetForm() {
    const int index = presetIndex(selectedPresetId());
    if (index < 0) return;
    nameEdit_->setText(chainOf(document_, scopeUid()).at(index).toObject().value("name").toString());
}

void PresetEditor::loadOperationForm() {
    const int presetRow = presetIndex(selectedPresetId());
    const int row = selectedOperationRow();
    if (presetRow < 0 || row < 0) return;

    const QJsonObject operation = chainOf(document_, scopeUid())
                                      .at(presetRow)
                                      .toObject()
                                      .value("operations")
                                      .toArray()
                                      .at(row)
                                      .toObject();
    const int kind = kindBox_->findData(operation.value("op").toString());
    if (kind >= 0) kindBox_->setCurrentIndex(kind);
    pathEdit_->setText(operation.value("path").toString());
    valueEdit_->setPlainText(prettyJson(operation.value("value")));
}

void PresetEditor::updateActions() {
    const int presetRow = presetIndex(selectedPresetId());
    const bool hasPreset = presetRow >= 0;
    const int presets = presetList_->topLevelItemCount();
    const int row = selectedOperationRow();
    const bool removes = kindBox_->currentData().toString() == QStringLiteral("remove");

    renamePresetButton_->setEnabled(hasPreset);
    removePresetButton_->setEnabled(hasPreset);
    presetUpButton_->setEnabled(hasPreset && presetRow > 0);
    presetDownButton_->setEnabled(hasPreset && presetRow < presets - 1);

    addOperationButton_->setEnabled(hasPreset);
    applyOperationButton_->setEnabled(hasPreset && row >= 0);
    removeOperationButton_->setEnabled(hasPreset && row >= 0);
    operationUpButton_->setEnabled(hasPreset && row > 0);
    operationDownButton_->setEnabled(hasPreset && row >= 0 &&
                                     row < operationList_->topLevelItemCount() - 1);

    // remove names a key; it carries no value, so the editor says so instead of
    // asking for JSON that would be ignored.
    valueEdit_->setEnabled(!removes);
    valueLabel_->setText(removes ? tr("Value (ignored by remove)") : tr("Value (JSON)"));
}

void PresetEditor::showError(const QString &message) {
    errorLabel_->setText(message);
    errorLabel_->setVisible(!message.isEmpty());
}

bool PresetEditor::commit(const QJsonObject &document) {
    QJsonObject next = document;
    next.insert("version", 1);
    if (!store_->setPresetDocument(next)) {
        // The store already summarised the rejection through errorOccurred(),
        // which the page shows. This says the other half -- which paths were
        // objected to, and that the lists below are again what is on disk --
        // so the two messages complement each other instead of repeating.
        QStringList reasons;
        for (const core::config::Diagnostic &diagnostic : store_->lastPresetDiagnostics()) {
            reasons << (diagnostic.path.isEmpty()
                            ? diagnostic.message
                            : tr("%1: %2").arg(diagnostic.path, diagnostic.message));
        }
        showError(tr("Nothing was saved; the presets below are the ones still on disk.%1")
                      .arg(reasons.isEmpty() ? QString()
                                             : QLatin1String("\n") + reasons.join(QLatin1Char('\n'))));
        document_ = store_->presetDocument();
        reload();
        return false;
    }

    showError({});
    document_ = store_->presetDocument();
    reload();
    emit presetsCommitted(scopeUid());
    return true;
}

void PresetEditor::addPreset() {
    const QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) {
        showError(tr("Type a name for the new preset."));
        return;
    }

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject preset;
    preset.insert("id", id);
    preset.insert("name", name);
    preset.insert("enabled", true);
    preset.insert("operations", QJsonArray{});

    QJsonArray chain = chainOf(document_, scopeUid());
    chain.append(preset);
    if (commit(withChain(document_, scopeUid(), chain))) selectPreset(id);
}

void PresetEditor::renamePreset() {
    const QString id = selectedPresetId();
    const int index = presetIndex(id);
    if (index < 0) return;

    const QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) {
        showError(tr("Type the new name for this preset."));
        return;
    }

    QJsonArray chain = chainOf(document_, scopeUid());
    QJsonObject preset = chain.at(index).toObject();
    preset.insert("name", name);
    chain.replace(index, preset);
    if (commit(withChain(document_, scopeUid(), chain))) selectPreset(id);
}

void PresetEditor::removePreset() {
    const int index = presetIndex(selectedPresetId());
    if (index < 0) return;

    QJsonArray chain = chainOf(document_, scopeUid());
    const QString name = chain.at(index).toObject().value("name").toString();
    const auto answer =
        QMessageBox::question(this, tr("Remove Preset"),
                              tr("Remove “%1”? Its operations are removed with it.").arg(name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    chain.removeAt(index);
    commit(withChain(document_, scopeUid(), chain));
}

void PresetEditor::movePreset(int delta) {
    const QString id = selectedPresetId();
    const int index = presetIndex(id);
    QJsonArray chain = chainOf(document_, scopeUid());
    const int target = index + delta;
    if (index < 0 || target < 0 || target >= chain.size()) return;

    const QJsonValue moved = chain.at(index);
    chain.removeAt(index);
    chain.insert(target, moved);
    if (commit(withChain(document_, scopeUid(), chain))) selectPreset(id);
}

void PresetEditor::setPresetEnabled(const QString &presetId, bool enabled) {
    const int index = presetIndex(presetId);
    if (index < 0) return;

    QJsonArray chain = chainOf(document_, scopeUid());
    QJsonObject preset = chain.at(index).toObject();
    if (preset.value("enabled").toBool(true) == enabled) return;

    preset.insert("enabled", enabled);
    chain.replace(index, preset);
    if (commit(withChain(document_, scopeUid(), chain))) selectPreset(presetId);
}

bool PresetEditor::draftOperation(QJsonObject *operation, QString *error) const {
    const QString kind = kindBox_->currentData().toString();
    const QString path = pathEdit_->text().trimmed();
    if (path.isEmpty() || !path.startsWith(QLatin1Char('/'))) {
        *error = tr("The path must be a JSON pointer starting with “/”, for example /dns/enable.");
        return false;
    }

    QJsonValue value = QJsonValue::Null;
    if (kind != QStringLiteral("remove")) {
        const QString text = valueEdit_->toPlainText().trimmed();
        if (text.isEmpty()) {
            *error = tr("This operation needs a JSON value.");
            return false;
        }
        QJsonParseError parse{};
        // Wrapped in an array so a scalar -- true, 7890, "strict" -- parses as
        // JSON too: QJsonDocument accepts only an object or an array on its own.
        const QJsonDocument parsed =
            QJsonDocument::fromJson("[" + text.toUtf8() + "]", &parse);
        if (parse.error != QJsonParseError::NoError || !parsed.isArray() ||
            parsed.array().size() != 1) {
            *error = tr("The value is not valid JSON: %1").arg(parse.errorString());
            return false;
        }
        value = parsed.array().at(0);
        if ((kind == QStringLiteral("prepend") || kind == QStringLiteral("append")) &&
            !value.isArray()) {
            *error = tr("prepend and append take an array of rule strings, for example "
                        "[\"MATCH,DIRECT\"].");
            return false;
        }
    }

    operation->insert("op", kind);
    operation->insert("path", path);
    operation->insert("value", value);
    return true;
}

void PresetEditor::addOperation() {
    const int index = presetIndex(selectedPresetId());
    if (index < 0) return;

    QJsonObject operation;
    QString error;
    if (!draftOperation(&operation, &error)) {
        showError(error);
        return;
    }

    QJsonArray chain = chainOf(document_, scopeUid());
    QJsonObject preset = chain.at(index).toObject();
    QJsonArray operations = preset.value("operations").toArray();
    operations.append(operation);
    preset.insert("operations", operations);
    chain.replace(index, preset);
    if (commit(withChain(document_, scopeUid(), chain))) selectOperation(operations.size() - 1);
}

void PresetEditor::applyOperation() {
    const int index = presetIndex(selectedPresetId());
    const int row = selectedOperationRow();
    if (index < 0 || row < 0) return;

    QJsonObject operation;
    QString error;
    if (!draftOperation(&operation, &error)) {
        showError(error);
        return;
    }

    QJsonArray chain = chainOf(document_, scopeUid());
    QJsonObject preset = chain.at(index).toObject();
    QJsonArray operations = preset.value("operations").toArray();
    if (row >= operations.size()) return;
    operations.replace(row, operation);
    preset.insert("operations", operations);
    chain.replace(index, preset);
    if (commit(withChain(document_, scopeUid(), chain))) selectOperation(row);
}

void PresetEditor::removeOperation() {
    const int index = presetIndex(selectedPresetId());
    const int row = selectedOperationRow();
    if (index < 0 || row < 0) return;

    QJsonArray chain = chainOf(document_, scopeUid());
    QJsonObject preset = chain.at(index).toObject();
    QJsonArray operations = preset.value("operations").toArray();
    if (row >= operations.size()) return;
    operations.removeAt(row);
    preset.insert("operations", operations);
    chain.replace(index, preset);
    commit(withChain(document_, scopeUid(), chain));
}

void PresetEditor::moveOperation(int delta) {
    const int index = presetIndex(selectedPresetId());
    const int row = selectedOperationRow();
    if (index < 0 || row < 0) return;

    QJsonArray chain = chainOf(document_, scopeUid());
    QJsonObject preset = chain.at(index).toObject();
    QJsonArray operations = preset.value("operations").toArray();
    const int target = row + delta;
    if (target < 0 || target >= operations.size()) return;

    const QJsonValue moved = operations.at(row);
    operations.removeAt(row);
    operations.insert(target, moved);
    preset.insert("operations", operations);
    chain.replace(index, preset);
    if (commit(withChain(document_, scopeUid(), chain))) selectOperation(target);
}

int PresetEditor::presetIndex(const QString &presetId) const {
    if (presetId.isEmpty()) return -1;
    const QJsonArray chain = chainOf(document_, scopeUid());
    for (int index = 0; index < chain.size(); ++index)
        if (chain.at(index).toObject().value("id").toString() == presetId) return index;
    return -1;
}

QString PresetEditor::selectedPresetId() const {
    const QTreeWidgetItem *item = presetList_->currentItem();
    return item ? item->data(PresetName, kIdRole).toString() : QString();
}

int PresetEditor::selectedOperationRow() const {
    const QTreeWidgetItem *item = operationList_->currentItem();
    return item ? operationList_->indexOfTopLevelItem(const_cast<QTreeWidgetItem *>(item)) : -1;
}

void PresetEditor::selectPreset(const QString &presetId) {
    for (int row = 0; row < presetList_->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = presetList_->topLevelItem(row);
        if (item->data(PresetName, kIdRole).toString() != presetId) continue;
        presetList_->setCurrentItem(item);
        return;
    }
}

void PresetEditor::selectOperation(int row) {
    if (row < 0 || row >= operationList_->topLevelItemCount()) return;
    operationList_->setCurrentItem(operationList_->topLevelItem(row));
}

}  // namespace ui
