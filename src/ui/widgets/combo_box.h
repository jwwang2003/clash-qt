#pragma once

#include <QComboBox>

class QActionGroup;
class QMenu;

namespace ui {

// Selectors share the same content-sized popup as the application's other menus.
class ComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit ComboBox(QWidget *parent = nullptr);
    void showPopup() override;
    void hidePopup() override;

private:
    void setMenuOpen(bool open);
    QMenu *menu_;
    QActionGroup *choices_;
};

}  // namespace ui
