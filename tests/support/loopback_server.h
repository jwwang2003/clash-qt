// A loopback HTTP + WebSocket fixture for the mihomo controller contracts.
//
// It is *not* a second mihomo: it serves only the replies a test scripts. An
// unscripted request is answered 501 and recorded as unexpected, so missing
// coverage fails loudly instead of passing on an empty `{}` body.
//
// HTTP and WebSocket share one ephemeral loopback port, because production code
// derives both bases from a single Endpoint. The WebSocket side is implemented
// against RFC 6455 directly rather than through QWebSocketServer, so that tests
// can fragment frames, refuse a handshake and abort a live stream - none of
// which QWebSocket exposes on the server side.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QUrlQuery>

#include <functional>

class QTcpSocket;

namespace testsupport {

struct RecordedRequest {
    QByteArray method;   // "GET", "POST", ... or "WS" for a WebSocket handshake
    QString target;      // path plus query, exactly as received
    QString path;        // path only
    QUrlQuery query;
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    bool authenticated = false;  // presented the expected bearer token or ?token=
    bool scripted = false;       // matched a route, a gate or an expected stream
};

class LoopbackServer;

// A held route: matching requests are parked until the test releases them.
// This replaces "sleep and hope" - the test can observe pending state, assert
// on it, release, and then await completion with a deadline.
class LoopbackGate : public QObject {
    Q_OBJECT
public:
    int pending() const { return static_cast<int>(held_.size()); }
    int released() const { return released_; }
    bool waitForPending(int count = 1, int timeoutMs = 5000);
    void release();     // release every request held right now
    void releaseOne();  // release the oldest held request

signals:
    void pendingChanged(int pending);

private:
    friend class LoopbackServer;
    explicit LoopbackGate(LoopbackServer *server);
    struct Held {
        QPointer<QTcpSocket> socket;
        RecordedRequest request;
    };
    LoopbackServer *server_ = nullptr;
    QList<Held> held_;
    int released_ = 0;
};

class LoopbackServer : public QObject {
    Q_OBJECT
public:
    struct Reply {
        int status = 200;
        QByteArray contentType = QByteArrayLiteral("application/json");
        QByteArray body;
        bool closeConnection = false;  // send the reply, then close
        bool dropConnection = false;   // send nothing, abort the socket

        static Reply json(const QByteArray &body);
        static Reply failure(int status, const QByteArray &body = {});
        static Reply drop();
    };

    // A held route (see LoopbackGate, below).
    using Gate = LoopbackGate;

    explicit LoopbackServer(QObject *parent = nullptr);
    ~LoopbackServer() override;

    // Binds 127.0.0.1 on an ephemeral port. An empty secret disables auth checks.
    bool listen(const QString &secret = QStringLiteral("fixture-secret"));
    // Deterministic teardown: aborts every live socket and stops listening.
    void close();

    QString host() const;
    quint16 port() const;
    QString secret() const;
    QString httpBase() const;  // http://127.0.0.1:<port>
    QString wsBase() const;    // ws://127.0.0.1:<port>

    // --- HTTP scripting -----------------------------------------------------
    void route(const QByteArray &method, const QString &path, const Reply &reply);
    // Parks matching requests instead of replying. Release sends the reply that
    // route() registered for the same key; if none is registered the release is
    // answered 501 and recorded as unexpected, so a half-scripted gate fails.
    LoopbackGate *hold(const QByteArray &method, const QString &path);

    // --- WebSocket scripting ------------------------------------------------
    // A handshake for an unexpected path is refused with 501 and recorded.
    void expectStream(const QString &path);
    int openStreams(const QString &path) const;
    int streamHandshakes(const QString &path) const;  // counts reconnects
    bool sendText(const QString &path, const QString &message);
    // Splits one logical message across `fragments` WebSocket frames.
    bool sendFragmentedText(const QString &path, const QString &message, int fragments);
    bool closeStream(const QString &path, quint16 code = 1000);  // orderly close frame
    bool dropStream(const QString &path);                        // abort, no close frame

    // --- Observation --------------------------------------------------------
    QList<RecordedRequest> requests() const;
    int requestCount(const QByteArray &method, const QString &path) const;
    bool sawUnexpectedRequest() const;
    bool sawUnauthenticatedRequest() const;
    // Credentials are replaced with <redacted>; safe to print on failure.
    QString redactedTranscript() const;
    void clearRequests();

    // Spins the event loop until `predicate` holds or the deadline passes.
    // No sleeps: it returns as soon as the predicate is true.
    bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 5000);
    // Pending gates, open streams and the redacted transcript, for a failure message.
    QString pendingReport() const;

signals:
    void requestRecorded(const testsupport::RecordedRequest &request);
    void unexpectedRequest(const QByteArray &method, const QString &path);
    void streamOpened(const QString &path);
    void streamClosed(const QString &path);
    void streamMessage(const QString &path, const QString &message);

private:
    friend class LoopbackGate;
    struct Connection;

    void accept();
    void readFrom(QTcpSocket *socket);
    bool consumeHttp(Connection &connection);
    void dispatch(Connection &connection, RecordedRequest request);
    void respond(QTcpSocket *socket, const Reply &reply, bool clientWantsClose);
    void upgradeToStream(Connection &connection, const RecordedRequest &request);
    void consumeFrames(Connection &connection);
    void drop(QTcpSocket *socket);
    QList<QTcpSocket *> streamSockets(const QString &path) const;
    QString redact(const QString &text) const;
    void record(RecordedRequest request);

    QTcpServer listener_;
    QString secret_;
    QHash<QTcpSocket *, Connection *> connections_;
    QHash<QString, Reply> routes_;            // key: "METHOD /path"
    QHash<QString, LoopbackGate *> gates_;    // key: "METHOD /path"
    QStringList expectedStreams_;
    QHash<QString, int> handshakes_;          // path -> handshake count
    QList<RecordedRequest> requests_;
    bool unexpected_ = false;
    bool unauthenticated_ = false;
};

} // namespace testsupport

Q_DECLARE_METATYPE(testsupport::RecordedRequest)
