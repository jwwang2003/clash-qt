#include "platform/service/privileged_service_client.h"

#include <QJsonDocument>
#include <QJsonArray>
#ifdef Q_OS_MACOS
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <QJsonParseError>
#include <QLocalSocket>
#include <QTimer>
#include <QtEndian>

namespace platform {
namespace {
constexpr quint32 kMaximumFrame = 8 * 1024 * 1024;
constexpr qsizetype kMaximumQueuedRequests = 8;
constexpr std::chrono::milliseconds kConnectTimeout{5000};
constexpr std::chrono::milliseconds kRequestTimeout{30000};

/// The production deadline: one single-shot QTimer, parented to the client so
/// it keeps that object's thread affinity and lifetime.
class TimerDeadline final : public RequestDeadline {
public:
    explicit TimerDeadline(QObject *owner) : timer_(new QTimer(owner)) {
        timer_->setSingleShot(true);
        QObject::connect(timer_, &QTimer::timeout, timer_, [this] {
            if (expired_) expired_();
        });
    }
    void setExpiredHandler(std::function<void()> handler) override { expired_ = std::move(handler); }
    void start(std::chrono::milliseconds timeout) override { timer_->start(timeout); }
    void stop() override { timer_->stop(); }

private:
    QTimer *timer_;
    std::function<void()> expired_;
};
}  // namespace

QString PrivilegedServiceClient::defaultSocketPath() {
    return QStringLiteral("/var/run/org.clash-qt.service/socket");
}

bool PrivilegedServiceClient::isSupported() {
#ifdef Q_OS_MACOS
    return true;
#else
    return false;
#endif
}

PrivilegedServiceClient::PrivilegedServiceClient(QObject *parent, const QString &socketPath,
                                                 RequestDeadline *deadline)
    : QObject(parent), socket_(new QLocalSocket(this)),
      ownedDeadline_(deadline ? nullptr : std::make_unique<TimerDeadline>(this)),
      deadline_(deadline ? deadline : ownedDeadline_.get()),
      socketPath_(socketPath.isEmpty() ? defaultSocketPath() : socketPath) {
    socket_->setReadBufferSize(kMaximumFrame + 4);
    deadline_->setExpiredHandler([this] {
        failConnection(hasActive_ ? tr("Privileged service request timed out: %1").arg(active_.operation)
                                  : tr("Connecting to the privileged service timed out."));
    });
    connect(socket_, &QLocalSocket::connected, this, [this] {
#ifdef Q_OS_MACOS
        if (socketPath_ == defaultSocketPath()) {
            uid_t uid = static_cast<uid_t>(-1);
            gid_t gid = static_cast<gid_t>(-1);
            if (getpeereid(static_cast<int>(socket_->socketDescriptor()), &uid, &gid) != 0 || uid != 0) {
                failConnection(tr("The privileged service socket is not owned by a root helper."));
                return;
            }
        }
#endif
        deadline_->stop();
        reportedConnected_ = true;
        emit connectedChanged(true);
        sendNext();
    });
    connect(socket_, &QLocalSocket::readyRead, this, &PrivilegedServiceClient::readFrames);
    connect(socket_, &QLocalSocket::disconnected, this, [this] {
        deadline_->stop();
        input_.clear();
        if (!closing_ && connectionError_.isEmpty())
            connectionError_ = tr("Privileged service disconnected; its core lease ended.");
        if (reportedConnected_) { reportedConnected_ = false; emit connectedChanged(false); }
        failRequests(closing_ ? tr("Privileged service connection closed.")
                              : tr("Privileged service disconnected; its core lease ended."));
    });
    connect(socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (!closing_) failConnection(tr("Privileged service: %1").arg(socket_->errorString()));
    });
}

PrivilegedServiceClient::~PrivilegedServiceClient() {
    // An injected deadline may outlive this client; it must not call back into it.
    deadline_->stop();
    deadline_->setExpiredHandler({});
}

bool PrivilegedServiceClient::isConnected() const { return socket_->state() == QLocalSocket::ConnectedState; }
bool PrivilegedServiceClient::isBusy() const { return hasActive_ || !queue_.isEmpty(); }
QString PrivilegedServiceClient::connectionError() const { return connectionError_; }

void PrivilegedServiceClient::connectToService() {
    if (isConnected() || socket_->state() == QLocalSocket::ConnectingState) return;
#ifndef Q_OS_MACOS
    if (socketPath_ == defaultSocketPath()) {
        emit errorOccurred(tr("The privileged core service is supported only on macOS."));
        failRequests(tr("The privileged core service is supported only on macOS."));
        return;
    }
#endif
    closing_ = false;
    connectionError_.clear();
    input_.clear();
    deadline_->start(kConnectTimeout);
    socket_->connectToServer(socketPath_);
}

void PrivilegedServiceClient::requestStatus() { enqueue("status"); }
void PrivilegedServiceClient::startCore(const QJsonObject &config) { enqueue("start", {{"config", config}}); }
void PrivilegedServiceClient::stopCore() {
    // Do not start a queued core after an explicit stop.
    QQueue<Request> preserved;
    while (!queue_.isEmpty()) {
        const Request request = queue_.dequeue();
        if (request.operation == "start")
            emit requestFinished("start", false, tr("Start cancelled by a stop request."));
        else if (request.operation != "logs" && request.operation != "status") preserved.enqueue(request);
    }
    queue_ = preserved;
    enqueue("stop");
}
void PrivilegedServiceClient::requestLogs() { enqueue("logs"); }

void PrivilegedServiceClient::enqueue(const QString &operation, const QJsonObject &payload) {
    if (operation != "start") {
        if (hasActive_ && active_.operation == operation) return;
        for (const auto &request : queue_) if (request.operation == operation) return;
    }
    const quint64 id = ++nextId_;
    QJsonObject wire = payload;
    wire.insert("id", static_cast<qint64>(id));
    wire.insert("command", operation);
    wire.insert("protocol", 1);
    if (QJsonDocument(wire).toJson(QJsonDocument::Compact).size() > kMaximumFrame) {
        emit requestFinished(operation, false, tr("Privileged service request exceeds the 8 MiB limit."));
        return;
    }
    if (queue_.size() >= kMaximumQueuedRequests) {
        emit requestFinished(operation, false, tr("The privileged service request queue is full."));
        return;
    }
    queue_.enqueue({id, operation, wire});
    if (!isConnected()) connectToService();
    else sendNext();
}

void PrivilegedServiceClient::sendNext() {
    if (!isConnected() || hasActive_ || queue_.isEmpty()) return;
    active_ = queue_.dequeue();
    hasActive_ = true;
    const QByteArray payload = QJsonDocument(active_.payload).toJson(QJsonDocument::Compact);
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
    frame.append(payload);
    deadline_->start(kRequestTimeout);
    if (socket_->write(frame) != frame.size()) failConnection(tr("Could not send the privileged service request."));
}

void PrivilegedServiceClient::readFrames() {
    input_.append(socket_->readAll());
    while (input_.size() >= 4) {
        const quint32 length = qFromBigEndian<quint32>(input_.constData());
        if (length == 0 || length > kMaximumFrame) {
            failConnection(tr("Privileged service returned an invalid frame length."));
            return;
        }
        if (input_.size() < length + 4) return;
        const QByteArray bytes = input_.mid(4, length);
        input_.remove(0, length + 4);
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        const auto response = document.object();
        if (parseError.error == QJsonParseError::NoError && document.isObject() && response.value("protocol").toDouble(-1) != 1) {
            failConnection(tr("The privileged service protocol is incompatible. Repair the service using this app version."));
            return;
        }
        if (parseError.error != QJsonParseError::NoError || !document.isObject() || !hasActive_ ||
            !response.value("id").isDouble() || response.value("id").toDouble(-1) != static_cast<double>(active_.id) ||
            !response.value("ok").isBool()) {
            failConnection(tr("Privileged service returned an invalid response."));
            return;
        }
        deadline_->stop();
        const QString operation = active_.operation;
        hasActive_ = false;
        bool ok = response.value("ok").toBool();
        QString error = response.value("error").toString();
        if (ok && operation == "start") {
            const auto endpoint = response.value("endpoint").toObject();
            if (endpoint.value("host").toString() != "127.0.0.1" ||
                !endpoint.value("port").isDouble() || endpoint.value("port").toDouble() != endpoint.value("port").toInt() ||
                endpoint.value("port").toInt() < 1024 || endpoint.value("port").toInt() > 65535 ||
                endpoint.value("secret").toString().isEmpty()) {
                failConnection(tr("Privileged service returned an invalid controller endpoint."));
                emit requestFinished(operation, false, tr("Privileged service returned an invalid controller endpoint."));
                return;
            }
            emit coreStarted(endpoint);
        } else if (ok && operation == "stop") emit coreStopped();
        else if (ok && operation == "status") emit statusReceived(response);
        else if (ok && operation == "logs") {
            const auto logValue = response.value("logs");
            QStringList lines;
            if (logValue.isArray()) {
                for (const auto &line : logValue.toArray()) {
                    if (!line.isString()) { ok = false; break; }
                    lines.append(line.toString());
                }
            } else if (logValue.isString()) lines.append(logValue.toString());
            else ok = false;
            if (ok) emit logsReceived(lines.join('\n'));
            else error = tr("Privileged service returned invalid log data.");
        }
        if (!ok && error.isEmpty()) error = tr("The privileged service rejected %1.").arg(operation);
        emit requestFinished(operation, ok, error);
        if (!ok) emit errorOccurred(error);
        sendNext();
    }
}

void PrivilegedServiceClient::failRequests(const QString &error) {
    const bool active = hasActive_;
    const Request request = active_;
    hasActive_ = false;
    const auto queued = queue_;
    queue_.clear();
    if (active) emit requestFinished(request.operation, false, error);
    for (const auto &pending : queued) emit requestFinished(pending.operation, false, error);
}

void PrivilegedServiceClient::failConnection(const QString &error) {
    if (closing_) return;
    connectionError_ = error;
    closing_ = true;
    deadline_->stop();
    failRequests(error);
    socket_->abort();
    input_.clear();
    if (reportedConnected_) { reportedConnected_ = false; emit connectedChanged(false); }
    emit errorOccurred(error);
}

void PrivilegedServiceClient::close() {
    closing_ = true;
    deadline_->stop();
    failRequests(tr("Privileged service connection closed."));
    socket_->abort();
    input_.clear();
    if (reportedConnected_) { reportedConnected_ = false; emit connectedChanged(false); }
}
} // namespace platform
