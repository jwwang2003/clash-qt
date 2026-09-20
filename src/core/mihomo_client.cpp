#include "core/mihomo_client.h"

#include <algorithm>
#include <memory>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWebSocket>

namespace core {
namespace {

constexpr auto kLatencyTestUrl = "https://www.gstatic.com/generate_204";
constexpr int kLatencyTimeoutMs = 5000;
constexpr auto kReconnectTimerName = "stream-reconnect";
constexpr int kReconnectMinMs = 1000;
constexpr int kReconnectMaxMs = 30000;

quint16 port(const QJsonObject &object, const QString &key) {
    return static_cast<quint16>(object.value(key).toInt());
}

}  // namespace

MihomoClient::MihomoClient(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

void MihomoClient::setEndpoint(const Endpoint &endpoint) {
    endpoint_ = endpoint;
    if (trafficSocket_) {
        closeTrafficStream();
        openTrafficStream();
    }
    if (connectionsSocket_) {
        closeConnectionsStream();
        openConnectionsStream();
    }
    if (logSocket_) {
        const QString level = logLevel_;
        closeLogStream();
        openLogStream(level);
    }
    if (memorySocket_) {
        closeMemoryStream();
        openMemoryStream();
    }
}

void MihomoClient::applyAuth(QNetworkRequest &request) const {
    if (!endpoint_.secret.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + endpoint_.secret.toUtf8());
    }
}

QNetworkReply *MihomoClient::get(const QString &path) {
    QNetworkRequest request{QUrl(endpoint_.httpBase() + path)};
    applyAuth(request);
    return network_->get(request);
}

void MihomoClient::fetchVersion() {
    QNetworkReply *reply = get("/version");
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Cannot reach controller: %1").arg(reply->errorString()));
            return;
        }
        const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
        emit versionReceived(object.value("version").toString());
    });
}

void MihomoClient::fetchProxies() {
    QNetworkReply *reply = get("/proxies");
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Failed to load proxies: %1").arg(reply->errorString()));
            return;
        }

        const QJsonObject proxies =
            QJsonDocument::fromJson(reply->readAll()).object().value("proxies").toObject();

        QVector<ProxyGroup> groups;
        QHash<QString, ProxyNode> nodes;

        for (auto it = proxies.begin(); it != proxies.end(); ++it) {
            const QJsonObject entry = it.value().toObject();
            const QString type = entry.value("type").toString();

            if (entry.contains("all")) {
                ProxyGroup group;
                group.name = it.key();
                group.type = type;
                group.now = entry.value("now").toString();
                for (const QJsonValue &member : entry.value("all").toArray()) {
                    group.all.append(member.toString());
                }
                groups.append(group);
                continue;
            }

            ProxyNode node;
            node.name = it.key();
            node.type = type;
            // history is append-only; the last entry is the most recent probe.
            if (const QJsonArray history = entry.value("history").toArray(); !history.isEmpty()) {
                node.delay = history.last().toObject().value("delay").toInt(-1);
            }
            nodes.insert(node.name, node);
        }

        // GLOBAL duplicates every other group's members; keep it last rather than first.
        std::sort(groups.begin(), groups.end(), [](const ProxyGroup &a, const ProxyGroup &b) {
            if ((a.name == "GLOBAL") != (b.name == "GLOBAL")) return b.name == "GLOBAL";
            return a.name.localeAwareCompare(b.name) < 0;
        });

        emit proxiesUpdated(groups, nodes);
    });
}

void MihomoClient::selectNode(const QString &group, const QString &node) {
    QNetworkRequest request{QUrl(endpoint_.httpBase() + "/proxies/" + QUrl::toPercentEncoding(group))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyAuth(request);

    const QJsonDocument body{QJsonObject{{"name", node}}};
    QNetworkReply *reply = network_->put(request, body.toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, group, node] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Could not switch %1: %2").arg(group, reply->errorString()));
            return;
        }
        emit nodeSelected(group, node);
        fetchProxies();
    });
}

void MihomoClient::testGroupDelay(const QString &group) {
    QUrl url(endpoint_.httpBase() + "/group/" + QUrl::toPercentEncoding(group) + "/delay");
    QUrlQuery query;
    query.addQueryItem("url", kLatencyTestUrl);
    query.addQueryItem("timeout", QString::number(kLatencyTimeoutMs));
    url.setQuery(query);

    QNetworkRequest request{url};
    applyAuth(request);
    QNetworkReply *reply = network_->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Latency test failed: %1").arg(reply->errorString()));
            return;
        }
        fetchProxies();
    });
}

void MihomoClient::fetchRules() {
    QNetworkReply *reply = get("/rules");
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Failed to load rules: %1").arg(reply->errorString()));
            return;
        }

        const QJsonArray items =
            QJsonDocument::fromJson(reply->readAll()).object().value("rules").toArray();

        QVector<Rule> rules;
        rules.reserve(items.size());
        for (const QJsonValue &item : items) {
            const QJsonObject entry = item.toObject();
            rules.append(Rule{entry.value("type").toString(), entry.value("payload").toString(),
                              entry.value("proxy").toString()});
        }

        emit rulesUpdated(rules);
    });
}

void MihomoClient::fetchConfigs() {
    QNetworkReply *reply = get("/configs");
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Failed to load configuration: %1").arg(reply->errorString()));
            return;
        }

        const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();

        BaseConfig config;
        config.mode = object.value("mode").toString();
        config.logLevel = object.value("log-level").toString();
        config.mixedPort = port(object, "mixed-port");
        config.httpPort = port(object, "port");
        config.socksPort = port(object, "socks-port");
        config.redirPort = port(object, "redir-port");
        config.tproxyPort = port(object, "tproxy-port");
        config.allowLan = object.value("allow-lan").toBool();
        config.ipv6 = object.value("ipv6").toBool();
        config.tunEnabled = object.value("tun").toObject().value("enable").toBool();

        emit configReceived(config);
    });
}

void MihomoClient::patchMode(const QString &mode) {
    QNetworkRequest request{QUrl(endpoint_.httpBase() + "/configs")};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyAuth(request);

    const QJsonDocument body{QJsonObject{{"mode", mode}}};
    QNetworkReply *reply =
        network_->sendCustomRequest(request, "PATCH", body.toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, mode] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(
                tr("Could not switch to %1 mode: %2").arg(mode, reply->errorString()));
            return;
        }
        emit modeChanged(mode);
        fetchConfigs();
    });
}

void MihomoClient::closeConnection(const QString &id) {
    QNetworkRequest request{
        QUrl(endpoint_.httpBase() + "/connections/" + QUrl::toPercentEncoding(id))};
    applyAuth(request);
    QNetworkReply *reply = network_->deleteResource(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Could not close connection: %1").arg(reply->errorString()));
        }
    });
}

void MihomoClient::closeAllConnections() {
    QNetworkRequest request{QUrl(endpoint_.httpBase() + "/connections")};
    applyAuth(request);
    QNetworkReply *reply = network_->deleteResource(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Could not close connections: %1").arg(reply->errorString()));
        }
    });
}

QWebSocket *MihomoClient::openStream(const QString &path,
                                     void (MihomoClient::*handler)(const QString &)) {
    QNetworkRequest request{QUrl(endpoint_.wsBase() + path)};
    applyAuth(request);

    auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    auto *retry = new QTimer(socket);
    retry->setObjectName(kReconnectTimerName);
    retry->setSingleShot(true);

    // mihomo drops every stream when it restarts, so back off and keep retrying.
    const auto backoff = std::make_shared<int>(kReconnectMinMs);
    const auto reconnect = [retry, backoff] {
        if (retry->isActive()) return;
        retry->start(*backoff);
        *backoff = std::min(*backoff * 2, kReconnectMaxMs);
    };

    connect(socket, &QWebSocket::textMessageReceived, this,
            [this, handler](const QString &message) { (this->*handler)(message); });
    connect(socket, &QWebSocket::connected, this, [backoff] { *backoff = kReconnectMinMs; });
    connect(socket, &QWebSocket::disconnected, this, reconnect);
    connect(socket, &QWebSocket::errorOccurred, this,
            [this, socket, path, reconnect](QAbstractSocket::SocketError) {
                emit errorOccurred(tr("Stream %1 failed: %2").arg(path, socket->errorString()));
                reconnect();
            });
    connect(retry, &QTimer::timeout, this, [socket, request] { socket->open(request); });

    socket->open(request);
    return socket;
}

void MihomoClient::closeStream(QWebSocket *&socket) {
    if (!socket) return;
    // the socket outlives this call, so a pending retry would revive a closed stream.
    if (QTimer *retry = socket->findChild<QTimer *>(kReconnectTimerName)) retry->stop();
    socket->disconnect(this);
    socket->close();
    socket->deleteLater();
    socket = nullptr;
}

void MihomoClient::openTrafficStream() {
    closeTrafficStream();
    trafficSocket_ = openStream("/traffic", &MihomoClient::handleTrafficMessage);
}

void MihomoClient::closeTrafficStream() { closeStream(trafficSocket_); }

void MihomoClient::openConnectionsStream() {
    closeConnectionsStream();
    connectionsSocket_ = openStream("/connections", &MihomoClient::handleConnectionsMessage);
}

void MihomoClient::closeConnectionsStream() { closeStream(connectionsSocket_); }

void MihomoClient::openLogStream(const QString &level) {
    closeLogStream();
    logLevel_ = level;

    QUrlQuery query;
    query.addQueryItem("level", logLevel_);
    logSocket_ = openStream("/logs?" + query.toString(), &MihomoClient::handleLogMessage);
}

void MihomoClient::closeLogStream() { closeStream(logSocket_); }

void MihomoClient::openMemoryStream() {
    closeMemoryStream();
    memorySocket_ = openStream("/memory", &MihomoClient::handleMemoryMessage);
}

void MihomoClient::closeMemoryStream() { closeStream(memorySocket_); }

void MihomoClient::handleTrafficMessage(const QString &message) {
    const QJsonObject sample = QJsonDocument::fromJson(message.toUtf8()).object();
    emit trafficSample(static_cast<quint64>(sample.value("up").toDouble()),
                       static_cast<quint64>(sample.value("down").toDouble()));
}

void MihomoClient::handleConnectionsMessage(const QString &message) {
    const QJsonObject snapshot = QJsonDocument::fromJson(message.toUtf8()).object();
    const QJsonArray items = snapshot.value("connections").toArray();

    QVector<Connection> connections;
    connections.reserve(items.size());

    for (const QJsonValue &item : items) {
        const QJsonObject entry = item.toObject();
        const QJsonObject metadata = entry.value("metadata").toObject();

        Connection connection;
        connection.id = entry.value("id").toString();
        connection.upload = static_cast<quint64>(entry.value("upload").toDouble());
        connection.download = static_cast<quint64>(entry.value("download").toDouble());
        connection.rule = entry.value("rule").toString();
        connection.rulePayload = entry.value("rulePayload").toString();
        for (const QJsonValue &hop : entry.value("chains").toArray()) {
            connection.chains.append(hop.toString());
        }

        const QString start = entry.value("start").toString();
        connection.start = QDateTime::fromString(start, Qt::ISODateWithMs);
        if (!connection.start.isValid()) {
            connection.start = QDateTime::fromString(start, Qt::ISODate);
        }

        connection.network = metadata.value("network").toString();
        connection.connectionType = metadata.value("type").toString();
        connection.sourceIp = metadata.value("sourceIP").toString();
        connection.destinationPort = metadata.value("destinationPort").toString();
        connection.process = metadata.value("process").toString();
        connection.host = metadata.value("host").toString();
        if (connection.host.isEmpty()) {
            connection.host = metadata.value("destinationIP").toString();
        }

        connections.append(connection);
    }

    emit connectionsUpdated(connections,
                            static_cast<quint64>(snapshot.value("uploadTotal").toDouble()),
                            static_cast<quint64>(snapshot.value("downloadTotal").toDouble()));
}

void MihomoClient::handleLogMessage(const QString &message) {
    const QJsonObject object = QJsonDocument::fromJson(message.toUtf8()).object();

    LogEntry entry;
    entry.level = object.value("type").toString();
    entry.payload = object.value("payload").toString();
    entry.time = QDateTime::currentDateTime();

    emit logReceived(entry);
}

void MihomoClient::handleMemoryMessage(const QString &message) {
    const QJsonObject sample = QJsonDocument::fromJson(message.toUtf8()).object();
    emit memorySample(static_cast<quint64>(sample.value("inuse").toDouble()),
                      static_cast<quint64>(sample.value("oslimit").toDouble()));
}

}  // namespace core
