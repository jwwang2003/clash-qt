#include <QtTest>
#include <algorithm>
#include <functional>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>
#include <QTimer>
#include <QTemporaryFile>
#include "core/mihomo_client.h"
#include "core/controller_discovery.h"

class TestController : public QTcpServer {
public:
    explicit TestController(QObject *parent = nullptr) : QTcpServer(parent) {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray data = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", data);
                    const qsizetype headerEnd = data.indexOf("\r\n\r\n");
                    if (headerEnd < 0 || socket->property("handled").toBool()) return;
                    qint64 contentLength = 0;
                    for (const QByteArray &line : data.left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:"))
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    }
                    if (data.size() < headerEnd + 4 + contentLength) return;
                    socket->setProperty("handled", true);
                    const QByteArray path = data.split(' ').value(1);
                    paths.append(path);
                    requests.append(data);
                    const QByteArray methodPath = data.split(' ').value(0) + " " + path;
                    if (onRequest) onRequest(methodPath, data.mid(headerEnd + 4));
                    const QByteArray body = responses.value(methodPath, responses.value(path, "{}"));
                    const QByteArray status = statuses.value(methodPath, statuses.value(path, "200 OK"));
                    const QPointer<QTcpSocket> guarded(socket);
                    QTimer::singleShot(delays.value(methodPath, delays.value(path)), this, [guarded, body, status] {
                        if (!guarded || guarded->state() != QAbstractSocket::ConnectedState) return;
                        guarded->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                       QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        guarded->disconnectFromHost();
                    });
                });
            }
        });
        responses["/version"] = R"({"version":"test"})";
        responses["/proxies"] = R"({"proxies":{}})";
        responses["/rules"] = R"({"rules":[]})";
        responses["/configs"] = R"({"mode":"rule"})";
    }
    core::Endpoint endpoint() const { return {"127.0.0.1", serverPort(), "secret"}; }
    QHash<QByteArray, QByteArray> responses;
    QHash<QByteArray, int> delays;
    QHash<QByteArray, QByteArray> statuses;
    QList<QByteArray> paths;
    QList<QByteArray> requests;
    std::function<void(const QByteArray &, const QByteArray &)> onRequest;
};

class ControllerTest : public QObject {
    Q_OBJECT
private slots:
    void ipv6Endpoint() {
        core::Endpoint endpoint{"::1", 9090, {}};
        QCOMPARE(endpoint.httpBase(), QString("http://[::1]:9090"));
        endpoint.host = "[::1]";
        QCOMPARE(endpoint.wsBase(), QString("ws://[::1]:9090"));
        QTemporaryFile config;
        QVERIFY(config.open());
        config.write("external-controller: '[::]:9091'\nsecret: testing\n");
        config.flush();
        const auto discovered = core::endpointFromConfigFile(config.fileName());
        QVERIFY(discovered.has_value());
        QCOMPARE(discovered->httpBase(), QString("http://[::1]:9091"));
        QCOMPARE(discovered->secret, QString("testing"));
    }

    void endpointRefreshAndStaleReplyIsolation() {
        TestController oldServer, newServer;
        QVERIFY(oldServer.listen(QHostAddress::LocalHost));
        QVERIFY(newServer.listen(QHostAddress::LocalHost));
        oldServer.responses["/version"] = R"({"version":"old"})";
        oldServer.responses["/proxies"] = R"({"proxies":{"obsolete":{"type":"Selector","all":["DIRECT"]}}})";
        oldServer.delays["/version"] = 180;
        oldServer.delays["/proxies"] = 180;
        newServer.responses["/version"] = R"({"version":"new"})";
        core::MihomoClient client;
        QSignalSpy versions(&client, &core::MihomoClient::versionReceived);
        QSignalSpy errors(&client, &core::MihomoClient::errorOccurred);
        QSignalSpy changed(&client, &core::MihomoClient::endpointChanged);
        bool staleGroups = false;
        connect(&client, &core::MihomoClient::proxiesUpdated, this,
                [&](const QVector<core::ProxyGroup> &groups, const QHash<QString, core::ProxyNode> &) {
                    if (!groups.isEmpty()) staleGroups = true;
                });
        client.setEndpoint(oldServer.endpoint());
        QTRY_VERIFY(oldServer.paths.contains("/version"));
        client.setEndpoint(newServer.endpoint());
        QTRY_COMPARE(versions.size(), 1);
        QCOMPARE(versions.first().first().toString(), QString("new"));
        QVERIFY(client.isConnected());
        QTRY_VERIFY(newServer.paths.contains("/configs") && newServer.paths.contains("/rules") &&
                    newServer.paths.contains("/proxies"));
        QTest::qWait(230);
        QCOMPARE(versions.size(), 1);
        QCOMPARE(changed.size(), 2);
        QVERIFY(!staleGroups);
        QCOMPARE(errors.size(), 0);
        for (const auto &request : newServer.requests)
            QVERIFY(request.contains("Authorization: Bearer secret"));
    }

    void nestedGroupsRemainNodes() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/proxies"] = R"({"proxies":{"GLOBAL":{"type":"Selector","now":"Auto","all":["Auto"]},"Auto":{"type":"URLTest","now":"Node","all":["Node"],"history":[{"delay":73}]},"Node":{"type":"Shadowsocks","history":[{"delay":105},{"delay":42}]}}})";
        core::MihomoClient client;
        QVector<core::ProxyGroup> groups;
        QHash<QString, core::ProxyNode> nodes;
        connect(&client, &core::MihomoClient::proxiesUpdated, this,
                [&](const QVector<core::ProxyGroup> &receivedGroups, const QHash<QString, core::ProxyNode> &receivedNodes) {
                    groups = receivedGroups;
                    nodes = receivedNodes;
                });
        client.setEndpoint(server.endpoint());
        QTRY_COMPARE(groups.size(), 2);
        QCOMPARE(groups.last().name, QString("GLOBAL"));
        QVERIFY(nodes.contains("Auto"));
        QCOMPARE(nodes["Auto"].type, QString("URLTest"));
        QCOMPARE(nodes["Auto"].delay, 73);
        QCOMPARE(nodes["Node"].delay, 42);
    }

    void groupOrderFollowsGlobalMembership() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        // API object key order differs from the profile order. GLOBAL also
        // contains ordinary nodes and a duplicate group name.
        server.responses["/proxies"] = R"({"proxies":{
            "Unlisted Omega":{"type":"Selector","all":["DIRECT"]},
            "Alpha":{"type":"Selector","all":["Node"]},
            "Node":{"type":"Shadowsocks"},
            "GLOBAL":{"type":"Selector","all":["GLOBAL","DIRECT","Zulu","Node","Alpha","Zulu"]},
            "Unlisted Beta":{"type":"Selector","all":["DIRECT"]},
            "Zulu":{"type":"URLTest","all":["Node"]},
            "DIRECT":{"type":"Direct"}
        }})";
        core::MihomoClient client;
        QStringList names;
        connect(&client, &core::MihomoClient::proxiesUpdated, this,
                [&](const QVector<core::ProxyGroup> &groups, const QHash<QString, core::ProxyNode> &) {
                    names.clear();
                    for (const auto &group : groups) names.append(group.name);
                });
        client.setEndpoint(server.endpoint());
        const QStringList expected{"Zulu", "Alpha", "Unlisted Beta", "Unlisted Omega", "GLOBAL"};
        QTRY_COMPARE(names, expected);

        // Controllers without GLOBAL must still produce a deterministic order.
        server.responses["/proxies"] = R"({"proxies":{
            "Zulu":{"type":"URLTest","all":["DIRECT"]},
            "Unlisted Omega":{"type":"Selector","all":["DIRECT"]},
            "Alpha":{"type":"Selector","all":["DIRECT"]},
            "Unlisted Beta":{"type":"Selector","all":["DIRECT"]}
        }})";
        client.fetchProxies();
        const QStringList fallback{"Alpha", "Unlisted Beta", "Unlisted Omega", "Zulu"};
        QTRY_COMPARE(names, fallback);
    }

    void automaticGroupSelectionCanBePinnedAndReleased() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/proxies"] = R"({"proxies":{"Auto":{"type":"URLTest","now":"Node","fixed":"Node","all":["Node"]}}})";
        core::MihomoClient client;
        QVector<core::ProxyGroup> groups;
        connect(&client, &core::MihomoClient::proxiesUpdated, this,
                [&](const QVector<core::ProxyGroup> &snapshot, const QHash<QString, core::ProxyNode> &) {
                    groups = snapshot;
                });
        client.setEndpoint(server.endpoint());
        QTRY_COMPARE(groups.size(), 1);
        QVERIFY(groups.first().selectable());
        QCOMPARE(groups.first().fixed, QString("Node"));
        client.selectNode("Auto", "Node");
        QTRY_VERIFY(std::any_of(server.requests.cbegin(), server.requests.cend(), [](const QByteArray &request) {
            return request.startsWith("PUT /proxies/Auto ");
        }));
        client.resetGroupSelection("Auto");
        QTRY_VERIFY(std::any_of(server.requests.cbegin(), server.requests.cend(), [](const QByteArray &request) {
            return request.startsWith("DELETE /proxies/Auto ");
        }));
    }

    void configFailureRollsBackAndDisconnectClearsState() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        core::MihomoClient client;
        QSignalSpy errors(&client, &core::MihomoClient::errorOccurred);
        QSignalSpy connectionChanges(&client, &core::MihomoClient::connectedChanged);
        QSignalSpy cleared(&client, &core::MihomoClient::trafficSample);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        QTRY_VERIFY(server.paths.contains("/configs"));
        server.statuses["PATCH /configs"] = "400 Bad Request";
        const int configRequests = server.paths.count("/configs");
        client.patchConfig(QJsonObject{{"mixed-port", 9999}});
        QTRY_COMPARE(errors.size(), 1);
        QTRY_COMPARE(server.paths.count("/configs"), configRequests + 2);
        QVERIFY(client.isConnected());
        bool repopulated = false;
        connect(&client, &core::MihomoClient::proxiesUpdated, this,
                [&](const QVector<core::ProxyGroup> &groups, const QHash<QString, core::ProxyNode> &) {
                    if (!groups.isEmpty()) repopulated = true;
                });
        server.responses["/proxies"] = R"({"proxies":{"late":{"type":"Selector","all":["DIRECT"]}}})";
        server.delays["/proxies"] = 180;
        const int proxyRequests = server.paths.count("/proxies");
        client.fetchProxies();
        QTRY_COMPARE(server.paths.count("/proxies"), proxyRequests + 1);
        server.statuses["/version"] = "503 Unavailable";
        client.fetchVersion();
        QTRY_VERIFY(!client.isConnected());
        QCOMPARE(connectionChanges.last().first().toBool(), false);
        QVERIFY(!cleared.isEmpty());
        QCOMPARE(cleared.last().first().toULongLong(), quint64(0));
        QTest::qWait(230);
        QVERIFY(!repopulated);
    }

    void dnsDiagnosticsUseControllerEndpoints() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray path = "/dns/query?name=example.com&type=AAAA";
        server.responses[path] = R"({"Status":0,"Answer":[{"name":"example.com.","TTL":60,"data":"::1"}]})";
        core::MihomoClient client;
        QSignalSpy queries(&client, &core::MihomoClient::dnsQueryFinished);
        QSignalSpy flushes(&client, &core::MihomoClient::dnsCacheFlushed);
        client.setEndpoint(server.endpoint());
        client.queryDns("example.com", "AAAA");
        QTRY_COMPARE(queries.size(), 1);
        QVERIFY(server.paths.contains(path));
        QVERIFY(queries.first().at(2).toString().isEmpty());
        QCOMPARE(queries.first().at(1).value<QJsonObject>().value("Status").toInt(), 0);
        client.flushDnsCache();
        client.flushDnsCache(true);
        QTRY_COMPARE(flushes.size(), 2);
        QVERIFY(server.paths.contains("/cache/dns/flush"));
        QVERIFY(server.paths.contains("/cache/fakeip/flush"));
        for (const auto &request : server.requests)
            if (request.contains("/cache/")) QVERIFY(request.startsWith("POST "));
    }

    void geoUpdatesAreAuthenticatedDeduplicatedAndReportFailures() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        core::MihomoClient client;
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        QSignalSpy updates(&client, &core::MihomoClient::geoDatabasesUpdated);
        client.updateGeoDatabases();
        client.updateGeoDatabases();
        QTRY_COMPARE(updates.size(), 1);
        QVERIFY(updates.first().first().toString().isEmpty());
        QCOMPARE(server.paths.count("/configs/geo"), 1);
        bool found = false;
        for (const auto &request : server.requests)
            if (request.startsWith("POST /configs/geo ")) {
                QVERIFY(request.contains("Authorization: Bearer secret"));
                found = true;
            }
        QVERIFY(found);
        server.statuses["POST /configs/geo"] = "500 Internal Server Error";
        client.updateGeoDatabases();
        QTRY_COMPARE(updates.size(), 2);
        QVERIFY(updates.last().first().toString().contains("500"));
    }

    void tunChangeConfirmsStateAndDeduplicates() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/configs"] = R"({"mode":"rule","mixed-port":7890,"tun":{"enable":false}})";
        server.responses["PATCH /configs"] = {};
        server.statuses["PATCH /configs"] = "204 No Content";
        server.delays["PATCH /configs"] = 80;
        QList<QJsonObject> patches;
        server.onRequest = [&](const QByteArray &method, const QByteArray &body) {
            if (method != "PATCH /configs") return;
            const auto patch = QJsonDocument::fromJson(body).object();
            patches.append(patch);
            auto config = QJsonDocument::fromJson(server.responses["/configs"]).object();
            config["tun"] = patch.value("tun");
            server.responses["/configs"] = QJsonDocument(config).toJson(QJsonDocument::Compact);
        };
        core::MihomoClient client;
        QSignalSpy completed(&client, &core::MihomoClient::tunChangeFinished);
        QVector<bool> snapshots;
        connect(&client, &core::MihomoClient::configReceived, this,
                [&](const core::BaseConfig &config) { snapshots.append(config.tunEnabled); });
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        QTRY_VERIFY(!snapshots.isEmpty());
        const int before = server.paths.count("/configs");
        client.setTunEnabled(true);
        QVERIFY(client.isTunChangePending());
        client.setTunEnabled(true);
        client.setTunEnabled(false);
        QCOMPARE(completed.size(), 0); // Never report optimistic success before network readback.
        QTRY_COMPARE(patches.size(), 1);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!client.isTunChangePending());
        QCOMPARE(completed.first().at(0).toBool(), true);
        QCOMPARE(completed.first().at(1).toBool(), true);
        QVERIFY(completed.first().at(2).toString().isEmpty());
        QCOMPARE(server.paths.count("/configs"), before + 3); // preflight, patch, readback
        QCOMPARE(patches.first().keys(), QStringList{"tun"});
        const auto tun = patches.first().value("tun").toObject();
        QCOMPARE(tun.value("stack").toString(), QString("mixed"));
        QCOMPARE(tun.value("auto-route").toBool(), true);
        QCOMPARE(tun.value("auto-detect-interface").toBool(), true);
        QCOMPARE(snapshots.last(), true);
        QVERIFY(snapshots.contains(false));
    }

    void tunChangePreservesExistingConfigurationAndDisables() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/configs"] = R"({"mode":"rule","tun":{"enable":false,"stack":"system","auto-route":false,"auto-detect-interface":false,"device":"custom-tun","dns-hijack":["any:53"]}})";
        server.responses["PATCH /configs"] = {};
        server.statuses["PATCH /configs"] = "204 No Content";
        QList<QJsonObject> patches;
        server.onRequest = [&](const QByteArray &method, const QByteArray &body) {
            if (method != "PATCH /configs") return;
            const auto patch = QJsonDocument::fromJson(body).object().value("tun").toObject();
            patches.append(patch);
            auto config = QJsonDocument::fromJson(server.responses["/configs"]).object();
            auto tun = config.value("tun").toObject();
            for (auto it = patch.begin(); it != patch.end(); ++it) tun.insert(it.key(), it.value());
            config["tun"] = tun;
            server.responses["/configs"] = QJsonDocument(config).toJson();
        };
        core::MihomoClient client;
        QSignalSpy completed(&client, &core::MihomoClient::tunChangeFinished);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        client.setTunEnabled(true);
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(patches.first(), (QJsonObject{{"enable", true}}));
        client.setTunEnabled(true); // already enabled: read actual state, no mutation
        QTRY_COMPARE(completed.size(), 2);
        QCOMPARE(patches.size(), 1);
        client.setTunEnabled(false);
        QTRY_COMPARE(completed.size(), 3);
        QCOMPARE(patches.last(), (QJsonObject{{"enable", false}}));
        QCOMPARE(completed.last().at(1).toBool(), false);
        QVERIFY(completed.last().at(2).toString().isEmpty());
        const auto tun = QJsonDocument::fromJson(server.responses["/configs"]).object().value("tun").toObject();
        QCOMPARE(tun.value("device").toString(), QString("custom-tun"));
        QCOMPARE(tun.value("auto-route").toBool(), false);
        QCOMPARE(tun.value("stack").toString(), QString("system"));
    }

    void tunChangeRejectsAcceptedButInactiveListener() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/configs"] = R"({"mode":"rule","tun":{"enable":false}})";
        server.responses["PATCH /configs"] = {};
        server.statuses["PATCH /configs"] = "204 No Content";
        core::MihomoClient client;
        QSignalSpy completed(&client, &core::MihomoClient::tunChangeFinished);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        client.setTunEnabled(true);
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(completed.first().at(0).toBool(), true);
        QCOMPARE(completed.first().at(1).toBool(), false);
        QVERIFY(completed.first().at(2).toString().contains("still disabled"));
        QVERIFY(!client.isTunChangePending());
    }

    void tunChangeFailureAndMalformedConfirmation() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/configs"] = R"({"mode":"rule","tun":{"enable":false}})";
        server.responses["PATCH /configs"] = R"({"message":"permission denied"})";
        server.statuses["PATCH /configs"] = "403 Forbidden";
        core::MihomoClient client;
        QSignalSpy completed(&client, &core::MihomoClient::tunChangeFinished);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        client.setTunEnabled(true);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(completed.first().at(2).toString().contains("permission denied"));
        QCOMPARE(completed.first().at(1).toBool(), false);
        server.statuses["PATCH /configs"] = "204 No Content";
        server.responses["PATCH /configs"] = {};
        server.onRequest = [&](const QByteArray &method, const QByteArray &) {
            if (method == "PATCH /configs") server.responses["/configs"] = "not-json";
        };
        client.setTunEnabled(true);
        QTRY_COMPARE(completed.size(), 2);
        QVERIFY(completed.last().at(2).toString().contains("Could not confirm"));
        QVERIFY(!client.isTunChangePending());
    }

    void tunChangeRejectsDisconnectedController() {
        core::MihomoClient client;
        QSignalSpy completed(&client, &core::MihomoClient::tunChangeFinished);
        client.setTunEnabled(true);
        QCOMPARE(completed.size(), 1);
        QVERIFY(!completed.first().at(2).toString().isEmpty());
        QVERIFY(!client.isTunChangePending());
    }

    void confirmedTunStateCannotBeOverwrittenByOldConfigPoll() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/configs"] = R"({"mode":"rule","tun":{"enable":false}})";
        server.responses["PATCH /configs"] = {};
        server.statuses["PATCH /configs"] = "204 No Content";
        server.onRequest = [&](const QByteArray &method, const QByteArray &) {
            if (method == "PATCH /configs")
                server.responses["/configs"] = R"({"mode":"rule","tun":{"enable":true}})";
        };
        core::MihomoClient client;
        QSignalSpy completed(&client, &core::MihomoClient::tunChangeFinished);
        QVector<bool> snapshots;
        connect(&client, &core::MihomoClient::configReceived, this,
                [&](const core::BaseConfig &config) { snapshots.append(config.tunEnabled); });
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        QTRY_VERIFY(!snapshots.isEmpty());
        const int before = server.paths.count("/configs");
        server.delays["GET /configs"] = 220;
        client.fetchConfigs();
        QTRY_COMPARE(server.paths.count("/configs"), before + 1);
        server.delays.remove("GET /configs");
        client.setTunEnabled(true);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(completed.first().at(2).toString().isEmpty());
        QCOMPARE(snapshots.last(), true);
        const int confirmedCount = snapshots.size();
        QTest::qWait(260);
        QCOMPARE(snapshots.size(), confirmedCount);
        QCOMPARE(snapshots.last(), true);
    }

    void tunChangeCancellationReleasesPendingAndIgnoresOldReply() {
        TestController oldServer, newServer;
        QVERIFY(oldServer.listen(QHostAddress::LocalHost));
        QVERIFY(newServer.listen(QHostAddress::LocalHost));
        oldServer.responses["/configs"] = R"({"mode":"rule","tun":{"enable":false}})";
        newServer.responses["/configs"] = R"({"mode":"rule","tun":{"enable":false}})";
        oldServer.responses["PATCH /configs"] = {};
        oldServer.statuses["PATCH /configs"] = "204 No Content";
        oldServer.delays["PATCH /configs"] = 180;
        core::MihomoClient client;
        QSignalSpy completed(&client, &core::MihomoClient::tunChangeFinished);
        client.setEndpoint(oldServer.endpoint());
        QTRY_VERIFY(client.isConnected());
        client.setTunEnabled(true);
        QTRY_VERIFY(std::any_of(oldServer.requests.cbegin(), oldServer.requests.cend(), [](const QByteArray &request) {
            return request.startsWith("PATCH /configs ");
        }));
        client.setEndpoint(newServer.endpoint());
        QCOMPARE(completed.size(), 1);
        QVERIFY(completed.first().at(2).toString().contains("controller changed"));
        QVERIFY(!client.isTunChangePending());
        QTest::qWait(230);
        QCOMPARE(completed.size(), 1);
        QTRY_VERIFY(client.isConnected());
        newServer.delays["GET /configs"] = 180;
        client.setTunEnabled(true);
        newServer.statuses["/version"] = "503 Unavailable";
        client.fetchVersion();
        QTRY_VERIFY(!client.isConnected());
        QCOMPARE(completed.size(), 2);
        QVERIFY(completed.last().at(2).toString().contains("disconnected"));
        QVERIFY(!client.isTunChangePending());
        QTest::qWait(230);
        QCOMPARE(completed.size(), 2);
    }

    void invalidVersionDoesNotConnect() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/version"] = "not-json";
        core::MihomoClient client;
        QSignalSpy errors(&client, &core::MihomoClient::errorOccurred);
        QSignalSpy versions(&client, &core::MihomoClient::versionReceived);
        client.setEndpoint(server.endpoint());
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!client.isConnected());
        QCOMPARE(versions.size(), 0);
    }
};

QTEST_GUILESS_MAIN(ControllerTest)
#include "controller_test.moc"
