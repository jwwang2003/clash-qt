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

    // --- scripted response headers -------------------------------------------
    //
    // A subscription's quota and expiry do not arrive in the body: they arrive
    // in `subscription-userinfo`, and its filename arrives in
    // `content-disposition`. core::ProfileStore parses both out of the RAW
    // header text, so the fixture's job is to put the bytes a provider sends on
    // the wire and not to have an opinion about them. These two cases are what
    // makes the subscription journey's quota assertions meaningful rather than
    // circular:
    // without them, a fixture that quietly dropped or rewrote the header would
    // make "the quota did not arrive" indistinguishable from "the store cannot
    // parse it".

    void scriptedResponseHeadersReachTheClientVerbatim() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        // Deliberately awkward spacing and ordering, because a real provider's
        // header is not normalised either.
        const QByteArray quota =
            QByteArrayLiteral("upload=1024; download= 2048 ;total=10737418240;expire=1794499200");
        const QByteArray disposition =
            QByteArrayLiteral("attachment; filename=\"subscription-fixture.yaml\"");
        server.route("GET", QStringLiteral("/sub.yaml"),
                     LoopbackServer::Reply::document("text/yaml", QByteArrayLiteral("proxies: []\n"))
                         .withHeader("subscription-userinfo", quota)
                         .withHeader("content-disposition", disposition));
        server.route("GET", QStringLiteral("/plain.yaml"),
                     LoopbackServer::Reply::document("text/yaml", QByteArrayLiteral("proxies: []\n")));

        QScopedPointer<QNetworkReply> reply(get(server, QStringLiteral("/sub.yaml")));
        QVERIFY2(finished(reply.data(), server), qPrintable(server.pendingReport()));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        // Byte for byte: the spacing is what production's regex has to survive.
        QCOMPARE(reply->rawHeader("subscription-userinfo"), quota);
        QCOMPARE(reply->rawHeader("content-disposition"), disposition);
        QCOMPARE(reply->header(QNetworkRequest::ContentTypeHeader).toByteArray(),
                 QByteArrayLiteral("text/yaml"));
        QCOMPARE(reply->readAll(), QByteArrayLiteral("proxies: []\n"));

        // No bleed-through: a route that scripts no headers sends none. A
        // fixture that kept them on the connection would make the journey's
        // "the quota changed" assertion pass on a stale value.
        QScopedPointer<QNetworkReply> plain(get(server, QStringLiteral("/plain.yaml")));
        QVERIFY2(finished(plain.data(), server), qPrintable(server.pendingReport()));
        QVERIFY2(plain->rawHeader("subscription-userinfo").isEmpty(),
                 qPrintable(QStringLiteral("an unrelated route answered with %1")
                                .arg(QString::fromUtf8(plain->rawHeader("subscription-userinfo")))));
        QVERIFY(plain->rawHeader("content-disposition").isEmpty());
        QVERIFY(!server.sawUnexpectedRequest());
    }

    void reservedResponseHeadersAreRefusedRatherThanDuplicated() {
        using Reply = LoopbackServer::Reply;
        QVERIFY(Reply::isReservedHeader("Content-Length"));
        QVERIFY(Reply::isReservedHeader("content-type"));
        QVERIFY(Reply::isReservedHeader(" Connection "));
        QVERIFY(!Reply::isReservedHeader("subscription-userinfo"));

        // withHeader() drops it...
        const Reply attempted = Reply::json(QByteArrayLiteral("{}"))
                                    .withHeader("Content-Length", "1")
                                    .withHeader("", "ignored")
                                    .withHeader("x-fixture", "kept");
        QCOMPARE(attempted.headers.size(), 1);
        QCOMPARE(attempted.headers.first().first, QByteArrayLiteral("x-fixture"));

        // ...and so does the wire, for a caller that appended to the plain
        // struct directly.
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        Reply forced = Reply::json(QByteArrayLiteral("{\"version\":\"fixture\"}"));
        forced.headers.append({QByteArrayLiteral("Content-Length"), QByteArrayLiteral("1")});
        forced.headers.append({QByteArrayLiteral("x-fixture"), QByteArrayLiteral("kept")});
        server.route("GET", QStringLiteral("/version"), forced);

        QScopedPointer<QNetworkReply> reply(get(server, QStringLiteral("/version")));
        QVERIFY2(finished(reply.data(), server), qPrintable(server.pendingReport()));
        QCOMPARE(reply->rawHeader("x-fixture"), QByteArrayLiteral("kept"));
        // A leaked duplicate would arrive as "22, 1": Qt joins repeated headers.
        QCOMPARE(reply->rawHeader("Content-Length"),
                 QByteArray::number(QByteArrayLiteral("{\"version\":\"fixture\"}").size()));
        QCOMPARE(reply->readAll(), QByteArrayLiteral("{\"version\":\"fixture\"}"));
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

    // Two sessions overlap on one path whenever an engine is replaced at the
    // address it already held: the retired socket is still open when the
    // successor dials back in. Killing the retired one may not take the
    // successor with it.
    void droppingTheOldestStreamSparesTheOverlappingReplacement() {
        LoopbackServer server;
        QVERIFY(server.listen(fixtureSecret()));
        server.expectStream(QStringLiteral("/traffic"));

        QWebSocket *retired = openStream(server, QStringLiteral("/traffic"));
        QVERIFY2(server.waitFor([&server] { return server.openStreams(QStringLiteral("/traffic")) == 1; }),
                 qPrintable(server.pendingReport()));
        QWebSocket *replacement = openStream(server, QStringLiteral("/traffic"));
        QVERIFY2(server.waitFor([&server] { return server.openStreams(QStringLiteral("/traffic")) == 2; }),
                 qPrintable(server.pendingReport()));

        QSignalSpy retiredGone(retired, &QWebSocket::disconnected);
        QSignalSpy replacementGone(replacement, &QWebSocket::disconnected);
        QVERIFY(server.dropOldestStream(QStringLiteral("/traffic")));
        QVERIFY2(server.waitFor([&retiredGone] { return retiredGone.size() == 1; }),
                 qPrintable(server.pendingReport()));
        QCOMPARE(server.openStreams(QStringLiteral("/traffic")), 1);
        QCOMPARE(replacementGone.size(), 0);

        // Which one survived: the message goes to whatever socket is left, and
        // it has to arrive on the replacement.
        QSignalSpy messages(replacement, &QWebSocket::textMessageReceived);
        QVERIFY(server.sendText(QStringLiteral("/traffic"), QStringLiteral("after-the-drop")));
        QVERIFY2(server.waitFor([&messages] { return messages.size() == 1; }),
                 qPrintable(server.pendingReport()));
        QCOMPARE(messages.takeFirst().first().toString(), QStringLiteral("after-the-drop"));

        // And with nothing on the path, there is nothing to drop.
        QVERIFY(server.dropOldestStream(QStringLiteral("/traffic")));
        QCOMPARE(server.openStreams(QStringLiteral("/traffic")), 0);
        QVERIFY(!server.dropOldestStream(QStringLiteral("/traffic")));

        retired->abort();
        replacement->abort();
        delete retired;
        delete replacement;
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
