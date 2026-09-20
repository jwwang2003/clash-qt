#include "ui/combo_box.h"

#include <QActionGroup>
#include <QAbstractItemModel>
#include <QApplication>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QStyle>

namespace ui {

ComboBox::ComboBox(QWidget *parent)
    : QComboBox(parent), menu_(new QMenu(this)), choices_(new QActionGroup(menu_)) {
    menu_->setObjectName("comboPopupMenu");
    menu_->setToolTipsVisible(true);
    menu_->setStyleSheet("QMenu { menu-scrollable: 1; }");
    choices_->setExclusive(true);
    connect(menu_, &QMenu::aboutToHide, this, [this] {
        QComboBox::hidePopup();
        setMenuOpen(false);
    });
}

void ComboBox::setMenuOpen(bool open) {
    setProperty("menuOpen", open);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void ComboBox::showPopup() {
    if (!isEnabled() || count() == 0 || menu_->isVisible()) return;
    menu_->clear();
    menu_->setFont(QApplication::font("QMenu"));
    QPointer<QAction> current;
    for (int row = 0; row < count(); ++row) {
        const QPersistentModelIndex index(model()->index(row, modelColumn(), rootModelIndex()));
        if (index.data(Qt::AccessibleDescriptionRole).toString() == "separator") {
            menu_->addSeparator();
            continue;
        }
        auto *action = menu_->addAction(itemIcon(row), QString(itemText(row)).replace('&', "&&"));
        action->setData(QVariant::fromValue(index));
        action->setToolTip(index.data(Qt::ToolTipRole).toString());
        action->setCheckable(true);
        action->setEnabled(index.flags().testFlag(Qt::ItemIsEnabled) &&
                           index.flags().testFlag(Qt::ItemIsSelectable));
        choices_->addAction(action);
        action->setChecked(row == currentIndex());
        if (row == currentIndex()) current = action;
        connect(action, &QAction::triggered, this, [this, index] {
            if (!index.isValid() || index.model() != model() || index.parent() != rootModelIndex() ||
                !index.flags().testFlag(Qt::ItemIsEnabled) || !index.flags().testFlag(Qt::ItemIsSelectable)) return;
            const int row = index.row();
            const QString text = index.data(Qt::DisplayRole).toString();
            const QPointer<ComboBox> guard(this);
            setCurrentIndex(row);
            if (!guard) return;
            emit activated(row);
            if (guard) emit textActivated(text);
        });
        connect(action, &QAction::hovered, this, [this, index] {
            if (!index.isValid() || index.model() != model()) return;
            const QString text = index.data(Qt::DisplayRole).toString();
            const QPointer<ComboBox> guard(this);
            emit highlighted(index.row());
            if (guard) emit textHighlighted(text);
        });
    }
    // The closed control can be compact, but the menu must fit every full label.
    menu_->setMinimumWidth(width());
    setMenuOpen(true);
    const QPointer<ComboBox> guard(this);
    menu_->popup(mapToGlobal(QPoint(0, height())));
    if (guard && current && current->isEnabled()) menu_->setActiveAction(current);
}

void ComboBox::hidePopup() {
    if (menu_->isVisible()) menu_->hide();
    else {
        QComboBox::hidePopup();
        setMenuOpen(false);
    }
}

}  // namespace ui
