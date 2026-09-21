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
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "core/mihomo/controller_discovery.h"
#include "core/config/yaml_util.h"
#include "platform/service/privileged_service_client.h"

namespace core {
namespace {

constexpr int kProbeIntervalMs = 200;
// A core that is still logging is still working: a first run downloading geo
// databases takes far longer than a fixed budget, so the deadline tracks
// silence rather than elapsed time, bounded so a wedged core still fails.
constexpr int kReadyIdleMs = 10000;
constexpr int kServiceReadyIdleMs = 60000;
constexpr int kReadyHardCapMs = 180000;
constexpr int kTerminateWaitMs = 3000;
constexpr int kLogTailLines = 8;

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

QStringList bundledBinaries() {
#ifdef Q_OS_MACOS
    return {
        "/Applications/Clash Verge.app/Contents/MacOS/verge-mihomo",
        "/Applications/Clash Verge.app/Contents/MacOS/verge-mihomo-alpha",
    };
#elif defined(Q_OS_WIN)
    QStringList paths;
    for (const char *variable : {"LOCALAPPDATA", "ProgramFiles", "ProgramFiles(x86)"}) {
        const QString root = qEnvironmentVariable(variable);
        if (root.isEmpty()) continue;
        paths << root + "/Programs/Clash Verge/verge-mihomo.exe"
              << root + "/Programs/Clash Verge/verge-mihomo-alpha.exe"
              << root + "/Clash Verge/verge-mihomo.exe"
              << root + "/Clash Verge/verge-mihomo-alpha.exe";
    }
    return paths;
#else
    return {
        "/usr/lib/clash-verge-rev/verge-mihomo",
        "/usr/lib/clash-verge-rev/verge-mihomo-alpha",
        "/opt/clash-verge-rev/verge-mihomo",
        "/opt/clash-verge-rev/verge-mihomo-alpha",
    };
#endif
}

}  // namespace

CoreProcess::CoreProcess(QObject *parent, platform::PrivilegedServiceClient *serviceClient)
    : QObject(parent), serviceClient_(serviceClient ? serviceClient : new platform::PrivilegedServiceClient(this)),
      servicePoll_(new QTimer(this)), injectedService_(serviceClient != nullptr),
      network_(new QNetworkAccessManager(this)) {
    servicePoll_->setInterval(1000);
    connect(servicePoll_, &QTimer::timeout, this, [this] {
        if (!serviceActive_ || serviceStopping_ || serviceClient_->isBusy()) return;
        serviceClient_->requestLogs();
        serviceClient_->requestStatus();
    });
    connect(serviceClient_, &platform::PrivilegedServiceClient::coreStarted, this, [this](const QJsonObject &endpoint) {
        if (!serviceActive_ || serviceStopping_) return;
        Endpoint controller;
        controller.host = endpoint.value("host").toString();
        controller.port = endpoint.value("port").toInt();
        controller.secret = endpoint.value("secret").toString();
        if (!controller.isValid()) { fail(tr("The privileged service returned an invalid controller endpoint.")); return; }
        servicePoll_->start();
        awaitController(controller);
    });
    connect(serviceClient_, &platform::PrivilegedServiceClient::coreStopped, this, [this] {
        if (!serviceActive_) return;
        servicePoll_->stop();
        serviceActive_ = false;
        serviceStopping_ = false;
        finishStop();
    });
    connect(serviceClient_, &platform::PrivilegedServiceClient::logsReceived, this, [this](const QString &lines) {
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
    });
    connect(serviceClient_, &platform::PrivilegedServiceClient::statusReceived, this, [this](const QJsonObject &status) {
        if (serviceActive_ && !serviceStopping_ && (state_ == CoreState::Running || state_ == CoreState::Starting) &&
            status.value("state").toString() == "stopped") {
            const QString details = logTail_.join('\n');
            fail(details.isEmpty() ? tr("The privileged core exited unexpectedly.")
                                  : tr("The privileged core exited unexpectedly.\n%1").arg(details));
        }
    });
    connect(serviceClient_, &platform::PrivilegedServiceClient::connectedChanged, this, [this](bool connected) {
        if (!connected && serviceActive_) serviceDisconnected();
    });
    connect(serviceClient_, &platform::PrivilegedServiceClient::requestFinished, this,
            [this](const QString &operation, bool success, const QString &error) {
        if (success || !serviceActive_) return;
        if (operation == "start") {
            servicePoll_->stop();
            serviceActive_ = false;
            serviceStopping_ = false;
            if (stopResult_ == CoreState::Stopped && state_ == CoreState::Stopping) finishStop();
            else fail(tr("Privileged core start failed: %1").arg(error));
        } else if (operation == "stop") {
            servicePoll_->stop();
            serviceActive_ = false;
            serviceStopping_ = false;
            serviceClient_->close();
            fail(tr("Privileged core stop failed: %1").arg(error));
        }
    });
}

CoreProcess::~CoreProcess() {
    serviceClient_->disconnect(this);
    serviceClient_->close();
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

QString CoreProcess::discoverBinary() {
#ifdef Q_OS_WIN
    const QString bundled = QCoreApplication::applicationDirPath() + "/mihomo.exe";
#else
    const QString bundled = QCoreApplication::applicationDirPath() + "/mihomo";
#endif
    if (const QFileInfo info(bundled); info.isFile() && info.isExecutable())
        return info.absoluteFilePath();
    const QString onPath = QStandardPaths::findExecutable("mihomo");
    if (!onPath.isEmpty()) return onPath;

    for (const QString &candidate : bundledBinaries()) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) return info.absoluteFilePath();
    }
    return {};
}

void CoreProcess::setBinaryPath(const QString &path) { binaryPath_ = path; }
QString CoreProcess::binaryPath() const { return binaryPath_; }

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
bool CoreProcess::setUseService(bool enabled) {
    if (enabled && !serviceSupported() && !injectedService_) return false;
    if (state_ != CoreState::Stopped && state_ != CoreState::Failed) return false;
    if (process_ || retiring_ || validation_ || serviceActive_ || serviceParsing_ || !cancelingValidations_.isEmpty()) return false;
    useService_ = enabled;
    return true;
}
bool CoreProcess::isServiceMode() const { return useService_; }
bool CoreProcess::usesPrivilegedService() const { return serviceActive_; }

void CoreProcess::serviceDisconnected() {
    const bool wasStopping = serviceStopping_;
    const QString connectionError = serviceClient_->connectionError();
    servicePoll_->stop();
    serviceActive_ = false;
    serviceStopping_ = false;
    if (wasStopping && stopResult_ == CoreState::Stopped) {
        // The helper owns the lease and terminates its core on disconnect.
        pendingLaunch_.reset();
        stopResult_ = CoreState::Failed;
        stopError_ = tr("The privileged service disconnected before confirming core shutdown.");
        if (!connectionError.isEmpty()) stopError_ += '\n' + connectionError;
        finishStop();
    } else {
        QString reason = tr("The privileged service connection was lost. Its core lease has ended.");
        if (!connectionError.isEmpty()) reason += '\n' + connectionError;
        fail(reason);
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
    if (binaryPath_.isEmpty()) binaryPath_ = discoverBinary();
    if (binaryPath_.isEmpty()) {
        const QString reason = tr("No mihomo binary found in PATH or in a local Clash Verge Rev install.");
        if (state_ == CoreState::Running) emit failed(reason);
        else fail(reason);
        return;
    }
    // The old core stays live while its replacement is validated. Validation
    // may download geo data, so it cannot run synchronously on the UI thread.
    validatingConfigPath_ = configPath;
    validatingWorkDir_ = workDir;
    validatingBinary_ = binaryPath_;
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
    QTimer::singleShot(15000, validation, [this, validation] {
        if (validation_ == validation)
            finishValidation(false, tr("Validation timed out after 15 seconds."));
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
        if (state_ == CoreState::Running) emit failed(message);
        else fail(message);
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
                if (state_ == CoreState::Running) emit failed(result.second);
                else fail(result.second);
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

    endpoint_ = Endpoint{};
    logBuffer_.clear();
    logTail_.clear();
    if (useService_) {
        serviceLogTail_.clear();
        serviceActive_ = true;
        serviceStopping_ = false;
        setState(CoreState::Starting);
        if (serviceActive_ && !serviceStopping_) serviceClient_->startCore(serviceConfig_);
        return;
    }

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyReadStandardOutput, this, &CoreProcess::drainOutput);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        fail(tr("Cannot launch %1: %2").arg(binaryPath_, process_->errorString()));
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
    endpoint_ = Endpoint{};
    terminateProcess();
}

void CoreProcess::terminateProcess() {
    if (serviceActive_) {
        if (!serviceStopping_) {
            serviceStopping_ = true;
            servicePoll_->stop();
            setState(CoreState::Stopping);
            serviceClient_->stopCore();
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
    QTimer::singleShot(kTerminateWaitMs, child, [this, child] {
        if (retiring_ == child && child->state() != QProcess::NotRunning) child->kill();
    });
    setState(CoreState::Stopping);
    child->terminate();
}

void CoreProcess::finishStop() {
    endpoint_ = Endpoint{};
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
        fail(tr("No usable external-controller in %1.").arg(configPath_));
        return;
    }

    pendingEndpoint_ = *parsed;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    readyDeadlineMs_ = now + (serviceActive_ ? kServiceReadyIdleMs : kReadyIdleMs);
    readyHardDeadlineMs_ = now + kReadyHardCapMs;
    if (!readyTimer_) {
        readyTimer_ = new QTimer(this);
        readyTimer_->setInterval(kProbeIntervalMs);
        connect(readyTimer_, &QTimer::timeout, this, &CoreProcess::probeController);
    }
    readyTimer_->start();
}

void CoreProcess::probeController() {
    if (state_ != CoreState::Starting) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now > readyDeadlineMs_ || now > readyHardDeadlineMs_) {
        fail(tr("Core did not answer on %1 and went quiet for %2 seconds.\n%3")
                 .arg(pendingEndpoint_.httpBase())
                 .arg((serviceActive_ ? kServiceReadyIdleMs : kReadyIdleMs) / 1000)
                 .arg(logTail_.join('\n')));
        return;
    }

    if (probeReply_) return;
    QNetworkRequest request{QUrl(pendingEndpoint_.httpBase() + "/version")};
    request.setTransferTimeout(2000);
    if (!pendingEndpoint_.secret.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + pendingEndpoint_.secret.toUtf8());
    }

    probeReply_ = network_->get(request);
    QNetworkReply *reply = probeReply_;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (probeReply_ != reply) return;
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
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

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
                           (serviceActive_ ? kServiceReadyIdleMs : kReadyIdleMs);
    }

    emit logLine(line);
}

void CoreProcess::handleFinished(int exitCode) {
    drainOutput();
    publishLine(logBuffer_);
    logBuffer_.clear();

    fail(tr("Core exited with code %1.\n%2").arg(exitCode).arg(logTail_.join('\n')));
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

void CoreProcess::fail(const QString &reason) {
    ++launchGeneration_;
    serviceParsing_ = false;
    cancelValidation();
    pendingLaunch_.reset();
    if (readyTimer_) readyTimer_->stop();
    cancelProbe();
    endpoint_ = Endpoint{};
    stopResult_ = CoreState::Failed;
    stopError_ = reason;
    terminateProcess();
}

}  // namespace core
