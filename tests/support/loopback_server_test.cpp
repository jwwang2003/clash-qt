#include <QtTest>

#include <algorithm>

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QWebSocket>

#include "support/loopback_server.h"

using testsupport::LoopbackServer;
using testsupport::RecordedRequest;

namespace {
QString fixtureSecret() { return QStringLiteral("0123456789abcdef0123456789abcdef"); }
}

class LoopbackServerTest : public QObject {
    Q_OBJECT

    QNetworkReply *get(LoopbackServer &server, const QString &path, bool authenticate = true) {
        QNetworkRequest request{QUrl(server.httpBase() + path)};
        if (authenticate)
            request.setRawHeader("Authorization", "Bearer " + server.secret().toUtf8());
        return network_.get(request);
    }

    static bool finished(QNetworkReply *reply, LoopbackServer &server, int timeoutMs = 5000) {
        return server.waitFor([reply] { return reply->isFinished(); }, timeoutMs);
    }

    QWebSocket *openStream(LoopbackServer &server, const QString &path, bool authenticate = true) {
        auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        QNetworkRequest request{QUrl(server.wsBase() + path)};
        if (authenticate)
            request.setRawHeader("Authorization", "Bearer " + server.secret().toUtf8());
        socket->open(request);
        return socket;
    }

    QNetworkAccessManager network_;

private slots:
    void unknownRequestFailsInsteadOfReturningEmptyJson() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        QSignalSpy unexpected(&server, &LoopbackServer::unexpectedRequest);

        QScopedPointer<QNetworkReply> reply(get(server, QStringLiteral("/proxies")));
        QVERIFY2(finished(reply.data(), server), qPrintable(server.pendingReport()));

        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 501);
        const QByteArray body = reply->readAll();
        QVERIFY2(body != "{}", "A permissive default turns missing coverage into a pass");
        QVERIFY(body.contains("unscripted"));
        QVERIFY(server.sawUnexpectedRequest());
        QCOMPARE(unexpected.size(), 1);
        QCOMPARE(unexpected.first().at(1).toString(), QStringLiteral("/proxies"));
    }

    void scriptedRouteRepliesAndRecordsTheRequest() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.route("GET", QStringLiteral("/version"),
                     LoopbackServer::Reply::json(R"({"version":"fixture"})"));

        QScopedPointer<QNetworkReply> reply(get(server, QStringLiteral("/version")));
        QVERIFY2(finished(reply.data(), server), qPrintable(server.pendingReport()));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QCOMPARE(reply->readAll(), QByteArray(R"({"version":"fixture"})"));

        QVERIFY(!server.sawUnexpectedRequest());
        QCOMPARE(server.requestCount("GET", QStringLiteral("/version")), 1);
        const RecordedRequest recorded = server.requests().first();
        QVERIFY(recorded.authenticated);
        QVERIFY(recorded.scripted);
    }

    // MihomoClient issues four controller requests at once on every endpoint
    // change, so the fixture has to survive a connection pool rather than one
    // request at a time.
    void concurrentRequestsAllComplete() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.route("GET", QStringLiteral("/version"), LoopbackServer::Reply::json(R"({"version":"v"})"));
        server.route("GET", QStringLiteral("/proxies"), LoopbackServer::Reply::json(R"({"proxies":{}})"));
        server.route("GET", QStringLiteral("/rules"), LoopbackServer::Reply::json(R"({"rules":[]})"));
        server.route("GET", QStringLiteral("/configs"), LoopbackServer::Reply::json(R"({"mode":"rule"})"));

        QList<QNetworkReply *> replies;
        for (const QString &path : {QStringLiteral("/version"), QStringLiteral("/proxies"),
                                    QStringLiteral("/rules"), QStringLiteral("/configs")})
            replies.append(get(server, path));

        QVERIFY2(server.waitFor([&replies] {
                     return std::all_of(replies.cbegin(), replies.cend(),
                                        [](QNetworkReply *reply) { return reply->isFinished(); });
                 }), qPrintable(server.pendingReport()));
        for (QNetworkReply *reply : replies) {
            QCOMPARE(reply->error(), QNetworkReply::NoError);
            QVERIFY2(!reply->readAll().isEmpty(), qPrintable(reply->url().path()));
            reply->deleteLater();
        }
        QVERIFY(!server.sawUnexpectedRequest());
    }

    void missingCredentialsAreRejected() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.route("GET", QStringLiteral("/version"), LoopbackServer::Reply::json("{}"));

        QScopedPointer<QNetworkReply> reply(get(server, QStringLiteral("/version"), false));
        QVERIFY2(finished(reply.data(), server), qPrintable(server.pendingReport()));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 401);
        QVERIFY(server.sawUnauthenticatedRequest());
        QVERIFY(!server.requests().first().authenticated);
    }

    void tokenQueryParameterAuthenticatesLikeTheController() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.route("GET", QStringLiteral("/logs"), LoopbackServer::Reply::json("{}"));

        QScopedPointer<QNetworkReply> reply(
            get(server, QStringLiteral("/logs?token=") + fixtureSecret(), false));
        QVERIFY2(finished(reply.data(), server), qPrintable(server.pendingReport()));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QVERIFY(server.requests().first().authenticated);
    }

    void gateHoldsTheReplyUntilTheTestReleasesIt() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.route("GET", QStringLiteral("/configs"),
                     LoopbackServer::Reply::json(R"({"mode":"rule"})"));
        LoopbackServer::Gate *gate = server.hold("GET", QStringLiteral("/configs"));
        QSignalSpy pending(gate, &LoopbackServer::Gate::pendingChanged);

        QScopedPointer<QNetworkReply> reply(get(server, QStringLiteral("/configs")));
        QVERIFY2(gate->waitForPending(1), qPrintable(server.pendingReport()));
        QCOMPARE(pending.size(), 1);

        // Observable pending state: the request arrived, nothing answered it.
        QVERIFY(!reply->isFinished());
        QCOMPARE(server.requestCount("GET", QStringLiteral("/configs")), 1);
        QVERIFY(server.pendingReport().contains(QStringLiteral("held: GET /configs x1")));

        gate->release();
        QCOMPARE(gate->pending(), 0);
        QCOMPARE(gate->released(), 1);
        QVERIFY2(finished(reply.data(), server), qPrintable(server.pendingReport()));
        QCOMPARE(reply->readAll(), QByteArray(R"({"mode":"rule"})"));
    }

    void releasingAGateWithNoScriptedRouteFailsLoudly() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        LoopbackServer::Gate *gate = server.hold("GET", QStringLiteral("/rules"));

        QScopedPointer<QNetworkReply> reply(get(server, QStringLiteral("/rules")));
        QVERIFY(gate->waitForPending(1));
        gate->release();
        QVERIFY2(finished(reply.data(), server), qPrintable(server.pendingReport()));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 501);
        QVERIFY(server.sawUnexpectedRequest());
    }

    void rejectAndDisconnectAreScriptable() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.route("GET", QStringLiteral("/configs"), LoopbackServer::Reply::failure(503));
        server.route("GET", QStringLiteral("/rules"), LoopbackServer::Reply::drop());

        QScopedPointer<QNetworkReply> rejected(get(server, QStringLiteral("/configs")));
        QVERIFY2(finished(rejected.data(), server), qPrintable(server.pendingReport()));
        QCOMPARE(rejected->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 503);

        QScopedPointer<QNetworkReply> dropped(get(server, QStringLiteral("/rules")));
        QVERIFY2(finished(dropped.data(), server), qPrintable(server.pendingReport()));
        QVERIFY(dropped->error() != QNetworkReply::NoError);
    }

    void failureTranscriptRedactsCredentials() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        QScopedPointer<QNetworkReply> reply(
            get(server, QStringLiteral("/proxies?token=") + fixtureSecret()));
        QVERIFY(finished(reply.data(), server));

        const QString transcript = server.redactedTranscript();
        QVERIFY2(!transcript.contains(fixtureSecret()), qPrintable(transcript));
        QVERIFY(transcript.contains(QStringLiteral("<redacted>")));
        QVERIFY(transcript.contains(QStringLiteral("scripted=NO")));
        QVERIFY(server.pendingReport().contains(QStringLiteral("<redacted>")));
    }

    void webSocketHandshakeRequiresAuthentication() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.expectStream(QStringLiteral("/traffic"));

        QWebSocket *socket = openStream(server, QStringLiteral("/traffic"), false);
        QSignalSpy connected(socket, &QWebSocket::connected);
        QVERIFY2(server.waitFor([&server] { return !server.requests().isEmpty(); }),
                 qPrintable(server.pendingReport()));
        QCOMPARE(server.requests().first().method, QByteArray("WS"));
        QVERIFY(!server.requests().first().authenticated);
        QVERIFY2(!server.waitFor([&connected] { return connected.size() > 0; }, 300),
                 "A handshake without credentials must not be accepted");
        QCOMPARE(server.openStreams(QStringLiteral("/traffic")), 0);
        socket->abort();
        delete socket;
    }

    void webSocketOnAnUnexpectedPathIsRefused() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        QWebSocket *socket = openStream(server, QStringLiteral("/memory"));
        QVERIFY2(server.waitFor([&server] { return server.sawUnexpectedRequest(); }),
                 qPrintable(server.pendingReport()));
        QCOMPARE(server.openStreams(QStringLiteral("/memory")), 0);
        socket->abort();
        delete socket;
    }

    void webSocketDeliversWholeAndFragmentedMessages() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.expectStream(QStringLiteral("/traffic"));

        QWebSocket *socket = openStream(server, QStringLiteral("/traffic"));
        QSignalSpy messages(socket, &QWebSocket::textMessageReceived);
        QVERIFY2(server.waitFor([&server] { return server.openStreams(QStringLiteral("/traffic")) == 1; }),
                 qPrintable(server.pendingReport()));
        QCOMPARE(server.streamHandshakes(QStringLiteral("/traffic")), 1);

        QVERIFY(server.sendText(QStringLiteral("/traffic"), QStringLiteral(R"({"up":1,"down":2})")));
        QVERIFY2(server.waitFor([&messages] { return messages.size() == 1; }),
                 qPrintable(server.pendingReport()));
        QCOMPARE(messages.takeFirst().first().toString(), QStringLiteral(R"({"up":1,"down":2})"));

        // One logical message split across three frames must arrive reassembled.
        const QString payload = QStringLiteral(R"({"up":123456,"down":654321,"note":"fragmenté"})");
        QVERIFY(server.sendFragmentedText(QStringLiteral("/traffic"), payload, 3));
        QVERIFY2(server.waitFor([&messages] { return messages.size() == 1; }),
                 qPrintable(server.pendingReport()));
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.takeFirst().first().toString(), payload);

        socket->close();
        QVERIFY(server.waitFor([&server] { return server.openStreams(QStringLiteral("/traffic")) == 0; }));
        delete socket;
    }

    void clientMessagesAreObservable() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.expectStream(QStringLiteral("/connections"));
        QSignalSpy inbound(&server, &LoopbackServer::streamMessage);

        QWebSocket *socket = openStream(server, QStringLiteral("/connections"));
        QVERIFY(server.waitFor([&server] { return server.openStreams(QStringLiteral("/connections")) == 1; }));
        // The client must have processed the 101 before it can send.
        QVERIFY2(server.waitFor([socket] { return socket->state() == QAbstractSocket::ConnectedState; }),
                 qPrintable(server.pendingReport()));
        socket->sendTextMessage(QStringLiteral("ping-from-client"));
        QVERIFY2(server.waitFor([&inbound] { return inbound.size() == 1; }), qPrintable(server.pendingReport()));
        QCOMPARE(inbound.first().at(1).toString(), QStringLiteral("ping-from-client"));
        socket->abort();
        delete socket;
    }

    void droppedStreamIsObservedAndReconnectIsCounted() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.expectStream(QStringLiteral("/traffic"));

        QWebSocket *socket = openStream(server, QStringLiteral("/traffic"));
        QSignalSpy disconnected(socket, &QWebSocket::disconnected);
        QSignalSpy closed(&server, &LoopbackServer::streamClosed);
        QVERIFY(server.waitFor([&server] { return server.openStreams(QStringLiteral("/traffic")) == 1; }));

        QVERIFY(server.dropStream(QStringLiteral("/traffic")));
        QVERIFY2(server.waitFor([&disconnected] { return disconnected.size() == 1; }),
                 qPrintable(server.pendingReport()));
        QCOMPARE(closed.size(), 1);
        QCOMPARE(server.openStreams(QStringLiteral("/traffic")), 0);

        // Reconnect, as MihomoClient does after the core restarts.
        QNetworkRequest request{QUrl(server.wsBase() + QStringLiteral("/traffic"))};
        request.setRawHeader("Authorization", "Bearer " + server.secret().toUtf8());
        socket->open(request);
        QVERIFY2(server.waitFor([&server] { return server.streamHandshakes(QStringLiteral("/traffic")) == 2; }),
                 qPrintable(server.pendingReport()));
        QCOMPARE(server.openStreams(QStringLiteral("/traffic")), 1);
        socket->abort();
        delete socket;
    }

    void closeReleasesEveryLiveSocket() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.expectStream(QStringLiteral("/traffic"));
        QWebSocket *socket = openStream(server, QStringLiteral("/traffic"));
        QSignalSpy disconnected(socket, &QWebSocket::disconnected);
        QVERIFY(server.waitFor([&server] { return server.openStreams(QStringLiteral("/traffic")) == 1; }));

        server.close();
        QCOMPARE(server.openStreams(QStringLiteral("/traffic")), 0);
        QVERIFY(server.waitFor([&disconnected] { return disconnected.size() == 1; }));
        delete socket;
    }
};

QTEST_GUILESS_MAIN(LoopbackServerTest)
#include "loopback_server_test.moc"
