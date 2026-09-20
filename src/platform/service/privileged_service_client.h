#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QString>

class QLocalSocket;
class QTimer;

namespace platform {

/// A single asynchronous connection is the helper's lease on a privileged core.
/// Dropping this connection makes the helper stop that core.
class PrivilegedServiceClient : public QObject {
    Q_OBJECT
public:
    explicit PrivilegedServiceClient(QObject *parent = nullptr, const QString &socketPath = {});
    static QString defaultSocketPath();
    static bool isSupported();
    bool isConnected() const;
    bool isBusy() const;

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
    QTimer *deadline_;
    QString socketPath_;
    QByteArray input_;
    QQueue<Request> queue_;
    Request active_{};
    quint64 nextId_ = 0;
    bool hasActive_ = false;
    bool reportedConnected_ = false;
    bool closing_ = false;
};
} // namespace platform
