#include "app/lifecycle/quit_guard.h"

#include <QCoreApplication>
#include <QEvent>

namespace app::lifecycle {

QuitGuard::QuitGuard(QObject *parent) : QObject(parent) {}

bool QuitGuard::eventFilter(QObject *object, QEvent *event) {
    // Verbatim from main.cpp:39-44. The qApp check matters: QEvent::Quit is
    // delivered to the application object, and filtering it anywhere else would
    // swallow unrelated events. QEvent::Quit also covers the native application
    // menu and the tray action, which is why this is an event filter rather
    // than a connection to a menu item.
    if (object == QCoreApplication::instance() && event->type() == QEvent::Quit && !approved_) {
        event->ignore();
        emit quitRequested();
        return true;
    }
    return QObject::eventFilter(object, event);
}

}  // namespace app::lifecycle
