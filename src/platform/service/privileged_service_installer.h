#pragma once

#include <QObject>
#include <QString>

namespace platform {

/// Requests native administrator authorization only from explicit install/remove
/// calls. Work and completion-pipe reads run off the GUI thread.
class PrivilegedServiceInstaller : public QObject {
    Q_OBJECT
public:
    explicit PrivilegedServiceInstaller(QObject *parent = nullptr);
    static bool isSupported();
    static QString bundledHelperPath();
    bool isBusy() const { return busy_; }
    bool isInstalled() const { return installed_; }

public slots:
    void install(const QString &corePath);
    void uninstall();
    void refresh();

signals:
    void busyChanged(bool busy);
    void statusChanged(bool installed, const QString &error);
    void finished(bool installed, bool success, const QString &error);

private:
    void run(bool install, const QString &corePath);
    bool busy_ = false;
    bool installed_ = false;
    quint64 refreshEpoch_ = 0;
};
} // namespace platform
