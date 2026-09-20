#include "ui/tray_icon.h"

#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QPixmap>

#include "ui/formatting.h"
#include "ui/main_window.h"

namespace ui {
namespace {

/// Placeholder mark so the tray slot is visible before real assets exist.
QIcon placeholderIcon() {
    QPixmap pixmap(22, 22);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawEllipse(3, 3, 16, 16);
    painter.drawLine(7, 11, 15, 11);
    QIcon icon(pixmap);
    icon.setIsMask(true);  // macOS tints template icons for light/dark menu bars.
    return icon;
}

}  // namespace

TrayIcon::TrayIcon(MainWindow *window, QObject *parent)
    : QSystemTrayIcon(placeholderIcon(), parent), window_(window), menu_(new QMenu) {
    menu_->addAction(QObject::tr("Show Window"), window_, [this] {
        window_->show();
        window_->raise();
        window_->activateWindow();
    });
    menu_->addSeparator();
    menu_->addAction(QObject::tr("Quit"), qApp, &QApplication::quit);

    setContextMenu(menu_);
    setToolTip("clash-qt");

    connect(this, &QSystemTrayIcon::activated, this, [this](ActivationReason reason) {
        if (reason != Trigger && reason != DoubleClick) return;
        if (window_->isVisible()) {
            window_->hide();
        } else {
            window_->show();
            window_->raise();
            window_->activateWindow();
        }
    });
}

void TrayIcon::setTraffic(quint64 up, quint64 down) {
    setToolTip(QString("clash-qt\n↑ %1\n↓ %2").arg(formatRate(up), formatRate(down)));
}

}  // namespace ui
