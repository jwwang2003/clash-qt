#include "ui/toggle_switch.h"

#include <QPainter>
#include "ui/theme.h"

namespace ui {
namespace {
constexpr int kTrackWidth = 32;
constexpr int kTrackHeight = 18;
constexpr int kGap = 8;
}

ToggleSwitch::ToggleSwitch(const QString &text, QWidget *parent) : QCheckBox(text, parent) {
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

QSize ToggleSwitch::sizeHint() const {
    return {kTrackWidth + kGap + fontMetrics().horizontalAdvance(text()) + 8,
            qMax(kTrackHeight, fontMetrics().height()) + 10};
}

void ToggleSwitch::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto &t = theme::tokens();
    if (!isEnabled()) painter.setOpacity(0.45);
    const bool rtl = layoutDirection() == Qt::RightToLeft;
    const QRectF track(rtl ? width() - kTrackWidth - 3 : 3,
                       (height() - kTrackHeight) / 2.0, kTrackWidth, kTrackHeight);
    painter.setPen(QPen(isChecked() ? t.accent : t.textFaint, 1));
    painter.setBrush(isChecked() ? t.accent : t.surfaceAlt);
    painter.drawRoundedRect(track, kTrackHeight / 2.0, kTrackHeight / 2.0);
    const bool right = isChecked() != rtl;
    const qreal centerX = right ? track.right() - 9 : track.left() + 9;
    painter.setPen(Qt::NoPen);
    painter.setBrush(isChecked() ? t.accentText : t.textDim);
    painter.drawEllipse(QPointF(centerX, track.center().y()), 6, 6);
    painter.setPen(t.text);
    const QRect label = rtl ? QRect(3, 0, width() - kTrackWidth - kGap - 6, height())
                            : QRect(kTrackWidth + kGap + 3, 0, width() - kTrackWidth - kGap - 6, height());
    painter.drawText(label, Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft), text());
    if (hasFocus()) {
        painter.setPen(QPen(t.accent, 1, Qt::DotLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    }
}

}  // namespace ui
