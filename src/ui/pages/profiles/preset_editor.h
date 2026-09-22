#pragma once

#include <QJsonObject>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

namespace core {
class ProfileStore;
}

namespace ui {

class ComboBox;

/// Edits the preset document the store owns (contract revision config-r1): the
/// global chain and one profile's chain, each an ordered list of presets, each
/// preset an ordered list of operations.
///
/// Order is semantic in both lists -- presets apply in array order, and global
/// presets apply before the selected profile's -- so neither list is ever
/// sorted, and both are reordered explicitly.
///
/// The editor keeps no authoritative state. Every accepted edit is written back
/// through ProfileStore::setPresetDocument() and the display is rebuilt from
/// ProfileStore::presetDocument() afterwards, so a rejected write -- which
/// leaves the persisted document untouched -- is also recovered by re-reading
/// it. What the lists show is therefore what is on disk, not what was typed.
class PresetEditor : public QWidget {
    Q_OBJECT

public:
    explicit PresetEditor(core::ProfileStore *store, QWidget *parent = nullptr);

    /// The profile the per-profile scope edits. An empty uid leaves that scope
    /// unavailable and falls back to the global chain.
    void setProfile(const QString &uid, const QString &name);

    /// Scope on display: empty for the global chain, else the profile's uid.
    QString scopeUid() const;

signals:
    /// The store accepted a document written here; `profileUid` is the scope it
    /// was written in, so a preview can be requested for the chain just edited.
    void presetsCommitted(const QString &profileUid);
    /// The user switched scope: a preview of the other chain is now the one
    /// that matches what is on screen.
    void scopeChanged(const QString &profileUid);

private:
    void reload();
    void reloadOperations();
    void loadPresetForm();
    void loadOperationForm();
    void updateActions();
    void showError(const QString &message);
    bool commit(const QJsonObject &document);

    void addPreset();
    void renamePreset();
    void removePreset();
    void movePreset(int delta);
    void setPresetEnabled(const QString &presetId, bool enabled);

    void addOperation();
    void applyOperation();
    void removeOperation();
    void moveOperation(int delta);

    /// The operation the form describes, or false with `error` filled in.
    bool draftOperation(QJsonObject *operation, QString *error) const;

    int presetIndex(const QString &presetId) const;
    QString selectedPresetId() const;
    int selectedOperationRow() const;
    void selectPreset(const QString &presetId);
    void selectOperation(int row);

    core::ProfileStore *store_;
    QJsonObject document_;  // last document read back from the store
    QString profileUid_;
    QString profileName_;
    bool loading_ = false;

    ComboBox *scopeBox_;
    QTreeWidget *presetList_;
    QTreeWidget *operationList_;
    QLabel *emptyLabel_;
    QLabel *errorLabel_;
    QLineEdit *nameEdit_;
    QPushButton *addPresetButton_;
    QPushButton *renamePresetButton_;
    QPushButton *removePresetButton_;
    QPushButton *presetUpButton_;
    QPushButton *presetDownButton_;
    ComboBox *kindBox_;
    QLineEdit *pathEdit_;
    QPlainTextEdit *valueEdit_;
    QLabel *valueLabel_;
    QPushButton *addOperationButton_;
    QPushButton *applyOperationButton_;
    QPushButton *removeOperationButton_;
    QPushButton *operationUpButton_;
    QPushButton *operationDownButton_;
};

}  // namespace ui
