#include "core/mihomo/process/core_process.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QtConcurrentRun>
#include <yaml-cpp/yaml.h>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>

#include "core/mihomo/controller_discovery.h"
#include "core/config/yaml_util.h"

namespace core {
namespace {

constexpr int kLogTailLines = 8;

/// "no controller", explicitly. core::Endpoint's default is the localhost
/// DEFAULT (127.0.0.1:9090), which isValid() accepts, so assigning Endpoint{}
/// to mean "cleared" made a stopped core look as though it were listening on
/// 9090. The published managedEndpoint() has to answer false there.
Endpoint noEndpoint() {
    Endpoint endpoint;
    endpoint.host.clear();
    endpoint.port = 0;
    endpoint.secret.clear();
    return endpoint;
}

QJsonValue serviceJson(const YAML::Node &node, int depth = 0) {
    if (depth > 128) throw YAML::BadConversion(node.Mark());
    if (node.IsMap()) {
        QJsonObject object;
        for (const auto &entry : node) {
            if (!entry.first.IsScalar()) throw YAML::BadConversion(entry.first.Mark());
            object.insert(QString::fromStdString(entry.first.Scalar()), serviceJson(entry.second, depth + 1));
        }
        return object;
    }
    if (node.IsSequence()) {
        QJsonArray array;
        for (const auto &item : node) array.append(serviceJson(item, depth + 1));
        return array;
    }
    if (node.IsScalar()) {
        const QString scalar = QString::fromStdString(node.Scalar());
        return node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str"
            ? QJsonValue(scalar) : yamlutil::plainToJson(scalar);
    }
    return QJsonValue::Null;
}

}  // namespace

CoreProcess::CoreProcess(QObject *parent, PrivilegedCoreService *service)
    : QObject(parent),
      ownedService_(service ? nullptr
                            : std::unique_ptr<PrivilegedCoreService>(new NullPrivilegedCoreService)),
      service_(service ? service : ownedService_.get()),
      servicePoll_(new QTimer(this)), injectedService_(service != nullptr),
      network_(new QNetworkAccessManager(this)) {
    // A plain listener, not signal/slot: signals are what put a platform type on
    // this class's surface in the first place (DECISION D2).
    endpoint_ = noEndpoint();
    service_->setListener(this);
    servicePoll_->setInterval(1000);
    connect(servicePoll_, &QTimer::timeout, this, [this] {
        if (!serviceActive_ || serviceStopping_ || service_->isBusy()) return;
        service_->requestLogs();
        service_->requestStatus();
    });
}

CoreProcess::~CoreProcess() {
    service_->setListener(nullptr);
    service_->close();
    cancelValidation();
    cancelProbe();
    for (QProcess *process : findChildren<QProcess *>()) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

// ---------------------------------------------------------------- the service

void CoreProcess::privilegedCoreStarted(const QJsonObject &endpoint) {
    if (!serviceActive_ || serviceStopping_) return;
    Endpoint controller;
    controller.host = endpoint.value("host").toString();
    controller.port = endpoint.value("port").toInt();
    controller.secret = endpoint.value("secret").toString();
    if (!controller.isValid()) {
        fail(CoreFailure::ServiceUnavailable,
             tr("The privileged service returned an invalid controller endpoint."));
        return;
    }
    servicePoll_->start();
    awaitController(controller);
}

void CoreProcess::privilegedCoreStopped() {
    if (!serviceActive_) return;
    servicePoll_->stop();
    serviceActive_ = false;
    serviceStopping_ = false;
    finishStop();
}

void CoreProcess::privilegedLogsReceived(const QString &lines) {
    if (!serviceActive_) return;
    QString fresh = lines;
    if (lines.startsWith(serviceLogTail_)) fresh = lines.mid(serviceLogTail_.size());
    else {
        const QStringList previousLines = serviceLogTail_.split('\n', Qt::SkipEmptyParts);
        const QStringList currentLines = lines.split('\n', Qt::SkipEmptyParts);
        qsizetype overlap = std::min(previousLines.size(), currentLines.size());
        while (overlap > 0 && previousLines.sliced(previousLines.size() - overlap) != currentLines.first(overlap))
            --overlap;
        fresh = currentLines.sliced(overlap).join('\n');
    }
    serviceLogTail_ = lines;
    for (const QString &line : fresh.split('\n', Qt::SkipEmptyParts)) publishLine(line);
}

void CoreProcess::privilegedStatusReceived(const QJsonObject &status) {
    emit serviceStatusReceived(status);
    if (serviceActive_ && !serviceStopping_ && (state_ == CoreState::Running || state_ == CoreState::Starting) &&
        status.value("state").toString() == "stopped") {
        const QString details = logTail_.join('\n');
        fail(CoreFailure::CoreExited,
             details.isEmpty() ? tr("The privileged core exited unexpectedly.")
                              : tr("The privileged core exited unexpectedly.\n%1").arg(details));
    }
}

void CoreProcess::privilegedConnectedChanged(bool connected) {
    emit serviceConnectedChanged(connected);
    if (!connected && serviceActive_) serviceDisconnected();
}

void CoreProcess::requestServiceStatus() { service_->requestStatus(); }

CoreFailure CoreProcess::lastFailure() const { return lastFailure_; }

void CoreProcess::privilegedRequestFinished(const QString &operation, bool success,
                                            const QString &error) {
    if (success || !serviceActive_) return;
    if (operation == "start") {
        servicePoll_->stop();
        serviceActive_ = false;
        serviceStopping_ = false;
        if (stopResult_ == CoreState::Stopped && state_ == CoreState::Stopping) finishStop();
        else fail(CoreFailure::ServiceUnavailable, tr("Privileged core start failed: %1").arg(error));
    } else if (operation == "stop") {
        servicePoll_->stop();
        serviceActive_ = false;
        serviceStopping_ = false;
        service_->close();
        fail(CoreFailure::ServiceUnavailable, tr("Privileged core stop failed: %1").arg(error));
    }
}

// ----------------------------------------------------------------- the engine

QString CoreProcess::discoverBinary() { return resolveManagedEngine().path; }

void CoreProcess::setBinaryPath(const QString &path) { binaryPath_ = path; }
QString CoreProcess::binaryPath() const { return binaryPath_; }

EngineResolution CoreProcess::engine() const {
    return binaryPath_.isEmpty() ? resolveManagedEngine() : describeChosenEngine(binaryPath_);
}

EngineResolution CoreProcess::resolvedEngine() const { return resolvedEngine_; }

CoreTimings CoreProcess::timings() const { return timings_; }

void CoreProcess::setTimings(const CoreTimings &timings) {
    timings_ = timings;
    if (readyTimer_) readyTimer_->setInterval(timings_.probeIntervalMs);
}

bool CoreProcess::serviceSupported() {
#ifdef Q_OS_MACOS
    return true;
#else
    return false;
#endif
}
bool CoreProcess::serviceAvailable() {
    return serviceSupported() && QFileInfo::exists("/var/run/org.clash-qt.service/socket");
}
bool CoreProcess::isServiceSupported() const { return service_->isSupported(); }
bool CoreProcess::isServiceAvailable() const { return service_->isAvailable(); }

bool CoreProcess::setUseService(bool enabled) {
    // The injected service is the authority. A component handed no privileged
    // service has none, whatever the platform supports.
    if (enabled && !service_->isSupported()) return false;
    if (state_ != CoreState::Stopped && state_ != CoreState::Failed) return false;
    if (process_ || retiring_ || validation_ || serviceActive_ || serviceParsing_ || !cancelingValidations_.isEmpty()) return false;
    useService_ = enabled;
    return true;
}
bool CoreProcess::isServiceMode() const { return useService_; }
bool CoreProcess::usesPrivilegedService() const { return serviceActive_; }

void CoreProcess::serviceDisconnected() {
    const bool wasStopping = serviceStopping_;
    const QString connectionError = service_->connectionError();
    servicePoll_->stop();
    serviceActive_ = false;
    serviceStopping_ = false;
    if (wasStopping && stopResult_ == CoreState::Stopped) {
        // The helper owns the lease and terminates its core on disconnect.
        pendingLaunch_.reset();
        stopResult_ = CoreState::Failed;
        stopError_ = tr("The privileged service disconnected before confirming core shutdown.");
        if (!connectionError.isEmpty()) stopError_ += '\n' + connectionError;
        lastFailure_ = CoreFailure::ServiceDisconnected;
        finishStop();
    } else {
        QString reason = tr("The privileged service connection was lost. Its core lease has ended.");
        if (!connectionError.isEmpty()) reason += '\n' + connectionError;
        fail(CoreFailure::ServiceDisconnected, reason);
    }
}

void CoreProcess::start(const QString &configPath, const QString &workDir) {
    explicitStop_ = false;
    ++launchGeneration_;
    serviceParsing_ = false;
    cancelValidation();
    pendingLaunch_.reset();
    if ((process_ || serviceActive_) && state_ != CoreState::Running) {
        stopResult_ = CoreState::Stopped;
        stopError_.clear();
        terminateProcess();
    }
    // G1. The managed engine is the one this project staged, or the local build
    // CLASH_QT_CORE_BINARY names. There is no fallback to PATH and none to
    // another Clash installation: an engine we did not build is an explicit
    // choice the user makes, and it is reported as such.
    const EngineResolution resolution = engine();
    if (!resolution.isResolved()) {
        const QString reason = resolution.problem;
        if (state_ == CoreState::Running) reportFailure(CoreFailure::BinaryNotFound, reason);
        else fail(CoreFailure::BinaryNotFound, reason);
        return;
    }
    if (binaryPath_.isEmpty()) binaryPath_ = resolution.path;
    resolvedEngine_ = resolution;
    // Reported on its own signal, NOT on logLine: logLine is the core's own
    // output, it refreshes the readiness deadline and it feeds the failure tail.
    // MihomoBackend republishes this on the observer's log channel.
    emit engineResolved(resolution.path, resolution.label, resolution.provenance);
    // The old core stays live while its replacement is validated. Validation
    // may download geo data, so it cannot run synchronously on the UI thread.
    validatingConfigPath_ = configPath;
    validatingWorkDir_ = workDir;
    validatingBinary_ = resolution.path;
    auto *validation = new QProcess(this);
    validation_ = validation;
    validation->setProcessChannelMode(QProcess::MergedChannels);
    connect(validation, &QProcess::finished, this,
            [this, validation](int exitCode, QProcess::ExitStatus status) {
        if (validation_ != validation) return;
        const QString output = QString::fromUtf8(validation->readAll()).trimmed();
        finishValidation(status == QProcess::NormalExit && exitCode == 0,
                         output.isEmpty() ? tr("Validator exited with code %1.").arg(exitCode) : output);
    });
    connect(validation, &QProcess::errorOccurred, this,
            [this, validation](QProcess::ProcessError error) {
        if (validation_ == validation && error == QProcess::FailedToStart)
            finishValidation(false, validation->errorString());
    });
    const int validationTimeoutMs = timings_.validationTimeoutMs;
    QTimer::singleShot(validationTimeoutMs, validation, [this, validation, validationTimeoutMs] {
        if (validation_ == validation)
            finishValidation(false, tr("Validation timed out after %1 seconds.")
                                        .arg(validationTimeoutMs / 1000));
    });
    if (state_ != CoreState::Running && !retiring_) setState(CoreState::Starting);
    if (validation_ != validation) return;
    validation->start(validatingBinary_, {"-t", "-d", workDir, "-f", configPath});
}

void CoreProcess::cancelValidation() {
    if (!validation_) return;
    QProcess *validation = validation_;
    validation_ = nullptr;
    validation->disconnect(this);
    if (validation->state() != QProcess::NotRunning) {
        cancelingValidations_.insert(validation, validatingConfigPath_);
        const auto retired = [this, validation] {
            if (!cancelingValidations_.remove(validation)) return;
            validation->deleteLater();
            if (!process_ && !retiring_ && !validation_ && !serviceActive_) finishStop();
        };
        connect(validation, &QProcess::finished, this, retired);
        connect(validation, &QProcess::errorOccurred, this, [retired](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) retired();
        });
        validation->kill();
    } else {
        validation->deleteLater();
    }
}

void CoreProcess::finishValidation(bool valid, const QString &reason) {
    const QString configPath = validatingConfigPath_;
    const QString workDir = validatingWorkDir_;
    const QString binary = validatingBinary_;
    cancelValidation();
    if (!valid) {
        const QString message = tr("Core configuration validation failed: %1").arg(reason);
        if (state_ == CoreState::Running) reportFailure(CoreFailure::ValidationFailed, message);
        else fail(CoreFailure::ValidationFailed, message);
        return;
    }
    if (useService_) {
        const quint64 generation = launchGeneration_;
        serviceParsing_ = true;
        using Parsed = QPair<QJsonObject, QString>;
        auto *watcher = new QFutureWatcher<Parsed>(this);
        connect(watcher, &QFutureWatcher<Parsed>::finished, this, [this, watcher, generation, configPath, workDir, binary] {
            const Parsed result = watcher->result();
            watcher->deleteLater();
            if (generation != launchGeneration_) return;
            serviceParsing_ = false;
            if (!result.second.isEmpty()) {
                if (state_ == CoreState::Running) reportFailure(CoreFailure::ConfigUnreadable, result.second);
                else fail(CoreFailure::ConfigUnreadable, result.second);
                return;
            }
            serviceConfig_ = result.first;
            launchValidated(configPath, workDir, binary);
        });
        watcher->setFuture(QtConcurrent::run([configPath]() -> Parsed {
            QFile file(configPath);
            if (!file.open(QIODevice::ReadOnly)) return {{}, file.errorString()};
            const QByteArray bytes = file.read(8 * 1024 * 1024 + 1);
            if (bytes.size() > 8 * 1024 * 1024) return {{}, CoreProcess::tr("Service configuration exceeds the 8 MiB limit.")};
            try {
                const YAML::Node config = YAML::Load(bytes.toStdString());
                if (!config.IsMap()) return {{}, CoreProcess::tr("Service configuration must be a YAML mapping.")};
                return {serviceJson(config).toObject(), {}};
            } catch (const YAML::Exception &error) { return {{}, QString::fromStdString(error.what())}; }
        }));
        return;
    }
    launchValidated(configPath, workDir, binary);
}

void CoreProcess::launchValidated(const QString &configPath, const QString &workDir,
                                  const QString &binary) {
    pendingLaunch_ = LaunchRequest{configPath, workDir, binary};
    stopResult_ = CoreState::Stopped;
    stopError_.clear();
    if (readyTimer_) readyTimer_->stop();
    cancelProbe();
    terminateProcess();
}

void CoreProcess::launchProcess(const QString &configPath, const QString &workDir,
                                const QString &binary) {
    configPath_ = configPath;
    workDir_ = workDir;

    endpoint_ = noEndpoint();
    logBuffer_.clear();
    logTail_.clear();
    if (useService_) {
        serviceLogTail_.clear();
        serviceActive_ = true;
        serviceStopping_ = false;
        setState(CoreState::Starting);
        if (serviceActive_ && !serviceStopping_) service_->startCore(serviceConfig_);
        return;
    }

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyReadStandardOutput, this, &CoreProcess::drainOutput);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        fail(CoreFailure::LaunchFailed,
             tr("Cannot launch %1: %2").arg(binaryPath_, process_->errorString()));
    });
    connect(process_, &QProcess::finished, this,
            [this](int exitCode) { handleFinished(exitCode); });

    QProcess *child = process_;
    setState(CoreState::Starting);
    if (process_ != child) return;
    child->start(binary, {"-d", workDir_, "-f", configPath_});
    // QProcess reports a failure to start from inside start().
    if (state_ == CoreState::Starting) awaitController();
}

void CoreProcess::stop() {
    explicitStop_ = true;
    ++launchGeneration_;
    serviceParsing_ = false;
    cancelValidation();
    pendingLaunch_.reset();
    stopResult_ = CoreState::Stopped;
    stopError_.clear();
    if (readyTimer_) readyTimer_->stop();
    cancelProbe();
    endpoint_ = noEndpoint();
    terminateProcess();
}

void CoreProcess::terminateProcess() {
    if (serviceActive_) {
        if (!serviceStopping_) {
            serviceStopping_ = true;
            servicePoll_->stop();
            setState(CoreState::Stopping);
            service_->stopCore();
        }
        return;
    }
    if (retiring_) {
        setState(CoreState::Stopping);
        return;
    }
    if (!process_ || process_->state() == QProcess::NotRunning) {
        discardProcess();
        finishStop();
        return;
    }
    drainOutput();
    QProcess *child = process_;
    process_ = nullptr;
    retiring_ = child;
    child->disconnect(this);
    connect(child, &QProcess::finished, this, [this, child] {
        if (retiring_ != child) return;
        retiring_ = nullptr;
        child->deleteLater();
        finishStop();
    });
    QTimer::singleShot(timings_.terminateWaitMs, child, [this, child] {
        if (retiring_ == child && child->state() != QProcess::NotRunning) child->kill();
    });
    setState(CoreState::Stopping);
    child->terminate();
}

void CoreProcess::finishStop() {
    endpoint_ = noEndpoint();
    if (pendingLaunch_) {
        const LaunchRequest request = *pendingLaunch_;
        pendingLaunch_.reset();
        launchProcess(request.configPath, request.workDir, request.binary);
        return;
    }
    if (validation_ || serviceParsing_) {
        setState(CoreState::Starting);
        return;
    }
    if (!cancelingValidations_.isEmpty()) {
        setState(CoreState::Stopping);
        return;
    }
    const CoreState result = stopResult_;
    const QString reason = stopError_;
    stopError_.clear();
    setState(result);
    if (result == CoreState::Failed) {
        emit failed(reason);
        if (explicitStop_) {
            explicitStop_ = false;
            emit stopFinished(false, reason);
        }
    } else {
        explicitStop_ = false;
        emit stopped();
        emit stopFinished(true, {});
    }
}

void CoreProcess::restart() {
    if (configPath_.isEmpty()) return;
    const QString configPath = configPath_;
    const QString workDir = workDir_;
    start(configPath, workDir);
}

CoreState CoreProcess::state() const { return state_; }
bool CoreProcess::isRestartPending() const { return pendingLaunch_.has_value() || validation_ != nullptr || serviceParsing_; }
Endpoint CoreProcess::endpoint() const { return endpoint_; }
QStringList CoreProcess::activeConfigPaths() const {
    QStringList paths;
    if (process_ || retiring_ || serviceActive_) paths.append(configPath_);
    if (validation_ || serviceParsing_) paths.append(validatingConfigPath_);
    if (pendingLaunch_) paths.append(pendingLaunch_->configPath);
    for (const QString &path : cancelingValidations_) paths.append(path);
    paths.removeDuplicates();
    return paths;
}

void CoreProcess::awaitController(const std::optional<Endpoint> &serviceEndpoint) {
    const std::optional<Endpoint> parsed = serviceEndpoint ? serviceEndpoint : endpointFromConfigFile(configPath_);
    if (!parsed) {
        fail(CoreFailure::ConfigUnreadable,
             tr("No usable external-controller in %1.").arg(configPath_));
        return;
    }

    pendingEndpoint_ = *parsed;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    readyDeadlineMs_ = now + (serviceActive_ ? timings_.serviceIdleDeadlineMs : timings_.idleDeadlineMs);
    readyHardDeadlineMs_ = now + timings_.hardCapMs;
    if (!readyTimer_) {
        readyTimer_ = new QTimer(this);
        connect(readyTimer_, &QTimer::timeout, this, &CoreProcess::probeController);
    }
    readyTimer_->setInterval(timings_.probeIntervalMs);
    readyTimer_->start();
}

void CoreProcess::probeController() {
    if (state_ != CoreState::Starting) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now > readyDeadlineMs_ || now > readyHardDeadlineMs_) {
        fail(CoreFailure::ReadyTimeout,
             tr("Core did not answer on %1 and went quiet for %2 seconds.\n%3")
                 .arg(pendingEndpoint_.httpBase())
                 .arg((serviceActive_ ? timings_.serviceIdleDeadlineMs : timings_.idleDeadlineMs) / 1000)
                 .arg(logTail_.join('\n')));
        return;
    }

    if (probeReply_) return;
    QNetworkRequest request{QUrl(pendingEndpoint_.httpBase() + "/version")};
    request.setTransferTimeout(timings_.probeTimeoutMs);
    if (!pendingEndpoint_.secret.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + pendingEndpoint_.secret.toUtf8());
    }

    probeReply_ = network_->get(request);
    QNetworkReply *reply = probeReply_;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (probeReply_ != reply) {
            // Contract section 5.2. cancelProbe() disconnects BEFORE it aborts,
            // so a cancelled probe can never reach this handler at all. Getting
            // here means that ordering was inverted and abort() delivered
            // finished() synchronously into a handler that was being torn down.
            // Counted rather than merely swallowed: an unobservable rule is one
            // no test can hold the implementation to.
            ++probeCompletionsAfterCancel_;
            return;
        }
        probeReply_ = nullptr;
        reply->deleteLater();
        // The core may have died or timed out while this probe was in flight.
        if (state_ != CoreState::Starting) return;
        if (reply->error() != QNetworkReply::NoError ||
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject() || !document.object().value("version").isString()) return;

        readyTimer_->stop();
        endpoint_ = pendingEndpoint_;
        setState(CoreState::Running);
        emit ready(endpoint_);
    });
}

void CoreProcess::cancelProbe() {
    if (!probeReply_) return;
    QNetworkReply *reply = probeReply_;
    probeReply_ = nullptr;
    // Contract section 5.2, and the order of these two statements IS the rule:
    // QNetworkReply::abort() emits finished() synchronously, so aborting first
    // would deliver a completion into a handler this call is tearing down.
    // probeCompletionsAfterCancel() is what makes the violation observable.
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

int CoreProcess::probeCompletionsAfterCancel() const { return probeCompletionsAfterCancel_; }

void CoreProcess::drainOutput() {
    if (!process_) return;
    logBuffer_ += QString::fromUtf8(process_->readAllStandardOutput());
    // mihomo flushes mid-line, so a partial tail is held back until its newline arrives.
    for (qsizetype newline = logBuffer_.indexOf('\n'); newline >= 0;
         newline = logBuffer_.indexOf('\n')) {
        publishLine(logBuffer_.left(newline));
        logBuffer_.remove(0, newline + 1);
    }
}

void CoreProcess::publishLine(QString line) {
    if (line.endsWith('\r')) line.chop(1);
    if (line.isEmpty()) return;

    logTail_.append(line);
    if (logTail_.size() > kLogTailLines) logTail_.removeFirst();

    if (state_ == CoreState::Starting) {
        readyDeadlineMs_ = QDateTime::currentMSecsSinceEpoch() +
                           (serviceActive_ ? timings_.serviceIdleDeadlineMs : timings_.idleDeadlineMs);
    }

    emit logLine(line);
}

void CoreProcess::handleFinished(int exitCode) {
    drainOutput();
    publishLine(logBuffer_);
    logBuffer_.clear();

    fail(CoreFailure::CoreExited,
         tr("Core exited with code %1.\n%2").arg(exitCode).arg(logTail_.join('\n')));
}

void CoreProcess::discardProcess() {
    if (!process_) return;
    process_->disconnect(this);
    process_->deleteLater();
    process_ = nullptr;
}

void CoreProcess::setState(CoreState state) {
    if (state_ == state) return;
    state_ = state;
    emit stateChanged(state_);
}

void CoreProcess::reportFailure(CoreFailure kind, const QString &reason) {
    lastFailure_ = kind;
    emit failed(reason);
}

void CoreProcess::fail(CoreFailure kind, const QString &reason) {
    lastFailure_ = kind;
    fail(reason);
}

void CoreProcess::fail(const QString &reason) {
    ++launchGeneration_;
    serviceParsing_ = false;
    cancelValidation();
    pendingLaunch_.reset();
    if (readyTimer_) readyTimer_->stop();
    cancelProbe();
    endpoint_ = noEndpoint();
    stopResult_ = CoreState::Failed;
    stopError_ = reason;
    terminateProcess();
}

}  // namespace core
