#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QString>

class QLocalSocket;

namespace platform {

/// The deadline a privileged-service operation runs against: the connection
/// attempt first, then each sent request. Production uses a single-shot QTimer;
/// a caller that must decide when an operation expires - a test - injects its
/// own instead of reaching into the client's QObject children.
class RequestDeadline {
public:
    virtual ~RequestDeadline() = default;
    /// Set once by the client. The implementation calls it when a started
    /// deadline elapses, on the client's thread, unless stop() came first.
    virtual void setExpiredHandler(std::function<void()> handler) = 0;
    /// Starts or restarts the single pending deadline.
    virtual void start(std::chrono::milliseconds timeout) = 0;
    virtual void stop() = 0;
};

/// A single asynchronous connection is the helper's lease on a privileged core.
/// Dropping this connection makes the helper stop that core.
class PrivilegedServiceClient : public QObject {
    Q_OBJECT
public:
    /// `deadline` must outlive this client; a null one means the real timer,
    /// which is what production passes.
    explicit PrivilegedServiceClient(QObject *parent = nullptr, const QString &socketPath = {},
                                     RequestDeadline *deadline = nullptr);
    ~PrivilegedServiceClient() override;
    static QString defaultSocketPath();
    static bool isSupported();
    bool isConnected() const;
    bool isBusy() const;
    /// Available before connectedChanged(false); reset on a new connection attempt.
    QString connectionError() const;

public slots:
    void connectToService();
    void requestStatus();
    void startCore(const QJsonObject &config);
    void stopCore();
    void requestLogs();
    void close();

signals:
    void connectedChanged(bool connected);
    void statusReceived(const QJsonObject &status);
    void coreStarted(const QJsonObject &endpoint);
    void coreStopped();
    void logsReceived(const QString &logs);
    void errorOccurred(const QString &error);
    void requestFinished(const QString &operation, bool success, const QString &error);

private:
    struct Request { quint64 id; QString operation; QJsonObject payload; };
    void enqueue(const QString &operation, const QJsonObject &payload = {});
    void sendNext();
    void readFrames();
    void failConnection(const QString &error);
    void failRequests(const QString &error);

    QLocalSocket *socket_;
    std::unique_ptr<RequestDeadline> ownedDeadline_;
    RequestDeadline *deadline_;
    QString socketPath_;
    QString connectionError_;
    QByteArray input_;
    QQueue<Request> queue_;
    Request active_{};
    quint64 nextId_ = 0;
    bool hasActive_ = false;
    bool reportedConnected_ = false;
    bool closing_ = false;
};
} // namespace platform
