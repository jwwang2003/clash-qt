#include "core/process/core_process.h"

#include <QDateTime>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "core/controller_discovery.h"

namespace core {
namespace {

constexpr int kProbeIntervalMs = 200;
// A core that is still logging is still working: a first run downloading geo
// databases takes far longer than a fixed budget, so the deadline tracks
// silence rather than elapsed time, bounded so a wedged core still fails.
constexpr int kReadyIdleMs = 10000;
constexpr int kReadyHardCapMs = 180000;
constexpr int kTerminateWaitMs = 3000;
constexpr int kLogTailLines = 8;

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

CoreProcess::CoreProcess(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

CoreProcess::~CoreProcess() { stop(); }

QString CoreProcess::discoverBinary() {
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

void CoreProcess::start(const QString &configPath, const QString &workDir) {
    stop();
    discardProcess();

    configPath_ = configPath;
    workDir_ = workDir;

    if (binaryPath_.isEmpty()) binaryPath_ = discoverBinary();
    if (binaryPath_.isEmpty()) {
        fail(tr("No mihomo binary found in PATH or in a local Clash Verge Rev install."));
        return;
    }

    endpoint_ = Endpoint{};
    logBuffer_.clear();
    logTail_.clear();
    stopRequested_ = false;

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyReadStandardOutput, this, &CoreProcess::drainOutput);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        fail(tr("Cannot launch %1: %2").arg(binaryPath_, process_->errorString()));
    });
    connect(process_, &QProcess::finished, this,
            [this](int exitCode) { handleFinished(exitCode); });

    setState(CoreState::Starting);
    process_->start(binaryPath_, {"-d", workDir_, "-f", configPath_});
    // QProcess reports a failure to start from inside start().
    if (state_ == CoreState::Starting) awaitController();
}

void CoreProcess::stop() {
    if (readyTimer_) readyTimer_->stop();
    if (!process_ || process_->state() == QProcess::NotRunning) return;

    terminateProcess();
    setState(CoreState::Stopped);
}

void CoreProcess::terminateProcess() {
    if (!process_) return;

    if (process_->state() != QProcess::NotRunning) {
        stopRequested_ = true;
        process_->terminate();
        if (!process_->waitForFinished(kTerminateWaitMs)) {
            process_->kill();
            process_->waitForFinished(kTerminateWaitMs);
        }
    }
    discardProcess();
}

void CoreProcess::restart() {
    if (configPath_.isEmpty()) return;
    const QString configPath = configPath_;
    const QString workDir = workDir_;
    stop();
    start(configPath, workDir);
}

CoreState CoreProcess::state() const { return state_; }
Endpoint CoreProcess::endpoint() const { return endpoint_; }

void CoreProcess::awaitController() {
    const std::optional<Endpoint> parsed = endpointFromConfigFile(configPath_);
    if (!parsed) {
        fail(tr("No usable external-controller in %1.").arg(configPath_));
        return;
    }

    pendingEndpoint_ = *parsed;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    readyDeadlineMs_ = now + kReadyIdleMs;
    readyHardDeadlineMs_ = now + kReadyHardCapMs;
    if (!readyTimer_) {
        readyTimer_ = new QTimer(this);
        readyTimer_->setInterval(kProbeIntervalMs);
        connect(readyTimer_, &QTimer::timeout, this, &CoreProcess::probeController);
    }
    readyTimer_->start();
}

void CoreProcess::probeController() {
    if (probeReply_) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now > readyDeadlineMs_ || now > readyHardDeadlineMs_) {
        fail(tr("Core did not answer on %1 and went quiet for %2 seconds.\n%3")
                 .arg(pendingEndpoint_.httpBase())
                 .arg(kReadyIdleMs / 1000)
                 .arg(logTail_.join('\n')));
        return;
    }

    QNetworkRequest request{QUrl(pendingEndpoint_.httpBase() + "/version")};
    if (!pendingEndpoint_.secret.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + pendingEndpoint_.secret.toUtf8());
    }

    probeReply_ = network_->get(request);
    connect(probeReply_, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = probeReply_;
        probeReply_ = nullptr;
        reply->deleteLater();
        // The core may have died or timed out while this probe was in flight.
        if (state_ != CoreState::Starting) return;
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;

        readyTimer_->stop();
        endpoint_ = pendingEndpoint_;
        setState(CoreState::Running);
        emit ready(endpoint_);
    });
}

void CoreProcess::drainOutput() {
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
        readyDeadlineMs_ = QDateTime::currentMSecsSinceEpoch() + kReadyIdleMs;
    }

    emit logLine(line);
}

void CoreProcess::handleFinished(int exitCode) {
    drainOutput();
    publishLine(logBuffer_);
    logBuffer_.clear();
    if (stopRequested_) return;

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
    if (readyTimer_) readyTimer_->stop();
    // A core we cannot reach must not keep running behind a Failed state.
    terminateProcess();
    setState(CoreState::Failed);
    emit failed(reason);
}

}  // namespace core
