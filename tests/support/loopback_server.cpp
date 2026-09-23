#include "support/loopback_server.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <cstring>

namespace testsupport {
namespace {

constexpr auto kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr quint8 kOpContinuation = 0x0;
constexpr quint8 kOpText = 0x1;
constexpr quint8 kOpClose = 0x8;
constexpr quint8 kOpPing = 0x9;
constexpr quint8 kOpPong = 0xA;
constexpr const char *kRedacted = "<redacted>";

QString routeKey(const QByteArray &method, const QString &path) {
    return QString::fromLatin1(method.toUpper()) + QLatin1Char(' ') + path;
}

QByteArray headerValue(const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &name) {
    const QByteArray wanted = name.toLower();
    for (const auto &header : headers)
        if (header.first.toLower() == wanted) return header.second;
    return {};
}

QByteArray statusText(int status) {
    switch (status) {
    case 200: return QByteArrayLiteral("OK");
    case 204: return QByteArrayLiteral("No Content");
    case 400: return QByteArrayLiteral("Bad Request");
    case 401: return QByteArrayLiteral("Unauthorized");
    case 403: return QByteArrayLiteral("Forbidden");
    case 404: return QByteArrayLiteral("Not Found");
    case 500: return QByteArrayLiteral("Internal Server Error");
    case 501: return QByteArrayLiteral("Not Implemented");
    case 503: return QByteArrayLiteral("Service Unavailable");
    default: return QByteArrayLiteral("Status");
    }
}

QByteArray buildFrame(quint8 opcode, const QByteArray &payload, bool fin) {
    QByteArray frame;
    frame.append(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
    const qsizetype size = payload.size();
    if (size < 126) {
        frame.append(static_cast<char>(size));
    } else if (size <= 0xFFFF) {
        frame.append(static_cast<char>(126));
        char length[2];
        qToBigEndian<quint16>(static_cast<quint16>(size), length);
        frame.append(length, 2);
    } else {
        frame.append(static_cast<char>(127));
        char length[8];
        qToBigEndian<quint64>(static_cast<quint64>(size), length);
        frame.append(length, 8);
    }
    frame.append(payload);  // server frames are never masked
    return frame;
}

// Split a UTF-8 payload into `parts` pieces without cutting a code point.
QList<QByteArray> splitUtf8(const QByteArray &payload, int parts) {
    QList<QByteArray> chunks;
    if (parts < 1) parts = 1;
    qsizetype offset = 0;
    for (int index = 0; index < parts && offset < payload.size(); ++index) {
        qsizetype remaining = parts - index;
        qsizetype length = (payload.size() - offset + remaining - 1) / remaining;
        qsizetype end = offset + length;
        while (end < payload.size() && (static_cast<quint8>(payload[end]) & 0xC0) == 0x80) ++end;
        chunks.append(payload.mid(offset, end - offset));
        offset = end;
    }
    if (offset < payload.size()) chunks.append(payload.mid(offset));
    while (chunks.size() < parts) chunks.append(QByteArray());
    return chunks;
}

} // namespace

struct LoopbackServer::Connection {
    QTcpSocket *socket = nullptr;
    QByteArray buffer;
    bool websocket = false;
    bool closing = false;
    QString streamPath;
    quint64 streamSerial = 0;  // handshake order; 0 until the upgrade succeeds
    quint8 fragmentOpcode = 0;
    QByteArray fragmentPayload;
};

LoopbackServer::Reply LoopbackServer::Reply::json(const QByteArray &body) {
    Reply reply;
    reply.body = body;
    return reply;
}

LoopbackServer::Reply LoopbackServer::Reply::failure(int status, const QByteArray &body) {
    Reply reply;
    reply.status = status;
    reply.body = body.isEmpty() ? QByteArrayLiteral("{\"message\":\"scripted failure\"}") : body;
    return reply;
}

LoopbackServer::Reply LoopbackServer::Reply::drop() {
    Reply reply;
    reply.dropConnection = true;
    return reply;
}

LoopbackServer::Reply LoopbackServer::Reply::document(const QByteArray &contentType,
                                                      const QByteArray &body) {
    Reply reply;
    reply.contentType = contentType;
    reply.body = body;
    return reply;
}

bool LoopbackServer::Reply::isReservedHeader(const QByteArray &name) {
    const QByteArray lowered = name.trimmed().toLower();
    return lowered == "content-type" || lowered == "content-length" || lowered == "connection";
}

LoopbackServer::Reply LoopbackServer::Reply::withHeader(const QByteArray &name,
                                                        const QByteArray &value) const {
    Reply reply = *this;
    if (name.trimmed().isEmpty() || isReservedHeader(name)) return reply;
    reply.headers.append({name, value});
    return reply;
}

LoopbackServer::LoopbackServer(QObject *parent) : QObject(parent) {
    connect(&listener_, &QTcpServer::newConnection, this, &LoopbackServer::accept);
}

LoopbackServer::~LoopbackServer() { close(); }

bool LoopbackServer::listen(const QString &secret) {
    secret_ = secret;
    return listener_.listen(QHostAddress::LocalHost, 0);
}

void LoopbackServer::close() {
    listener_.close();
    const auto sockets = connections_.keys();
    for (QTcpSocket *socket : sockets) {
        Connection *connection = connections_.take(socket);
        if (connection && connection->websocket) emit streamClosed(connection->streamPath);
        delete connection;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    connections_.clear();
    // Let the queued deleteLater() calls run so nothing outlives the fixture.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QString LoopbackServer::host() const { return QStringLiteral("127.0.0.1"); }
quint16 LoopbackServer::port() const { return listener_.serverPort(); }
QString LoopbackServer::secret() const { return secret_; }
QString LoopbackServer::httpBase() const {
    return QStringLiteral("http://%1:%2").arg(host()).arg(port());
}
QString LoopbackServer::wsBase() const {
    return QStringLiteral("ws://%1:%2").arg(host()).arg(port());
}

void LoopbackServer::route(const QByteArray &method, const QString &path, const Reply &reply) {
    routes_.insert(routeKey(method, path), reply);
}

LoopbackGate::LoopbackGate(LoopbackServer *server) : QObject(server), server_(server) {}

LoopbackGate *LoopbackServer::hold(const QByteArray &method, const QString &path) {
    const QString key = routeKey(method, path);
    if (LoopbackGate *existing = gates_.value(key)) return existing;
    auto *gate = new LoopbackGate(this);
    gates_.insert(key, gate);
    return gate;
}

void LoopbackServer::expectStream(const QString &path) {
    if (!expectedStreams_.contains(path)) expectedStreams_.append(path);
}

void LoopbackServer::accept() {
    while (QTcpSocket *socket = listener_.nextPendingConnection()) {
        auto *connection = new Connection;
        connection->socket = socket;
        connections_.insert(socket, connection);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readFrom(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            Connection *closing = connections_.take(socket);
            if (closing && closing->websocket) emit streamClosed(closing->streamPath);
            delete closing;
            socket->deleteLater();
        });
    }
}

void LoopbackServer::readFrom(QTcpSocket *socket) {
    Connection *connection = connections_.value(socket);
    if (!connection) return;
    connection->buffer.append(socket->readAll());
    if (connection->websocket) {
        consumeFrames(*connection);
        return;
    }
    // Every step can destroy the connection (a dropped reply, a protocol
    // error), so the map is re-consulted instead of reusing the pointer.
    while ((connection = connections_.value(socket)) && !connection->websocket &&
           consumeHttp(*connection)) {
    }
    connection = connections_.value(socket);
    if (connection && connection->websocket) consumeFrames(*connection);
}

bool LoopbackServer::consumeHttp(Connection &connection) {
    const qsizetype headerEnd = connection.buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) return false;

    const QList<QByteArray> lines = connection.buffer.left(headerEnd).split('\n');
    if (lines.isEmpty()) return false;
    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    if (requestLine.size() < 2) return false;

    RecordedRequest request;
    request.method = requestLine.at(0).toUpper();
    request.target = QString::fromUtf8(requestLine.at(1));
    const QUrl url(request.target);
    request.path = url.path();
    request.query = QUrlQuery(url.query());

    qint64 contentLength = 0;
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const QByteArray line = lines.at(index).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) continue;
        const QByteArray name = line.left(colon).trimmed();
        const QByteArray value = line.mid(colon + 1).trimmed();
        request.headers.append({name, value});
        if (name.toLower() == "content-length") contentLength = value.toLongLong();
    }
    if (connection.buffer.size() < headerEnd + 4 + contentLength) return false;
    request.body = connection.buffer.mid(headerEnd + 4, contentLength);
    connection.buffer.remove(0, headerEnd + 4 + contentLength);

    const QByteArray authorization = headerValue(request.headers, "Authorization");
    request.authenticated = secret_.isEmpty() ||
                            authorization == "Bearer " + secret_.toUtf8() ||
                            request.query.queryItemValue(QStringLiteral("token")) == secret_;

    QTcpSocket *socket = connection.socket;
    dispatch(connection, std::move(request));
    // `connection` may have been destroyed by dispatch(); never touch it again.
    const Connection *current = connections_.value(socket);
    return current && !current->websocket && !current->closing;
}

void LoopbackServer::record(RecordedRequest request) {
    if (!request.authenticated) unauthenticated_ = true;
    requests_.append(request);
    emit requestRecorded(request);
}

void LoopbackServer::dispatch(Connection &connection, RecordedRequest request) {
    if (headerValue(request.headers, "Upgrade").toLower() == "websocket") {
        request.method = QByteArrayLiteral("WS");
        upgradeToStream(connection, request);
        return;
    }

    const QString key = routeKey(request.method, request.path);
    const bool clientWantsClose = headerValue(request.headers, "Connection").toLower() == "close";
    if (!request.authenticated) {
        request.scripted = routes_.contains(key) || gates_.contains(key);
        record(request);
        respond(connection.socket, Reply::failure(401, QByteArrayLiteral("{\"message\":\"Unauthorized\"}")),
                clientWantsClose);
        return;
    }

    if (LoopbackGate *gate = gates_.value(key)) {
        request.scripted = true;
        record(request);
        gate->held_.append({connection.socket, request});
        emit gate->pendingChanged(gate->pending());
        return;
    }

    if (routes_.contains(key)) {
        request.scripted = true;
        record(request);
        respond(connection.socket, routes_.value(key), clientWantsClose);
        return;
    }

    request.scripted = false;
    unexpected_ = true;
    record(request);
    emit unexpectedRequest(request.method, request.path);
    respond(connection.socket,
            Reply::failure(501, QByteArrayLiteral("{\"message\":\"unscripted request; the fixture refuses "
                                                  "to invent a reply\"}")),
            clientWantsClose);
}

void LoopbackServer::respond(QTcpSocket *socket, const Reply &reply, bool clientWantsClose) {
    Connection *connection = connections_.value(socket);
    if (!socket || !connection) return;
    if (reply.dropConnection) {
        drop(socket);
        return;
    }
    // HTTP/1.1 keep-alive is honoured. Closing after every reply loses any
    // request QNetworkAccessManager had already written on a pooled connection,
    // which shows up as a rare vanished request rather than a clear failure.
    const bool close = reply.closeConnection || clientWantsClose;
    QByteArray extra;
    for (const auto &header : reply.headers) {
        // Re-checked here, not only in withHeader(): `Reply` is a plain struct
        // and a caller may append to `headers` directly.
        if (Reply::isReservedHeader(header.first)) continue;
        extra += header.first.trimmed() + ": " + header.second + "\r\n";
    }
    const QByteArray response = "HTTP/1.1 " + QByteArray::number(reply.status) + " " +
                                statusText(reply.status) + "\r\nContent-Type: " + reply.contentType +
                                "\r\nContent-Length: " + QByteArray::number(reply.body.size()) +
                                "\r\nConnection: " + (close ? "close" : "keep-alive") + "\r\n" +
                                extra + "\r\n" + reply.body;
    if (close) connection->closing = true;
    socket->write(response);
    socket->flush();
    if (close) socket->disconnectFromHost();
}

void LoopbackServer::upgradeToStream(Connection &connection, const RecordedRequest &incoming) {
    RecordedRequest request = incoming;
    const QByteArray key = headerValue(request.headers, "Sec-WebSocket-Key");
    const QByteArray version = headerValue(request.headers, "Sec-WebSocket-Version");

    if (!request.authenticated) {
        request.scripted = expectedStreams_.contains(request.path);
        record(request);
        respond(connection.socket, Reply::failure(401, QByteArrayLiteral("{\"message\":\"Unauthorized\"}")), true);
        return;
    }
    if (key.isEmpty() || version != "13") {
        record(request);
        respond(connection.socket, Reply::failure(400, QByteArrayLiteral("{\"message\":\"bad handshake\"}")), true);
        return;
    }
    if (!expectedStreams_.contains(request.path)) {
        unexpected_ = true;
        record(request);
        emit unexpectedRequest(request.method, request.path);
        respond(connection.socket,
                Reply::failure(501, QByteArrayLiteral("{\"message\":\"unexpected stream\"}")), true);
        return;
    }

    request.scripted = true;
    record(request);

    const QByteArray accept =
        QCryptographicHash::hash(key + kWebSocketGuid, QCryptographicHash::Sha1).toBase64();
    connection.socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                             "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
    connection.websocket = true;
    connection.streamPath = request.path;
    connection.streamSerial = ++nextStreamSerial_;
    handshakes_[request.path] = handshakes_.value(request.path) + 1;
    emit streamOpened(request.path);
}

void LoopbackServer::consumeFrames(Connection &connection) {
    QByteArray &buffer = connection.buffer;
    while (buffer.size() >= 2) {
        const quint8 first = static_cast<quint8>(buffer.at(0));
        const quint8 second = static_cast<quint8>(buffer.at(1));
        const bool fin = first & 0x80;
        const quint8 opcode = first & 0x0F;
        const bool masked = second & 0x80;
        quint64 length = second & 0x7F;
        qsizetype offset = 2;
        if (length == 126) {
            if (buffer.size() < offset + 2) return;
            length = qFromBigEndian<quint16>(buffer.constData() + offset);
            offset += 2;
        } else if (length == 127) {
            if (buffer.size() < offset + 8) return;
            length = qFromBigEndian<quint64>(buffer.constData() + offset);
            offset += 8;
        }
        char mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (buffer.size() < offset + 4) return;
            memcpy(mask, buffer.constData() + offset, 4);
            offset += 4;
        }
        if (static_cast<quint64>(buffer.size()) < static_cast<quint64>(offset) + length) return;
        if (!masked) {  // RFC 6455: every client-to-server frame must be masked.
            drop(connection.socket);
            return;
        }
        QByteArray payload = buffer.mid(offset, static_cast<qsizetype>(length));
        buffer.remove(0, offset + static_cast<qsizetype>(length));
        for (qsizetype index = 0; index < payload.size(); ++index)
            payload[index] = payload[index] ^ mask[index % 4];

        if (opcode == kOpClose) {
            connection.socket->write(buildFrame(kOpClose, payload.left(2), true));
            connection.socket->disconnectFromHost();
            return;
        }
        if (opcode == kOpPing) {
            connection.socket->write(buildFrame(kOpPong, payload, true));
            continue;
        }
        if (opcode == kOpPong) continue;

        if (opcode == kOpContinuation) {
            connection.fragmentPayload.append(payload);
        } else {
            connection.fragmentOpcode = opcode;
            connection.fragmentPayload = payload;
        }
        if (!fin) continue;
        if (connection.fragmentOpcode == kOpText)
            emit streamMessage(connection.streamPath, QString::fromUtf8(connection.fragmentPayload));
        connection.fragmentPayload.clear();
        connection.fragmentOpcode = 0;
    }
}

void LoopbackServer::drop(QTcpSocket *socket) {
    if (!socket) return;
    Connection *connection = connections_.take(socket);
    if (connection && connection->websocket) emit streamClosed(connection->streamPath);
    delete connection;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

QList<QTcpSocket *> LoopbackServer::streamSockets(const QString &path) const {
    QList<QTcpSocket *> sockets;
    for (auto it = connections_.cbegin(); it != connections_.cend(); ++it)
        if (it.value()->websocket && it.value()->streamPath == path) sockets.append(it.key());
    return sockets;
}

int LoopbackServer::openStreams(const QString &path) const {
    return static_cast<int>(streamSockets(path).size());
}

int LoopbackServer::streamHandshakes(const QString &path) const { return handshakes_.value(path); }

bool LoopbackServer::sendText(const QString &path, const QString &message) {
    const auto sockets = streamSockets(path);
    for (QTcpSocket *socket : sockets) {
        socket->write(buildFrame(kOpText, message.toUtf8(), true));
        socket->flush();
    }
    return !sockets.isEmpty();
}

bool LoopbackServer::sendFragmentedText(const QString &path, const QString &message, int fragments) {
    const auto sockets = streamSockets(path);
    const auto chunks = splitUtf8(message.toUtf8(), fragments);
    for (QTcpSocket *socket : sockets) {
        for (qsizetype index = 0; index < chunks.size(); ++index) {
            const bool first = index == 0;
            const bool last = index == chunks.size() - 1;
            socket->write(buildFrame(first ? kOpText : kOpContinuation, chunks.at(index), last));
            socket->flush();
        }
    }
    return !sockets.isEmpty();
}

bool LoopbackServer::closeStream(const QString &path, quint16 code) {
    const auto sockets = streamSockets(path);
    char raw[2];
    qToBigEndian<quint16>(code, raw);
    for (QTcpSocket *socket : sockets) {
        socket->write(buildFrame(kOpClose, QByteArray(raw, 2), true));
        socket->flush();
        socket->disconnectFromHost();
    }
    return !sockets.isEmpty();
}

bool LoopbackServer::dropStream(const QString &path) {
    const auto sockets = streamSockets(path);
    for (QTcpSocket *socket : sockets) drop(socket);
    return !sockets.isEmpty();
}

bool LoopbackServer::dropOldestStream(const QString &path) {
    QTcpSocket *oldest = nullptr;
    quint64 serial = 0;
    for (auto it = connections_.cbegin(); it != connections_.cend(); ++it) {
        const Connection *connection = it.value();
        if (!connection->websocket || connection->streamPath != path) continue;
        if (oldest && connection->streamSerial >= serial) continue;
        oldest = it.key();
        serial = connection->streamSerial;
    }
    if (!oldest) return false;
    drop(oldest);
    return true;
}

QList<RecordedRequest> LoopbackServer::requests() const { return requests_; }

int LoopbackServer::requestCount(const QByteArray &method, const QString &path) const {
    int count = 0;
    const QByteArray wanted = method.toUpper();
    for (const RecordedRequest &request : requests_)
        if (request.method == wanted && request.path == path) ++count;
    return count;
}

bool LoopbackServer::sawUnexpectedRequest() const { return unexpected_; }
bool LoopbackServer::sawUnauthenticatedRequest() const { return unauthenticated_; }

void LoopbackServer::clearRequests() {
    requests_.clear();
    unexpected_ = false;
    unauthenticated_ = false;
}

QString LoopbackServer::redact(const QString &text) const {
    QString out = text;
    if (!secret_.isEmpty()) out.replace(secret_, QLatin1String(kRedacted));
    static const QRegularExpression bearer(QStringLiteral("(?i)bearer\\s+\\S+"));
    static const QRegularExpression token(QStringLiteral("(?i)token=[^&\\s]+"));
    out.replace(bearer, QStringLiteral("Bearer <redacted>"));
    out.replace(token, QStringLiteral("token=<redacted>"));
    return out;
}

QString LoopbackServer::redactedTranscript() const {
    QStringList lines;
    for (const RecordedRequest &request : requests_) {
        QString line = QString::fromLatin1(request.method) + QLatin1Char(' ') + request.target +
                       QStringLiteral(" auth=") + (request.authenticated ? QStringLiteral("ok")
                                                                         : QStringLiteral("REJECTED")) +
                       QStringLiteral(" scripted=") + (request.scripted ? QStringLiteral("yes")
                                                                        : QStringLiteral("NO"));
        if (!request.body.isEmpty())
            line += QStringLiteral(" body=") + QString::fromUtf8(request.body.left(200));
        lines.append(redact(line));
    }
    return lines.join(QLatin1Char('\n'));
}

QString LoopbackServer::pendingReport() const {
    QStringList lines;
    for (auto it = gates_.cbegin(); it != gates_.cend(); ++it)
        if (it.value()->pending() > 0)
            lines.append(QStringLiteral("held: %1 x%2").arg(it.key()).arg(it.value()->pending()));
    for (auto it = handshakes_.cbegin(); it != handshakes_.cend(); ++it)
        lines.append(QStringLiteral("stream %1: %2 open, %3 handshakes")
                         .arg(it.key()).arg(openStreams(it.key())).arg(it.value()));
    lines.append(QStringLiteral("requests (%1):").arg(requests_.size()));
    lines.append(redactedTranscript());
    return lines.join(QLatin1Char('\n'));
}

bool LoopbackServer::waitFor(const std::function<bool()> &predicate, int timeoutMs) {
    QDeadlineTimer deadline(timeoutMs);
    while (!predicate()) {
        if (deadline.hasExpired()) return predicate();
        // Runs the real event loop between checks instead of spinning on the CPU.
        QEventLoop loop;
        QTimer::singleShot(2, &loop, &QEventLoop::quit);
        loop.exec(QEventLoop::AllEvents);
    }
    return true;
}

bool LoopbackGate::waitForPending(int count, int timeoutMs) {
    return server_->waitFor([this, count] { return pending() >= count; }, timeoutMs);
}

void LoopbackGate::release() {
    while (!held_.isEmpty()) releaseOne();
}

void LoopbackGate::releaseOne() {
    if (held_.isEmpty()) return;
    const Held held = held_.takeFirst();
    ++released_;
    emit pendingChanged(pending());
    if (!held.socket) return;
    const QString key = QString::fromLatin1(held.request.method) + QLatin1Char(' ') + held.request.path;
    bool clientWantsClose = false;
    for (const auto &header : held.request.headers)
        if (header.first.toLower() == "connection") clientWantsClose = header.second.toLower() == "close";
    if (server_->routes_.contains(key)) {
        server_->respond(held.socket, server_->routes_.value(key), clientWantsClose);
        return;
    }
    server_->unexpected_ = true;
    emit server_->unexpectedRequest(held.request.method, held.request.path);
    server_->respond(held.socket,
                     LoopbackServer::Reply::failure(501, QByteArrayLiteral("{\"message\":\"gate released with no "
                                                                           "scripted route\"}")),
                     clientWantsClose);
}

} // namespace testsupport
