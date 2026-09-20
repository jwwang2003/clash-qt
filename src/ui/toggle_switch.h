#pragma once

#include <QCheckBox>

namespace ui {

class ToggleSwitch : public QCheckBox {
    Q_OBJECT
public:
    explicit ToggleSwitch(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent *event) override;
    bool hitButton(const QPoint &position) const override { return rect().contains(position); }
};

}  // namespace ui
