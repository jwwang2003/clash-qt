#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "core/types.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTimer;

namespace core {

enum class CoreState { Stopped, Starting, Running, Failed };

/// Owns the mihomo child process: locating a binary, launching it against a
/// generated config, and reporting liveness.
///
/// Contract with the ui module. Extend, do not reshape.
class CoreProcess : public QObject {
    Q_OBJECT

public:
    explicit CoreProcess(QObject *parent = nullptr);
    ~CoreProcess() override;

    /// Looks for a usable mihomo in PATH, then in a local Clash Verge Rev
    /// install. Empty when none is found.
    static QString discoverBinary();

    void setBinaryPath(const QString &path);
    QString binaryPath() const;

    /// Launches the core with `configPath` as its config and `workDir` as its
    /// home directory (where it expects Country.mmdb, geosite.dat and friends).
    void start(const QString &configPath, const QString &workDir);
    void stop();
    void restart();

    CoreState state() const;
    /// Controller the running core listens on, parsed from the config it was
    /// launched with.
    Endpoint endpoint() const;

signals:
    void stateChanged(CoreState state);
    void ready(const Endpoint &endpoint);
    void logLine(const QString &line);
    void failed(const QString &reason);

private:
    void awaitController();
    void probeController();
    void drainOutput();
    void publishLine(QString line);
    void handleFinished(int exitCode);
    void terminateProcess();
    void discardProcess();
    void setState(CoreState state);
    void fail(const QString &reason);

    QProcess *process_ = nullptr;
    QString binaryPath_;
    QString configPath_;
    QString workDir_;
    CoreState state_ = CoreState::Stopped;
    Endpoint endpoint_;

    QNetworkAccessManager *network_;
    QNetworkReply *probeReply_ = nullptr;
    QTimer *readyTimer_ = nullptr;
    Endpoint pendingEndpoint_;
    QString logBuffer_;
    QStringList logTail_;
    qint64 readyDeadlineMs_ = 0;
    qint64 readyHardDeadlineMs_ = 0;
    bool stopRequested_ = false;
};

}  // namespace core
