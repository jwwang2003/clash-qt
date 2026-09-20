#pragma once

#include <QSystemTrayIcon>

class QMenu;

namespace ui {

class MainWindow;

/// Tray entry point. The window is hidden rather than destroyed on close, so the
/// app keeps running in the tray the way Verge Rev does.
class TrayIcon : public QSystemTrayIcon {
    Q_OBJECT

public:
    explicit TrayIcon(MainWindow *window, QObject *parent = nullptr);

    void setTraffic(quint64 up, quint64 down);

private:
    MainWindow *window_;
    QMenu *menu_;
};

}  // namespace ui
