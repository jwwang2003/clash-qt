#pragma once

#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QVector>
#include <QWidget>

#include "core/profiles/profile_store.h"

class QLabel;
class QLineEdit;
class QListView;

namespace ui {

class EffectiveConfigView;
class PresetEditor;

class ProfileModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        ProfileRole = Qt::UserRole + 1,
        CurrentRole,
        IntervalRole,
    };

    explicit ProfileModel(QObject *parent = nullptr);

    void setProfiles(const QVector<core::Profile> &profiles, const QString &currentUid);
    core::Profile profileAt(const QModelIndex &index) const;
    QString currentUid() const { return currentUid_; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

signals:
    void updateIntervalEdited(const QString &uid, int minutes);

private:
    QVector<core::Profile> profiles_;
    QString currentUid_;
};

class ProfileDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit ProfileDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override;
    void setEditorData(QWidget *editor, const QModelIndex &index) const override;
    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override;
};

class ProfilesPage : public QWidget {
    Q_OBJECT

public:
    explicit ProfilesPage(core::ProfileStore *store, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onProfilesChanged(const QVector<core::Profile> &profiles, const QString &currentUid);
    void onErrorOccurred(const QString &message);
    void showContextMenu(const QPoint &pos);

private:
    void importUrl();
    void importFile();
    void editProfile(const core::Profile &profile);
    /// Points the preset editor at the profile the list has selected, so the
    /// per-profile scope always edits the chain the user is looking at.
    void updatePresetScope();

    core::ProfileStore *store_;
    ProfileModel *model_;
    QListView *view_;
    QLineEdit *urlEdit_;
    QLabel *errorLabel_;
    QLabel *emptyLabel_;
    PresetEditor *presetEditor_;
    EffectiveConfigView *preview_;
    bool previewRequested_ = false;
};

}  // namespace ui
