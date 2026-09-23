#include <QtTest>
#include <algorithm>
#include <functional>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>
#include <QTimer>
#include <QTemporaryFile>
#include "core/mihomo/mihomo_client.h"
#include "core/mihomo/controller_discovery.h"

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

    // core::Endpoint's default is the DISCOVERY default. A guess about where a
    // controller might be is not an attachment, and a client that adopts it at
    // construction makes every published attachment query answer for a
    // controller nothing pointed it at.
    void aFreshClientIsAttachedToNothing() {
        const core::Endpoint discoveryDefault;
        QCOMPARE(discoveryDefault.host, QString("127.0.0.1"));
        QCOMPARE(discoveryDefault.port, quint16(9090));
        QVERIFY2(discoveryDefault.isValid(),
                 "the discovery default must stay a usable endpoint: it is what "
                 "controller_discovery falls back to");

        core::MihomoClient client;
        QVERIFY2(!client.endpoint().isValid(),
                 "a client nothing has pointed anywhere reports itself attached");
        QVERIFY(client.endpoint().host.isEmpty());
        QCOMPARE(client.endpoint().port, quint16(0));
        QVERIFY(!client.isConnected());
        QVERIFY(!core::MihomoClient::detachedEndpoint().isValid());

        // An explicit attach is what makes it an attachment - and because the
        // client starts from nowhere, the first one is a CHANGE and is
        // published as one. It used to be silent whenever the controller
        // happened to sit on the default port, which is the common case for a
        // managed core.
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QSignalSpy changed(&client, &core::MihomoClient::endpointChanged);
        client.setEndpoint(server.endpoint());
        QVERIFY(client.endpoint().isValid());
        QCOMPARE(changed.size(), 1);
        QTRY_VERIFY(client.isConnected());

        // Detaching returns it to "nothing", not to the discovery default.
        client.detach();
        QCOMPARE(changed.size(), 2);
        QVERIFY(!client.endpoint().isValid());
        QVERIFY(!client.isConnected());
    }

    // A reload rebinds the replacement engine to the port the retired one held,
    // so the same endpoint arrives again and means a NEW session. Everything
    // the retired process still owes must be retired with it: left current,
    // those replies land after the replacement has answered and report a
    // healthy engine as disconnected.
    void reattachingToAReplacedEngineRetiresItsInFlightWork() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/version"] = R"({"version":"first-session"})";
        core::MihomoClient client;
        QSignalSpy versions(&client, &core::MihomoClient::versionReceived);
        QSignalSpy connections(&client, &core::MihomoClient::connectedChanged);
        QSignalSpy errors(&client, &core::MihomoClient::errorOccurred);
        QSignalSpy invalidations(&client, &core::MihomoClient::invalidating);
        QSignalSpy changed(&client, &core::MihomoClient::endpointChanged);
        QSignalSpy settled(&client, &core::MihomoClient::requestSettled);
        QSignalSpy cleared(&client, &core::MihomoClient::trafficSample);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        QTRY_COMPARE(versions.size(), 1);

        // What the engine that is about to go still owes. The fixture captures
        // body, status and delay when the request ARRIVES, so this one is
        // already committed to failing late even after the scripting changes.
        const int probes = server.paths.count("/version");
        server.delays["/version"] = 200;
        server.statuses["/version"] = "503 Unavailable";
        const quint64 stale = client.fetchVersion();
        QVERIFY(stale != 0);
        QTRY_COMPARE(server.paths.count("/version"), probes + 1);

        // The replacement binds the same host, port and secret.
        server.delays.remove("/version");
        server.statuses.remove("/version");
        server.responses["/version"] = R"({"version":"second-session"})";
        const int announced = invalidations.size();
        client.setEndpoint(server.endpoint());
        // Exactly one. None means nothing the retired engine owed was retired;
        // more than one means an aborted reply was accepted as live on its way
        // out and disconnected the session, because setConnected(false)
        // announces an invalidation of its own.
        QCOMPARE(invalidations.size(), announced + 1);
        QVERIFY2(changed.size() == 1,
                 "an endpoint that did not change was published as an endpoint change");

        // The replacement answers. From here on the retired engine is replying
        // AFTER the new response.
        QTRY_COMPARE(versions.size(), 2);
        QCOMPARE(versions.last().first().toString(), QString("second-session"));
        QVERIFY(client.isConnected());
        const int transitions = connections.size();
        const int clears = cleared.size();

        QTest::qWait(300);
        QVERIFY2(client.isConnected(),
                 "a reply owed by the retired engine disconnected the live session");
        QVERIFY2(connections.size() == transitions,
                 "a reply owed by the retired engine produced a connection transition");
        QVERIFY2(cleared.size() == clears,
                 "a reply owed by the retired engine cleared the live view the "
                 "replacement had just populated");
        QCOMPARE(errors.size(), 0);
        QCOMPARE(versions.size(), 2);

        // It is retired, not silently dropped: its single terminal event says
        // superseded, with no error to report to anyone.
        bool sawStale = false;
        for (const auto &row : settled) {
            if (row.at(0).toULongLong() != stale) continue;
            sawStale = true;
            QVERIFY2(row.at(1).toBool(), "the retired engine's reply settled as live work");
            QVERIFY(row.at(2).toString().isEmpty());
        }
        QVERIFY2(sawStale, "the retired engine's request never settled at all");
    }

    // The same boundary, for the one operation that spans several round trips.
    void aReplacedEngineCancelsAnOutstandingTunChangeExactlyOnce() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/configs"] = R"({"mode":"rule","tun":{"enable":false}})";
        server.responses["PATCH /configs"] = {};
        server.statuses["PATCH /configs"] = "204 No Content";
        server.delays["PATCH /configs"] = 180;
        core::MihomoClient client;
        QSignalSpy completed(&client, &core::MihomoClient::tunChangeFinished);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        client.setTunEnabled(true);
        QTRY_VERIFY(std::any_of(server.requests.cbegin(), server.requests.cend(),
                                [](const QByteArray &request) {
                                    return request.startsWith("PATCH /configs ");
                                }));

        client.setEndpoint(server.endpoint());
        QCOMPARE(completed.size(), 1);
        QVERIFY(completed.first().at(2).toString().contains("replaced"));
        QVERIFY(!client.isTunChangePending());
        // The retired engine's confirmation arrives here, and must not report a
        // second, contradictory outcome for a change already called off.
        QTest::qWait(230);
        QCOMPARE(completed.size(), 1);
        // The new session owns the seam: a change is accepted rather than
        // refused by a pending flag nobody will ever clear.
        QVERIFY(client.setTunEnabled(true) != 0);
    }

    // The SAME boundary, driven by the owner instead of by an attachment.
    //
    // A managed start, a managed failure and a stop all invalidate outstanding
    // work by the contract's own definition, and none of them is a client
    // event: nothing here changes address or connectivity. retireSession() is
    // how the owner performs that boundary, and the thing it must NOT do is
    // announce an invalidation of its own - invalidating() is what makes the
    // owner bump, so re-emitting it here would ask for a second bump, and a
    // second retirement, for one logical boundary.
    void anOwnerDrivenRetirementDoesNotAnnounceASecondInvalidation() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        core::MihomoClient client;
        QSignalSpy invalidations(&client, &core::MihomoClient::invalidating);
        QSignalSpy changed(&client, &core::MihomoClient::endpointChanged);
        QSignalSpy settled(&client, &core::MihomoClient::requestSettled);
        QSignalSpy connections(&client, &core::MihomoClient::connectedChanged);
        QSignalSpy cleared(&client, &core::MihomoClient::trafficSample);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());

        const int announced = invalidations.size();
        const int endpoints = changed.size();
        const int transitions = connections.size();
        const int clears = cleared.size();
        const quint64 session = client.sessionEpoch();

        // What the session that is about to be retired still owes.
        const int probes = server.paths.count("/version");
        server.delays["/version"] = 200;
        server.statuses["/version"] = "503 Unavailable";
        const quint64 stale = client.fetchVersion();
        QVERIFY(stale != 0);
        QTRY_COMPARE(server.paths.count("/version"), probes + 1);

        client.retireSession(QStringLiteral("the managed core was replaced"));

        QVERIFY2(invalidations.size() == announced,
                 "an owner-driven retirement announced an invalidation of its own: the owner "
                 "bumps on that signal, so one boundary would become two");
        QVERIFY2(client.sessionEpoch() > session,
                 "the session did not move, so nothing the previous one owed was retired");
        QVERIFY2(changed.size() == endpoints,
                 "a lifecycle retirement published an endpoint change; it changes no address");
        QVERIFY2(connections.size() == transitions && client.isConnected(),
                 "a lifecycle retirement decided connectivity, which only the controller can");
        QVERIFY2(cleared.size() == clears,
                 "a lifecycle retirement cleared the live view instead of leaving the "
                 "re-issue its owner owes to re-establish it");

        // Retired, not dropped: one terminal event, marked, with nothing to
        // report to anyone.
        bool sawStale = false;
        for (const auto &row : settled) {
            if (row.at(0).toULongLong() != stale) continue;
            sawStale = true;
            QVERIFY2(row.at(1).toBool(), "a reply owed by the retired session settled as live work");
            QVERIFY(row.at(2).toString().isEmpty());
        }
        QVERIFY2(sawStale, "the retired session's request never settled at all");

        // And the retired reply, when it finally lands, is nobody's.
        QTest::qWait(250);
        QVERIFY(client.isConnected());
        QCOMPARE(connections.size(), transitions);
    }

    void anOwnerDrivenRetirementCancelsAnOutstandingTunChange() {
        TestController server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/configs"] = R"({"mode":"rule","tun":{"enable":false}})";
        server.responses["PATCH /configs"] = {};
        server.statuses["PATCH /configs"] = "204 No Content";
        server.delays["PATCH /configs"] = 180;
        core::MihomoClient client;
        QSignalSpy completed(&client, &core::MihomoClient::tunChangeFinished);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(client.isConnected());
        QVERIFY(client.setTunEnabled(true) != 0);
        QTRY_VERIFY(std::any_of(server.requests.cbegin(), server.requests.cend(),
                                [](const QByteArray &request) {
                                    return request.startsWith("PATCH /configs ");
                                }));

        client.retireSession(QStringLiteral("the managed core failed"));
        // Exactly once, and as a cancellation rather than as a protocol error
        // the engine never committed: the adapter classifies it by the fact of
        // the retirement, never by this text.
        QCOMPARE(completed.size(), 1);
        QVERIFY(!client.isTunChangePending());
        QVERIFY(completed.first().at(2).toString().contains("failed"));
        QTest::qWait(230);
        QCOMPARE(completed.size(), 1);
        QVERIFY(client.setTunEnabled(true) != 0);
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
