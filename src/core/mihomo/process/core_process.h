#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QJsonObject>
#include <atomic>
#include <memory>
#include <optional>

#include "core/mihomo/process/engine_discovery.h"
#include "core/backend/privileged_core_service.h"
#include "core/types.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QThreadPool;
class QTimer;

namespace core {

enum class CoreState { Stopped, Starting, Running, Stopping, Failed };

/// Why the last failure happened, as a code rather than as prose. The `failed`
/// signal carries a localised, human message; a message is not something a
/// caller can switch on, and matching on its text breaks the moment it is
/// translated. Read it from lastFailure() while handling `failed` or
/// `stopFinished`.
enum class CoreFailure : quint8 {
    None = 0,
    Unspecified,
    BinaryNotFound,
    ValidationFailed,
    LaunchFailed,
    CoreExited,
    ReadyTimeout,
    ServiceUnavailable,
    ServiceDisconnected,
    ConfigUnreadable,
};

/// Readiness and termination budget. The values are the backend contract's
/// (docs/module-api.md section 3) and are what the defaults below hold; they are
/// instance state rather than file-scope constants so that a test can observe a
/// silence-based deadline being refreshed without waiting ten real seconds, and
/// so that BackendCapabilities::timings() can report the backend's REAL budget
/// rather than a constant that might have drifted from it.
struct CoreTimings {
    int probeIntervalMs = 200;
    int probeTimeoutMs = 2000;
    int idleDeadlineMs = 10000;
    int serviceIdleDeadlineMs = 60000;
    int hardCapMs = 180000;
    int terminateWaitMs = 3000;
    int validationTimeoutMs = 15000;
};

/// Owns the mihomo child process: locating a binary, launching it against a
/// generated config, and reporting liveness.
///
/// Contract with the ui module. Extend, do not reshape.
class CoreProcess : public QObject, private PrivilegedCoreServiceListener {
    Q_OBJECT

public:
    /// `service` is the privileged-execution seam, an interface this component
    /// owns rather than a platform type. Null means this
    /// component was given no privileged service, and service mode is then
    /// REFUSED rather than silently selected. The composition root adapts
    /// platform::PrivilegedServiceClient onto the interface; the platform type is
    /// deliberately absent from this signature.
    explicit CoreProcess(QObject *parent = nullptr, PrivilegedCoreService *service = nullptr);
    ~CoreProcess() override;

    /// The MANAGED engine path, or empty: the staged engine (or the local
    /// build $CLASH_QT_CORE_BINARY names) and nothing else. It never returns an
    /// executable found on PATH or belonging to another Clash installation -
    /// those are offered by core::externalEngineCandidates() as an explicit,
    /// separately labelled user choice.
    static QString discoverBinary();

    void setBinaryPath(const QString &path);
    QString binaryPath() const;
    /// What this process would run right now, with its label and provenance.
    /// Always reportable: when nothing resolves, problem() carries what to do.
    EngineResolution engine() const;
    /// The engine the last start() actually launched. Empty before the first.
    EngineResolution resolvedEngine() const;

    bool setUseService(bool enabled);
    bool isServiceMode() const;
    bool usesPrivilegedService() const;
    /// Instance answers, from the injected service. The statics below describe
    /// the PLATFORM and are kept only for existing callers; a component that was
    /// handed no privileged service has none, whatever the platform supports.
    bool isServiceSupported() const;
    bool isServiceAvailable() const;
    static bool serviceSupported();
    static bool serviceAvailable();

    CoreTimings timings() const;
    void setTimings(const CoreTimings &timings);

    /// Launches the core with `configPath` as its config and `workDir` as its
    /// home directory (where it expects Country.mmdb, geosite.dat and friends).
    void start(const QString &configPath, const QString &workDir);
    void stop();
    void restart();

    CoreState state() const;
    /// The code behind the most recent `failed` / unconfirmed `stopFinished`.
    CoreFailure lastFailure() const;
    /// Asks the privileged service for its status. The answer arrives on
    /// serviceStatusReceived(). Published so a consumer does not open a SECOND
    /// connection to the one privileged socket: two live connections to it is a
    /// correctness hazard, not a layering complaint.
    void requestServiceStatus();
    /// Controller the running core listens on, parsed from the config it was
    /// launched with.
    Endpoint endpoint() const;
    QStringList activeConfigPaths() const;
    bool isRestartPending() const;
    /// How many readiness-probe completions reached this object AFTER the probe
    /// that produced them was cancelled.
    ///
    /// docs/module-api.md section 5.2 requires the probe to disconnect its
    /// signals BEFORE aborting, precisely so an abort cannot deliver a
    /// completion into a torn-down handler - QNetworkReply::abort() emits
    /// finished() synchronously. Without this counter that ordering is
    /// unobservable from outside: the handler's own `probeReply_ != reply` guard
    /// swallows the late completion, so inverting the two statements changes
    /// nothing a test can see and the acceptance bullet passes vacuously.
    /// It must always read 0.
    int probeCompletionsAfterCancel() const;

    /// Runnables this object has SUBMITTED to the pool it owns and has not yet
    /// seen exit - queued ones included. Today that is the service-mode config
    /// parse, which is moved off the event loop because an 8 MiB YAML document
    /// must not stall it.
    ///
    /// WHY THIS IS PUBLISHED AT ALL. When this class is compiled into a
    /// dynamically loaded module, the runnable's instructions live in that
    /// module's image. A watcher that has emitted finished(), or a cancel flag
    /// that has been set, says the HOST's half is over; neither says the
    /// runnable has left the module's code. A consumer deciding whether the
    /// image may be unmapped needs the second fact, and this is it. The
    /// destructor's wait is what makes the answer eventually zero; this is what
    /// makes the waiting observable, and reportable as a timeout, rather than a
    /// silent stall.
    int pendingNativeWork() const noexcept;

signals:
    void stateChanged(CoreState state);
    void ready(const Endpoint &endpoint);
    void logLine(const QString &line);
    void failed(const QString &reason);
    void stopped();
    /// Terminal response to stop(): false means lease cleanup was requested but
    /// service disconnect prevented confirmation that the privileged child exited.
    void stopFinished(bool confirmed, const QString &error);
    /// Whatever engine a launch resolved is reported, so provenance can never
    /// be implied. Emitted once per start(), before the child is launched.
    void engineResolved(const QString &path, const QString &label, const QString &provenance);
    /// Re-published from the injected privileged service, so no consumer has to
    /// open a second connection of its own.
    void serviceStatusReceived(const QJsonObject &status);
    void serviceConnectedChanged(bool connected);

private:
    // --- PrivilegedCoreServiceListener
    void privilegedConnectedChanged(bool connected) override;
    void privilegedStatusReceived(const QJsonObject &status) override;
    void privilegedCoreStarted(const QJsonObject &endpoint) override;
    void privilegedCoreStopped() override;
    void privilegedLogsReceived(const QString &logs) override;
    void privilegedRequestFinished(const QString &operation, bool success,
                                   const QString &error) override;

    void finishValidation(bool valid, const QString &reason = {});
    void cancelValidation();
    /// Asks the in-flight service-config parse to stop as soon as it can. A
    /// queued runnable then exits without doing any work; a running one still
    /// has to finish, which is exactly why pendingNativeWork() exists.
    void cancelServiceParse();
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
    void fail(CoreFailure kind, const QString &reason);
    void reportFailure(CoreFailure kind, const QString &reason);

    struct LaunchRequest { QString configPath, workDir, binary; };
    std::optional<LaunchRequest> pendingLaunch_;
    std::unique_ptr<PrivilegedCoreService> ownedService_;
    PrivilegedCoreService *service_ = nullptr;
    QTimer *servicePoll_ = nullptr;
    QJsonObject serviceConfig_;
    QString serviceLogTail_;
    CoreTimings timings_;
    EngineResolution resolvedEngine_;
    CoreFailure lastFailure_ = CoreFailure::None;
    bool useService_ = false;
    bool serviceActive_ = false;
    bool serviceStopping_ = false;
    bool serviceParsing_ = false;
    /// OWNED, not QThreadPool::globalInstance(). A runnable on the global pool
    /// outlives this object by construction - nothing here can wait for it -
    /// and if this class was compiled into a loadable module, that is a
    /// runnable executing code the loader is free to unmap. An owned pool can
    /// be, and is, waited on in the destructor. Created on first use, because
    /// only service mode ever submits anything.
    std::unique_ptr<QThreadPool> parsePool_;
    /// Shared with the runnable rather than owned outright, so the counter is
    /// still valid for the decrement even in the window between the runnable's
    /// last statement and the pool's own bookkeeping.
    std::shared_ptr<std::atomic<int>> parseWork_;
    std::shared_ptr<std::atomic<bool>> parseCancelled_;
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
    // ABSOLUTE, and deliberately never refreshed: publishLine() moves
    // readyDeadlineMs_ only. Contract section 3 - a core that keeps logging
    // keeps its IDLE deadline alive, and the hard cap still ends it.
    qint64 readyHardDeadlineMs_ = 0;
    int probeCompletionsAfterCancel_ = 0;
};

}  // namespace core
