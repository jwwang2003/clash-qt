#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace core {
class ConfigEnhancer;
}

namespace ui {

/// The enhancement chain in the order it runs. The order is semantic, so the
/// list is reorderable and never sorted.
class ChainEditor : public QWidget {
    Q_OBJECT

public:
    explicit ChainEditor(core::ConfigEnhancer *enhancer, QWidget *parent = nullptr);

private:
    void reload();
    void updateActions();
    void addStep(bool script);
    void importStep();
    void editStep();
    void renameStep();
    void removeStep();
    void move(int delta);
    QString selectedUid() const;

    core::ConfigEnhancer *enhancer_;
    QTreeWidget *list_;
    QLabel *emptyLabel_;
    QPushButton *renameButton_;
    QPushButton *editButton_;
    QPushButton *removeButton_;
    QPushButton *upButton_;
    QPushButton *downButton_;
};

}  // namespace ui
