#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QPointer>

#include "core/mihomo/mihomo_client.h"
#include "core/mihomo/provider_client.h"

class ProviderServer : public QTcpServer {
public:
    ProviderServer() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                    socket->setProperty("handled", true);
                    requests.append(request);
                    const auto path = request.split(' ').value(1);
                    const auto body = responses.value(path, "{}");
                    const auto status = statuses.value(path, "200 OK");
                    const QPointer<QTcpSocket> guarded(socket);
                    QTimer::singleShot(delays.value(path), this, [guarded, status, body] {
                        if (!guarded || guarded->state() != QAbstractSocket::ConnectedState) return;
                        guarded->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                       QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        guarded->disconnectFromHost();
                    });
                });
            }
        });
    }
    QHash<QByteArray, QByteArray> responses;
    QHash<QByteArray, QByteArray> statuses;
    QHash<QByteArray, int> delays;
    QList<QByteArray> requests;
};

class ProviderTest : public QObject {
    Q_OBJECT
private slots:
    void listsCountsAndSubscription() {
        ProviderServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/providers/proxies"] = R"({"providers":{"subscription":{"type":"Proxy","vehicleType":"HTTP","proxies":[{},{}],"updatedAt":"2026-01-02T03:04:05Z","subscriptionInfo":{"Upload":12,"Download":30,"Total":100,"Expire":1800000000}},"implicit":{"vehicleType":"Compatible"}}})";
        server.responses["/providers/rules"] = R"({"providers":{"sites":{"type":"Rule","vehicleType":"HTTP","behavior":"Domain","ruleCount":123}}})";
        core::MihomoClient client;
        client.setEndpoint({"127.0.0.1", server.serverPort(), "test-secret"});
        core::ProviderClient providers(&client);
        QVector<core::Provider> result;
        int received = 0;
        bool rules = false;
        connect(&providers, &core::ProviderClient::providersReceived, this,
                [&](bool isRules, const auto &items) { result = items; rules = isRules; ++received; });
        providers.fetch(false);
        QTRY_COMPARE(received, 1);
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.first().name, QString("subscription"));
        QCOMPARE(result.first().count, 2);
        QCOMPARE(result.first().used, quint64(42));
        QCOMPARE(result.first().total, quint64(100));
        QVERIFY(result.first().updated.isValid());
        QVERIFY(result.first().expires.isValid());
        QVERIFY(!rules);
        providers.fetch(true);
        QTRY_COMPARE(received, 2);
        QVERIFY(rules);
        QCOMPARE(result.first().count, 123);
        QCOMPARE(result.first().behavior, QString("Domain"));
        for (const auto &request : server.requests)
            QVERIFY(request.contains("Authorization: Bearer test-secret"));
    }

    void mutationEncodingAndFailures() {
        ProviderServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/providers/proxies"] = R"({"providers":{}})";
        core::MihomoClient client;
        client.setEndpoint({"127.0.0.1", server.serverPort(), {}});
        core::ProviderClient providers(&client);
        QSignalSpy completed(&providers, &core::ProviderClient::operationFinished);
        QSignalSpy errors(&providers, &core::ProviderClient::errorOccurred);
        providers.update(false, "a/b c");
        QTRY_COMPARE(completed.size(), 1);
        bool encodedPut = false;
        for (const auto &request : server.requests)
            encodedPut |= request.startsWith("PUT /providers/proxies/a%2Fb%20c HTTP/");
        QVERIFY(encodedPut);
        providers.healthCheck("a/b c");
        QTRY_COMPARE(completed.size(), 2);
        bool healthGet = false;
        for (const auto &request : server.requests)
            healthGet |= request.startsWith("GET /providers/proxies/a%2Fb%20c/healthcheck HTTP/");
        QVERIFY(healthGet);
        server.statuses["/providers/proxies/bad"] = "500 Internal Server Error";
        providers.update(false, "bad");
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(completed.size(), 2);
        server.responses["/providers/rules"] = "invalid json";
        providers.fetch(true);
        QTRY_COMPARE(errors.size(), 2);
    }

    // The address is not the session.
    //
    // ProviderClient used to learn of an invalidation from
    // MihomoClient::endpointChanged, which a replacement at an unchanged
    // address deliberately does not emit, and its reply guard then compared
    // the ADDRESS - which a replacement preserves exactly. So its epoch never
    // moved across a replacement: the retired process's reply was accepted as
    // live data, and a fetch submitted afterwards was handed the retired
    // request's key and issued nothing at all.
    void aReplacementAtTheSameAddressRetiresProviderWork() {
        ProviderServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/providers/proxies"] =
            R"({"providers":{"obsolete":{"vehicleType":"HTTP"}}})";
        server.delays["/providers/proxies"] = 250;
        core::MihomoClient client;
        const core::Endpoint endpoint{"127.0.0.1", server.serverPort(), "secret"};
        client.setEndpoint(endpoint);
        core::ProviderClient providers(&client);
        QSignalSpy settled(&providers, &core::ProviderClient::requestSettled);
        QSignalSpy errors(&providers, &core::ProviderClient::errorOccurred);
        QStringList received;
        connect(&providers, &core::ProviderClient::providersReceived, this,
                [&](bool, const auto &items) { if (!items.isEmpty()) received << items.first().name; });

        const auto issued = [&] {
            int count = 0;
            for (const QByteArray &request : server.requests)
                if (request.startsWith("GET /providers/proxies HTTP/")) ++count;
            return count;
        };
        const QString retired = providers.fetch(false);
        QVERIFY(!retired.isEmpty());
        QTRY_COMPARE(issued(), 1);

        // The engine behind the address is replaced: byte-identical endpoint,
        // a different process.
        client.setEndpoint(endpoint);
        server.responses["/providers/proxies"] = R"({"providers":{"fresh":{"vehicleType":"HTTP"}}})";
        server.delays["/providers/proxies"] = 0;

        const QString fresh = providers.fetch(false);
        QVERIFY2(fresh != retired,
                 "a fetch submitted after the replacement was handed the key of the request "
                 "the RETIRED process owed");
        QTRY_COMPARE(issued(), 2);
        QTRY_COMPARE(received, QStringList{"fresh"});

        // The retired reply is still on its way, and belongs to nobody.
        QTest::qWait(300);
        QCOMPARE(received, QStringList{"fresh"});
        QCOMPARE(errors.size(), 0);
        bool sawRetired = false;
        for (const auto &row : settled) {
            if (row.at(0).toString() != retired) continue;
            sawRetired = true;
            QVERIFY2(row.at(1).toBool(), "the retired provider reply settled as live work");
            QVERIFY(row.at(2).toString().isEmpty());
        }
        QVERIFY2(sawRetired, "the retired provider request never settled at all");
    }

    void staleResponseIsIgnoredWhenReturningToSameEndpoint() {
        ProviderServer first, second;
        QVERIFY(first.listen(QHostAddress::LocalHost));
        QVERIFY(second.listen(QHostAddress::LocalHost));
        first.responses["/providers/proxies"] = R"({"providers":{"obsolete":{"vehicleType":"HTTP"}}})";
        first.delays["/providers/proxies"] = 250;
        second.responses["/providers/proxies"] = R"({"providers":{"other":{"vehicleType":"HTTP"}}})";
        second.delays["/providers/proxies"] = 250;
        core::MihomoClient client;
        const core::Endpoint endpointA{"127.0.0.1", first.serverPort(), "first"};
        const core::Endpoint endpointB{"127.0.0.1", second.serverPort(), "second"};
        client.setEndpoint(endpointA);
        core::ProviderClient providers(&client);
        QSignalSpy errors(&providers, &core::ProviderClient::errorOccurred);
        QStringList received;
        connect(&providers, &core::ProviderClient::providersReceived, this,
                [&](bool, const auto &items) { if (!items.isEmpty()) received << items.first().name; });
        providers.fetch(false);
        const auto gotRequest = [](const ProviderServer &server) {
            return std::any_of(server.requests.begin(), server.requests.end(), [](const QByteArray &request) {
                return request.startsWith("GET /providers/proxies HTTP/");
            });
        };
        QTRY_VERIFY(gotRequest(first));
        client.setEndpoint(endpointB);
        providers.fetch(false);
        QTRY_VERIFY(gotRequest(second));
        first.responses["/providers/proxies"] = R"({"providers":{"fresh":{"vehicleType":"HTTP"}}})";
        first.delays["/providers/proxies"] = 0;
        client.setEndpoint(endpointA);
        providers.fetch(false);
        QTRY_COMPARE(received, QStringList{"fresh"});
        QTest::qWait(300);
        QCOMPARE(received, QStringList{"fresh"});
        QCOMPARE(errors.size(), 0);
    }
};

QTEST_GUILESS_MAIN(ProviderTest)
#include "provider_test.moc"
