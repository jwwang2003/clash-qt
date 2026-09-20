#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QJsonObject>
#include <optional>

#include "core/types.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTimer;

namespace platform { class PrivilegedServiceClient; }

namespace core {

enum class CoreState { Stopped, Starting, Running, Stopping, Failed };

/// Owns the mihomo child process: locating a binary, launching it against a
/// generated config, and reporting liveness.
///
/// Contract with the ui module. Extend, do not reshape.
class CoreProcess : public QObject {
    Q_OBJECT

public:
    explicit CoreProcess(QObject *parent = nullptr, platform::PrivilegedServiceClient *serviceClient = nullptr);
    ~CoreProcess() override;

    /// Looks for a usable mihomo in PATH, then in a local Clash Verge Rev
    /// install. Empty when none is found.
    static QString discoverBinary();

    void setBinaryPath(const QString &path);
    QString binaryPath() const;
    bool setUseService(bool enabled);
    bool isServiceMode() const;
    bool usesPrivilegedService() const;
    static bool serviceSupported();
    static bool serviceAvailable();

    /// Launches the core with `configPath` as its config and `workDir` as its
    /// home directory (where it expects Country.mmdb, geosite.dat and friends).
    void start(const QString &configPath, const QString &workDir);
    void stop();
    void restart();

    CoreState state() const;
    /// Controller the running core listens on, parsed from the config it was
    /// launched with.
    Endpoint endpoint() const;
    QStringList activeConfigPaths() const;
    bool isRestartPending() const;

signals:
    void stateChanged(CoreState state);
    void ready(const Endpoint &endpoint);
    void logLine(const QString &line);
    void failed(const QString &reason);
    void stopped();
    /// Terminal response to stop(): false means lease cleanup was requested but
    /// service disconnect prevented confirmation that the privileged child exited.
    void stopFinished(bool confirmed, const QString &error);

private:
    void finishValidation(bool valid, const QString &reason = {});
    void cancelValidation();
    void launchValidated(const QString &configPath, const QString &workDir, const QString &binary);
    void launchProcess(const QString &configPath, const QString &workDir, const QString &binary);
    void finishStop();
    void awaitController(const std::optional<Endpoint> &serviceEndpoint = std::nullopt);
    void serviceDisconnected();
    void probeController();
    void cancelProbe();
    void drainOutput();
    void publishLine(QString line);
    void handleFinished(int exitCode);
    void terminateProcess();
    void discardProcess();
    void setState(CoreState state);
    void fail(const QString &reason);

    struct LaunchRequest { QString configPath, workDir, binary; };
    std::optional<LaunchRequest> pendingLaunch_;
    platform::PrivilegedServiceClient *serviceClient_ = nullptr;
    QTimer *servicePoll_ = nullptr;
    QJsonObject serviceConfig_;
    QString serviceLogTail_;
    bool useService_ = false;
    bool serviceActive_ = false;
    bool serviceStopping_ = false;
    bool serviceParsing_ = false;
    bool explicitStop_ = false;
    bool injectedService_ = false;
    quint64 launchGeneration_ = 0;
    QProcess *process_ = nullptr;
    QProcess *retiring_ = nullptr;
    CoreState stopResult_ = CoreState::Stopped;
    QString stopError_;
    QProcess *validation_ = nullptr;
    QHash<QProcess *, QString> cancelingValidations_;
    QString validatingConfigPath_;
    QString validatingWorkDir_;
    QString validatingBinary_;
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
};

}  // namespace core
