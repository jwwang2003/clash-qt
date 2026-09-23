#include <QtTest>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <functional>
#include "platform/service/privileged_service_client.h"

/// A deadline the test decides the moment of: it replaces searching the client
/// for its QTimer and shortening it, so nothing here depends on wall-clock time.
class ManualDeadline : public platform::RequestDeadline {
public:
    void setExpiredHandler(std::function<void()> handler) override { expired_ = std::move(handler); }
    void start(std::chrono::milliseconds) override { pending_ = true; }
    void stop() override { pending_ = false; }
    bool expire() {
        if (!pending_ || !expired_) return false;
        pending_ = false;
        expired_();
        return true;
    }

private:
    std::function<void()> expired_;
    bool pending_ = false;
};

class FakePrivilegedService : public QLocalServer {
public:
    QList<QJsonObject> requests;
    QLocalSocket *peer = nullptr;
    QByteArray input;
    std::function<void(const QJsonObject &)> onRequest;
    FakePrivilegedService() {
        connect(this, &QLocalServer::newConnection, this, [this] {
            peer = nextPendingConnection();
            connect(peer, &QLocalSocket::disconnected, peer, &QObject::deleteLater);
            connect(peer, &QLocalSocket::readyRead, this, [this] {
                input += peer->readAll();
                while (input.size() >= 4) {
                    const quint32 length = qFromBigEndian<quint32>(input.constData());
                    if (input.size() < length + 4) return;
                    const auto request = QJsonDocument::fromJson(input.mid(4, length)).object();
                    input.remove(0, length + 4);
                    requests.append(request);
                    if (onRequest) onRequest(request);
                }
            });
        });
    }
    QByteArray frame(const QJsonObject &object) {
        auto response = object;
        if (!response.contains("protocol")) response.insert("protocol", 1);
        const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
        QByteArray output(4, Qt::Uninitialized);
        qToBigEndian<quint32>(bytes.size(), output.data());
        return output + bytes;
    }
    void respond(const QJsonObject &object, bool fragmented = false) {
        const auto bytes = frame(object);
        if (!fragmented) { peer->write(bytes); return; }
        peer->write(bytes.left(2));
        QTimer::singleShot(5, peer, [this, bytes] { peer->write(bytes.mid(2, 3)); });
        QTimer::singleShot(10, peer, [this, bytes] { peer->write(bytes.mid(5)); });
    }
};

class PrivilegedServiceClientTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory_;
    QString socketPath() const { return directory_.path() + "/service.sock"; }
private slots:
    void initTestCase() { QVERIFY(directory_.isValid()); }
    void init() { QLocalServer::removeServer(socketPath()); }
    void configStartFragmentedRepliesAndLeaseClose() {
        FakePrivilegedService server;
        QVERIFY(server.listen(socketPath()));
        server.onRequest = [&](const QJsonObject &request) {
            QJsonObject response{{"id", request.value("id")}, {"ok", true}, {"state", "running"}};
            if (request.value("command") == "start") response["endpoint"] = QJsonObject{
                {"host", "127.0.0.1"}, {"port", 29097}, {"secret", "fixture-secret"}};
            if (request.value("command") == "logs") response["logs"] = QJsonArray{"fixture log", "second line"};
            server.respond(response, true);
        };
        platform::PrivilegedServiceClient client(nullptr, socketPath());
        QSignalSpy started(&client, &platform::PrivilegedServiceClient::coreStarted);
        QSignalSpy logs(&client, &platform::PrivilegedServiceClient::logsReceived);
        QSignalSpy stopped(&client, &platform::PrivilegedServiceClient::coreStopped);
        client.startCore({{"mode", "rule"}, {"mixed-port", 7890}});
        client.requestLogs();
        QTRY_COMPARE(started.size(), 1);
        QTRY_COMPARE(logs.size(), 1);
        QCOMPARE(server.requests.first().value("config").toObject().value("mixed-port").toInt(), 7890);
        QCOMPARE(started.first().first().toJsonObject().value("port").toInt(), 29097);
        QCOMPARE(logs.first().first().toString(), QString("fixture log\nsecond line"));
        QVERIFY(client.isConnected());
        client.stopCore();
        QTRY_COMPARE(stopped.size(), 1);
        QVERIFY(client.isConnected());
        client.close();
        QVERIFY(!client.isConnected());
    }
    void statusDeduplicatesAndStopDropsQueuedStart() {
        FakePrivilegedService server;
        QVERIFY(server.listen(socketPath()));
        platform::PrivilegedServiceClient client(nullptr, socketPath());
        QSignalSpy finished(&client, &platform::PrivilegedServiceClient::requestFinished);
        client.requestStatus();
        QTRY_COMPARE(server.requests.size(), 1);
        client.requestStatus();
        client.requestStatus();
        client.startCore({{"mode", "rule"}});
        client.stopCore();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QString("start"));
        QVERIFY(!finished.first().at(1).toBool());
        server.respond({{"id", server.requests.first().value("id")}, {"ok", true}, {"state", "stopped"}});
        QTRY_COMPARE(server.requests.size(), 2);
        QCOMPARE(server.requests.last().value("command").toString(), QString("stop"));
        client.close();
    }
    void malformedLengthDisconnectsAndFailsPending() {
        FakePrivilegedService server;
        QVERIFY(server.listen(socketPath()));
        platform::PrivilegedServiceClient client(nullptr, socketPath());
        QSignalSpy finished(&client, &platform::PrivilegedServiceClient::requestFinished);
        QSignalSpy errors(&client, &platform::PrivilegedServiceClient::errorOccurred);
        client.requestStatus();
        client.startCore({});
        QTRY_COMPARE(server.requests.size(), 1);
        QByteArray header(4, Qt::Uninitialized);
        qToBigEndian<quint32>(9 * 1024 * 1024, header.data());
        server.peer->write(header);
        QTRY_VERIFY(!errors.isEmpty());
        QVERIFY(!client.isConnected());
        QCOMPARE(finished.size(), 2);
        QVERIFY(!client.isBusy());
    }
    void mismatchedResponseIdAndInvalidEndpointRejected() {
        FakePrivilegedService server;
        QVERIFY(server.listen(socketPath()));
        platform::PrivilegedServiceClient client(nullptr, socketPath());
        QSignalSpy errors(&client, &platform::PrivilegedServiceClient::errorOccurred);
        client.requestStatus();
        QTRY_COMPARE(server.requests.size(), 1);
        server.respond({{"id", 9001}, {"ok", true}});
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!client.isConnected());
        client.startCore({});
        QTRY_COMPARE(server.requests.size(), 2);
        QSignalSpy started(&client, &platform::PrivilegedServiceClient::coreStarted);
        server.respond({{"id", server.requests.last().value("id")}, {"ok", true},
            {"endpoint", QJsonObject{{"host", "0.0.0.0"}, {"port", 80}, {"secret", "x"}}}});
        QTRY_COMPARE(errors.size(), 2);
        QCOMPARE(started.size(), 0);
        QVERIFY(!client.isConnected());
    }
    void rejectionAndDisconnectNeverClaimStarted() {
        FakePrivilegedService server;
        QVERIFY(server.listen(socketPath()));
        platform::PrivilegedServiceClient client(nullptr, socketPath());
        QSignalSpy finished(&client, &platform::PrivilegedServiceClient::requestFinished);
        QSignalSpy started(&client, &platform::PrivilegedServiceClient::coreStarted);
        client.startCore({});
        QTRY_COMPARE(server.requests.size(), 1);
        server.respond({{"id", server.requests.first().value("id")}, {"ok", false}, {"error", "unsafe configuration"}});
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(2).toString(), QString("unsafe configuration"));
        QCOMPARE(started.size(), 0);
        client.requestStatus();
        QTRY_COMPARE(server.requests.size(), 2);
        server.peer->abort();
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(!client.isBusy());
    }
    void protocolMismatchClosesConnection() {
        FakePrivilegedService server;
        QVERIFY(server.listen(socketPath()));
        platform::PrivilegedServiceClient client(nullptr, socketPath());
        QSignalSpy errors(&client, &platform::PrivilegedServiceClient::errorOccurred);
        client.requestStatus();
        QTRY_COMPARE(server.requests.size(), 1);
        QCOMPARE(server.requests.first().value("protocol").toInt(), 1);
        server.respond({{"id", server.requests.first().value("id")}, {"ok", true}, {"protocol", 99}});
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains("incompatible"));
        QVERIFY(!client.isConnected());
        QVERIFY(!client.isBusy());
    }

    void invalidLogArrayIsRejected() {
        FakePrivilegedService server;
        QVERIFY(server.listen(socketPath()));
        platform::PrivilegedServiceClient client(nullptr, socketPath());
        QSignalSpy logs(&client, &platform::PrivilegedServiceClient::logsReceived);
        QSignalSpy finished(&client, &platform::PrivilegedServiceClient::requestFinished);
        client.requestLogs();
        QTRY_COMPARE(server.requests.size(), 1);
        server.respond({{"id", server.requests.first().value("id")}, {"ok", true}, {"logs", QJsonArray{"text", 42}}});
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(1).toBool());
        QCOMPARE(logs.size(), 0);
    }

    void stalledReplyTimesOutAndEndsLease() {
        FakePrivilegedService server;
        QVERIFY(server.listen(socketPath()));
        ManualDeadline deadline;
        platform::PrivilegedServiceClient client(nullptr, socketPath(), &deadline);
        QSignalSpy finished(&client, &platform::PrivilegedServiceClient::requestFinished);
        QSignalSpy errors(&client, &platform::PrivilegedServiceClient::errorOccurred);
        client.requestStatus();
        QTRY_COMPARE(server.requests.size(), 1);
        // Expire only this isolated client's pending request; production has no
        // environment/config override for the timeout or helper socket.
        QVERIFY(deadline.expire());
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(1).toBool());
        QVERIFY(finished.first().at(2).toString().contains("timed out"));
        QVERIFY(!errors.isEmpty());
        QVERIFY(!client.isConnected());
        QVERIFY(!client.isBusy());
    }

    void oversizedRequestNeverConnects() {
        platform::PrivilegedServiceClient client(nullptr, socketPath());
        QSignalSpy finished(&client, &platform::PrivilegedServiceClient::requestFinished);
        client.startCore({{"oversized", QString(9 * 1024 * 1024, 'x')}});
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(1).toBool());
        QVERIFY(!client.isBusy());
        QVERIFY(!client.isConnected());
    }
};
QTEST_GUILESS_MAIN(PrivilegedServiceClientTest)
#include "privileged_service_client_test.moc"
