#include "core/mihomo/mihomo_client.h"

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

Endpoint MihomoClient::detachedEndpoint() {
    Endpoint endpoint;
    endpoint.host.clear();
    endpoint.port = 0;
    endpoint.secret.clear();
    return endpoint;
}

void MihomoClient::setEndpoint(const Endpoint &endpoint) {
    if (endpoint_.host == endpoint.host && endpoint_.port == endpoint.port &&
        endpoint_.secret == endpoint.secret) {
        // The same controller ADDRESS, and a new session on it: a reload rebinds
        // the replacement engine to the port the retired one held, so every
        // reply still owed by that address is owed by a process that has gone.
        // Left "current", those replies land AFTER the replacement has answered
        // - a refused connection then calls setConnected(false) and clears the
        // live view the new session just populated, and the UI reports a healthy
        // engine as disconnected until the next poll.
        //
        // The REQUESTS moved on, not the attachment, so requestEpoch_ is what
        // advances: the endpoint is unchanged and publishing endpointChanged for
        // it would be a lie. Bump, announce, THEN abort, for the reason spelled
        // out below - and finish any outstanding TUN change immediately after
        // invalidating(), which is what lets a consumer classify the
        // cancellation as a supersession instead of matching on its text.
        ++requestEpoch_;
        emit invalidating();
        finishTunChange(lastTunEnabled_,
                        tr("TUN change cancelled because the controller was replaced."));
        lastTunEnabled_ = false;
        for (auto *reply : network_->findChildren<QNetworkReply *>())
            if (reply->isRunning()) reply->abort();
        // Neither connected_ nor the live view is cleared: the replacement is
        // reachable at the address we are already on, and refreshState() - the
        // re-issue this path owes - is what confirms or denies it. The streams
        // are left to their own reconnect, which re-dials the same address.
        refreshState();
        return;
    }
    ++endpointEpoch_;
    // Bump, announce, THEN abort. finished() may run synchronously inside
    // abort(), so an observer told afterwards would already have accepted the
    // very reply the bump was meant to discard.
    emit invalidating();
    finishTunChange(lastTunEnabled_, tr("TUN change cancelled because the controller changed."));
    lastTunEnabled_ = false;
    endpoint_ = endpoint;
    // Abort after changing the epoch: finished() may run synchronously in abort().
    for (auto *reply : network_->findChildren<QNetworkReply *>())
        if (reply->isRunning()) reply->abort();
    setConnected(false);
    clearLiveState();
    emit endpointChanged();
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
    refreshState();
}

void MihomoClient::detach() {
    ++endpointEpoch_;
    emit invalidating();
    finishTunChange(lastTunEnabled_, tr("TUN change cancelled because the controller changed."));
    lastTunEnabled_ = false;
    endpoint_ = detachedEndpoint();
    for (auto *reply : network_->findChildren<QNetworkReply *>())
        if (reply->isRunning()) reply->abort();
    setConnected(false);
    clearLiveState();
    closeTrafficStream();
    closeConnectionsStream();
    closeLogStream();
    closeMemoryStream();
    emit endpointChanged();
}

void MihomoClient::refreshState() {
    fetchVersion();
    fetchProxies();
    fetchRules();
    fetchConfigs();
}

void MihomoClient::clearLiveState() {
    // Never attributed to whichever reply handler happens to be running: the
    // cleared live state answers no request, and a consumer must see it as the
    // unsolicited publication it is.
    ReplyScope scope(this, 0);
    emit proxiesUpdated({}, {});
    emit rulesUpdated({});
    emit connectionsUpdated({}, 0, 0);
    emit trafficSample(0, 0);
    emit memorySample(0, 0);
}

void MihomoClient::setConnected(bool connected) {
    if (connected_ == connected) return;
    connected_ = connected;
    if (!connected) {
        emit invalidating();
        finishTunChange(lastTunEnabled_, tr("TUN change cancelled because the controller disconnected. Its state could not be confirmed."));
        lastTunEnabled_ = false;
        // An old in-flight snapshot must not repopulate the just-cleared offline UI.
        ++requestEpoch_;
        for (auto *reply : network_->findChildren<QNetworkReply *>())
            if (reply->isRunning()) reply->abort();
        clearLiveState();
    }
    emit connectedChanged(connected);
}

QNetworkReply *MihomoClient::trackReply(QNetworkReply *reply) {
    reply->setProperty("endpointEpoch", QVariant::fromValue(endpointEpoch_));
    reply->setProperty("requestEpoch", QVariant::fromValue(requestEpoch_));
    return reply;
}

bool MihomoClient::isCurrentReply(QNetworkReply *reply) const {
    return reply->property("endpointEpoch").toULongLong() == endpointEpoch_ &&
           reply->property("requestEpoch").toULongLong() == requestEpoch_;
}

void MihomoClient::applyAuth(QNetworkRequest &request) const {
    request.setTransferTimeout(10000);
    if (!endpoint_.secret.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + endpoint_.secret.toUtf8());
    }
}

quint64 MihomoClient::beginOperation() { return ++operation_; }

void MihomoClient::settle(quint64 operation, bool superseded, const QString &error) {
    if (operation == 0) return;
    emit requestSettled(operation, superseded, error);
}

QNetworkReply *MihomoClient::get(const QString &path) {
    QNetworkRequest request{QUrl(endpoint_.httpBase() + path)};
    applyAuth(request);
    return trackReply(network_->get(request));
}

quint64 MihomoClient::fetchVersion() {
    const quint64 operation = beginOperation();
    QNetworkReply *reply = get("/version");
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = tr("Cannot reach controller: %1").arg(reply->errorString());
            setConnected(false);
            emit errorOccurred(error);
            settle(operation, false, error);
            return;
        }
        const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
        const QString version = object.value("version").toString();
        if (version.isEmpty()) {
            const QString error = tr("Controller returned an invalid version response");
            setConnected(false);
            emit errorOccurred(error);
            settle(operation, false, error);
            return;
        }
        setConnected(true);
        emit versionReceived(version);
        settle(operation, false, {});
    });
    return operation;
}

quint64 MihomoClient::fetchProxies() {
    const quint64 operation = beginOperation();
    QNetworkReply *reply = get("/proxies");
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = tr("Failed to load proxies: %1").arg(reply->errorString());
            emit errorOccurred(error);
            settle(operation, false, error);
            return;
        }

        const auto document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject() || !document.object().value("proxies").isObject()) {
            const QString error = tr("Controller returned an invalid proxy response");
            emit errorOccurred(error);
            settle(operation, false, error);
            return;
        }
        const QJsonObject proxies = document.object().value("proxies").toObject();

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
                group.fixed = entry.value("fixed").toString();
                for (const QJsonValue &member : entry.value("all").toArray()) {
                    group.all.append(member.toString());
                }
                groups.append(group);
            }

            // Groups can themselves be members of other groups. Keep their type and delay.
            ProxyNode node;
            node.name = it.key();
            node.type = type;
            // history is append-only; the last entry is the most recent probe.
            if (const QJsonArray history = entry.value("history").toArray(); !history.isEmpty()) {
                node.delay = history.last().toObject().value("delay").toInt(-1);
            }
            nodes.insert(node.name, node);
        }

        // GLOBAL's members retain the configured order even though /proxies is a
        // JSON object. Ordinary node names in this list simply leave gaps in the
        // group ranks. Keep the first occurrence, and show GLOBAL itself last.
        const QJsonArray globalOrder = proxies.value("GLOBAL").toObject().value("all").toArray();
        QHash<QString, qsizetype> groupOrder;
        for (qsizetype i = 0; i < globalOrder.size(); ++i) {
            const QString name = globalOrder.at(i).toString();
            if (!groupOrder.contains(name)) groupOrder.insert(name, i);
        }
        std::sort(groups.begin(), groups.end(), [&groupOrder, &globalOrder](const ProxyGroup &a,
                                                                          const ProxyGroup &b) {
            if ((a.name == "GLOBAL") != (b.name == "GLOBAL")) return b.name == "GLOBAL";
            const qsizetype first = groupOrder.value(a.name, globalOrder.size());
            const qsizetype second = groupOrder.value(b.name, globalOrder.size());
            if (first != second) return first < second;
            // Unlisted groups follow listed groups, with a stable fallback if
            // GLOBAL is absent or the locale collates distinct names equally.
            const int compared = a.name.localeAwareCompare(b.name);
            return compared != 0 ? compared < 0 : a.name < b.name;
        });

        emit proxiesUpdated(groups, nodes);
        settle(operation, false, {});
    });
    return operation;
}

quint64 MihomoClient::selectNode(const QString &group, const QString &node) {
    const quint64 operation = beginOperation();
    QNetworkRequest request{QUrl(endpoint_.httpBase() + "/proxies/" + QUrl::toPercentEncoding(group))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyAuth(request);

    const QJsonDocument body{QJsonObject{{"name", node}}};
    QNetworkReply *reply = trackReply(network_->put(request, body.toJson(QJsonDocument::Compact)));

    connect(reply, &QNetworkReply::finished, this, [this, reply, group, node, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = tr("Could not switch %1: %2").arg(group, reply->errorString());
            emit errorOccurred(error);
            settle(operation, false, error);
            fetchProxies();
            return;
        }
        emit nodeSelected(group, node);
        settle(operation, false, {});
        fetchProxies();
    });
    return operation;
}

quint64 MihomoClient::resetGroupSelection(const QString &group) {
    const quint64 operation = beginOperation();
    QNetworkRequest request{QUrl(endpoint_.httpBase() + "/proxies/" + QUrl::toPercentEncoding(group))};
    applyAuth(request);
    QNetworkReply *reply = trackReply(network_->deleteResource(request));
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        QString error;
        if (reply->error() != QNetworkReply::NoError) {
            error = tr("Could not restore automatic selection: %1").arg(reply->errorString());
            emit errorOccurred(error);
        }
        settle(operation, false, error);
        fetchProxies();
    });
    return operation;
}

quint64 MihomoClient::testGroupDelay(const QString &group) {
    const quint64 operation = beginOperation();
    QUrl url(endpoint_.httpBase() + "/group/" + QUrl::toPercentEncoding(group) + "/delay");
    QUrlQuery query;
    query.addQueryItem("url", kLatencyTestUrl);
    query.addQueryItem("timeout", QString::number(kLatencyTimeoutMs));
    url.setQuery(query);

    QNetworkRequest request{url};
    applyAuth(request);
    QNetworkReply *reply = trackReply(network_->get(request));

    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        QString error;
        if (reply->error() != QNetworkReply::NoError) {
            error = tr("Latency test failed: %1").arg(reply->errorString());
            emit errorOccurred(error);
        }
        settle(operation, false, error);
        fetchProxies();
    });
    return operation;
}

quint64 MihomoClient::testNodeDelay(const QString &node) {
    const quint64 operation = beginOperation();
    QUrl url(endpoint_.httpBase() + "/proxies/" + QUrl::toPercentEncoding(node) + "/delay");
    QUrlQuery query;
    query.addQueryItem("url", kLatencyTestUrl);
    query.addQueryItem("timeout", QString::number(kLatencyTimeoutMs));
    url.setQuery(query);
    QNetworkRequest request{url};
    applyAuth(request);
    QNetworkReply *reply = trackReply(network_->get(request));
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        QString error;
        if (reply->error() != QNetworkReply::NoError) {
            error = tr("Latency test failed: %1").arg(reply->errorString());
            emit errorOccurred(error);
        }
        settle(operation, false, error);
        fetchProxies();
    });
    return operation;
}

quint64 MihomoClient::fetchRules() {
    const quint64 operation = beginOperation();
    QNetworkReply *reply = get("/rules");
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = tr("Failed to load rules: %1").arg(reply->errorString());
            emit errorOccurred(error);
            settle(operation, false, error);
            return;
        }

        const auto document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject() || !document.object().value("rules").isArray()) {
            const QString error = tr("Controller returned an invalid rules response");
            emit errorOccurred(error);
            settle(operation, false, error);
            return;
        }
        const QJsonArray items = document.object().value("rules").toArray();

        QVector<Rule> rules;
        rules.reserve(items.size());
        for (const QJsonValue &item : items) {
            const QJsonObject entry = item.toObject();
            rules.append(Rule{entry.value("type").toString(), entry.value("payload").toString(),
                              entry.value("proxy").toString()});
        }

        emit rulesUpdated(rules);
        settle(operation, false, {});
    });
    return operation;
}

quint64 MihomoClient::fetchConfigs() {
    const quint64 operation = beginOperation();
    QNetworkReply *reply = get("/configs");
    const quint64 tunRevision = tunChangeId_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, tunRevision, operation] {
        reply->deleteLater();
        // A config poll begun before a confirmed TUN snapshot is superseded by
        // it, exactly as an endpoint or request epoch supersedes a reply.
        if (!isCurrentReply(reply) || tunRevision != tunChangeId_) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = tr("Failed to load configuration: %1").arg(reply->errorString());
            emit errorOccurred(error);
            settle(operation, false, error);
            return;
        }
        const auto document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject() || !document.object().value("mode").isString()) {
            const QString error = tr("Controller returned an invalid configuration response");
            emit errorOccurred(error);
            settle(operation, false, error);
            return;
        }
        publishConfig(document.object());
        settle(operation, false, {});
    });
    return operation;
}

void MihomoClient::publishConfig(const QJsonObject &object) {
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
    lastTunEnabled_ = config.tunEnabled;
    emit configReceived(config);
}

void MihomoClient::finishTunChange(bool actual, const QString &error) {
    if (!tunChangePending_) return;
    const bool requested = tunRequested_;
    tunChangePending_ = false;
    // Also invalidate ordinary config polls begun before the confirmed snapshot.
    ++tunChangeId_;
    emit tunChangeFinished(requested, actual, error);
}

quint64 MihomoClient::setTunEnabled(bool enabled) {
    if (tunChangePending_) return 0;
    tunChangePending_ = true;
    tunRequested_ = enabled;
    const quint64 operation = ++tunChangeId_;
    if (!connected_) {
        finishTunChange(lastTunEnabled_, tr("Connect to the controller before changing TUN."));
        return operation;
    }

    QNetworkReply *reply = get("/configs");
    QTimer::singleShot(10000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation, enabled] {
        reply->deleteLater();
        if (!isCurrentReply(reply) || operation != tunChangeId_ || !tunChangePending_) return;
        if (reply->error() != QNetworkReply::NoError) {
            finishTunChange(lastTunEnabled_, tr("Could not read TUN configuration: %1").arg(reply->errorString()));
            return;
        }
        const auto document = QJsonDocument::fromJson(reply->readAll());
        const auto object = document.object();
        const auto tunValue = object.value("tun");
        if (!document.isObject() || !object.value("mode").isString() ||
            (!tunValue.isUndefined() && !tunValue.isObject()) ||
            (tunValue.toObject().contains("enable") && !tunValue.toObject().value("enable").isBool())) {
            finishTunChange(lastTunEnabled_, tr("Controller returned an invalid TUN configuration."));
            return;
        }
        publishConfig(object);
        const QJsonObject currentTun = tunValue.toObject();
        if (currentTun.value("enable").isBool() && currentTun.value("enable").toBool() == enabled) {
            finishTunChange(enabled, {});
            return;
        }

        // Mihomo merges omitted tun fields with LastTunConf. Supply defaults only
        // for absent keys; explicit routing/interface/stack choices remain intact.
        QJsonObject tunPatch{{"enable", enabled}};
        if (enabled) {
            if (!currentTun.contains("stack")) tunPatch.insert("stack", "mixed");
            if (!currentTun.contains("auto-route")) tunPatch.insert("auto-route", true);
            if (!currentTun.contains("auto-detect-interface")) tunPatch.insert("auto-detect-interface", true);
        }
        QNetworkRequest request{QUrl(endpoint_.httpBase() + "/configs")};
        applyAuth(request);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply *patchReply = trackReply(network_->sendCustomRequest(
            request, "PATCH", QJsonDocument(QJsonObject{{"tun", tunPatch}}).toJson(QJsonDocument::Compact)));
        QTimer::singleShot(10000, patchReply, [patchReply] { if (patchReply->isRunning()) patchReply->abort(); });
        connect(patchReply, &QNetworkReply::finished, this, [this, patchReply, operation] {
            patchReply->deleteLater();
            if (!isCurrentReply(patchReply) || operation != tunChangeId_ || !tunChangePending_) return;
            QString error;
            const int status = patchReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (patchReply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                const auto detail = QJsonDocument::fromJson(patchReply->readAll()).object().value("message").toString();
                error = tr("Could not change TUN: %1").arg(detail.isEmpty() ? patchReply->errorString() : detail);
            }
            // PATCH may return 204 after device creation fails. Read back the
            // listener state, including after an HTTP failure, before reporting.
            confirmTunChange(operation, error);
        });
    });
    return operation;
}

void MihomoClient::confirmTunChange(quint64 operation, const QString &patchError) {
    QNetworkReply *reply = get("/configs");
    QTimer::singleShot(10000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation, patchError] {
        reply->deleteLater();
        if (!isCurrentReply(reply) || operation != tunChangeId_ || !tunChangePending_) return;
        QString error = patchError;
        const auto document = QJsonDocument::fromJson(reply->readAll());
        const auto object = document.object();
        if (reply->error() != QNetworkReply::NoError || !document.isObject() ||
            !object.value("mode").isString() || !object.value("tun").toObject().value("enable").isBool()) {
            const QString reason = reply->error() != QNetworkReply::NoError
                ? reply->errorString() : tr("invalid controller response");
            if (!error.isEmpty()) error += "\n";
            error += tr("Could not confirm the actual TUN state: %1").arg(reason);
            finishTunChange(lastTunEnabled_, error);
            return;
        }
        publishConfig(object);
        const bool actual = object.value("tun").toObject().value("enable").toBool();
        if (error.isEmpty() && actual != tunRequested_) {
            error = tunRequested_
                ? tr("The controller accepted the request, but TUN is still disabled. Check the core logs and whether the core has permission to create a TUN interface.")
                : tr("The controller accepted the request, but TUN is still enabled.");
        }
        finishTunChange(actual, error);
    });
}

quint64 MihomoClient::patchMode(const QString &mode) {
    return patchConfig(QJsonObject{{"mode", mode}});
}

quint64 MihomoClient::patchConfig(const QJsonObject &patch) {
    const quint64 operation = beginOperation();
    QNetworkRequest request{QUrl(endpoint_.httpBase() + "/configs")};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyAuth(request);
    QNetworkReply *reply = trackReply(network_->sendCustomRequest(
        request, "PATCH", QJsonDocument(patch).toJson(QJsonDocument::Compact)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, patch, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = tr("Could not update configuration: %1").arg(reply->errorString());
            emit errorOccurred(error);
            settle(operation, false, error);
            // Read back the actual state so optimistic widgets roll back on rejection.
            fetchConfigs();
            return;
        }
        if (patch.contains("mode")) emit modeChanged(patch.value("mode").toString());
        settle(operation, false, {});
        fetchConfigs();
    });
    return operation;
}

quint64 MihomoClient::closeConnection(const QString &id) {
    const quint64 operation = beginOperation();
    QNetworkRequest request{
        QUrl(endpoint_.httpBase() + "/connections/" + QUrl::toPercentEncoding(id))};
    applyAuth(request);
    QNetworkReply *reply = trackReply(network_->deleteResource(request));

    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        QString error;
        if (reply->error() != QNetworkReply::NoError) {
            error = tr("Could not close connection: %1").arg(reply->errorString());
            emit errorOccurred(error);
        }
        settle(operation, false, error);
    });
    return operation;
}

quint64 MihomoClient::closeAllConnections() {
    const quint64 operation = beginOperation();
    QNetworkRequest request{QUrl(endpoint_.httpBase() + "/connections")};
    applyAuth(request);
    QNetworkReply *reply = trackReply(network_->deleteResource(request));

    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        QString error;
        if (reply->error() != QNetworkReply::NoError) {
            error = tr("Could not close connections: %1").arg(reply->errorString());
            emit errorOccurred(error);
        }
        settle(operation, false, error);
    });
    return operation;
}

quint64 MihomoClient::updateGeoDatabases() {
    if (geoUpdate_) return 0;
    const quint64 operation = beginOperation();
    QNetworkRequest request{QUrl(endpoint_.httpBase() + "/configs/geo")};
    applyAuth(request);
    request.setTransferTimeout(180000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = trackReply(network_->post(request, "{}"));
    geoUpdate_ = reply;
    QTimer::singleShot(180000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        if (geoUpdate_ == reply) geoUpdate_ = nullptr;
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString error = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300
            ? QString() : tr("Could not update GEO databases (HTTP %1): %2").arg(status).arg(reply->errorString());
        emit geoDatabasesUpdated(error);
        settle(operation, false, error);
        if (!error.isEmpty()) emit errorOccurred(error);
        else fetchRules();
    });
    return operation;
}

quint64 MihomoClient::queryDns(const QString &name, const QString &type) {
    const quint64 operation = beginOperation();
    QUrlQuery query;
    query.addQueryItem("name", name);
    query.addQueryItem("type", type);
    QNetworkReply *reply = get("/dns/query?" + query.toString(QUrl::FullyEncoded));
    connect(reply, &QNetworkReply::finished, this, [this, reply, name, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        if (reply->error() != QNetworkReply::NoError) {
            emit dnsQueryFinished(name, {}, reply->errorString());
            settle(operation, false, reply->errorString());
            return;
        }
        const auto document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject()) {
            const QString error = tr("Invalid DNS response from controller");
            emit dnsQueryFinished(name, {}, error);
            settle(operation, false, error);
            return;
        }
        emit dnsQueryFinished(name, document.object(), {});
        settle(operation, false, {});
    });
    return operation;
}

quint64 MihomoClient::flushDnsCache(bool fakeIp) {
    const quint64 operation = beginOperation();
    QNetworkRequest request{QUrl(endpoint_.httpBase() +
        (fakeIp ? "/cache/fakeip/flush" : "/cache/dns/flush"))};
    applyAuth(request);
    QNetworkReply *reply = trackReply(network_->post(request, QByteArray()));
    connect(reply, &QNetworkReply::finished, this, [this, reply, fakeIp, operation] {
        reply->deleteLater();
        if (!isCurrentReply(reply)) { settle(operation, true, {}); return; }
        ReplyScope scope(this, operation);
        const QString error = reply->error() == QNetworkReply::NoError ? QString()
                                                                      : reply->errorString();
        emit dnsCacheFlushed(fakeIp, error);
        settle(operation, false, error);
    });
    return operation;
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

    const quint64 epoch = endpointEpoch_;
    connect(socket, &QWebSocket::textMessageReceived, this,
            [this, handler, epoch](const QString &message) {
                if (epoch == endpointEpoch_) (this->*handler)(message);
            });
    connect(socket, &QWebSocket::connected, this, [this, backoff, epoch, path] {
        *backoff = kReconnectMinMs;
        if (epoch == endpointEpoch_ && path == "/traffic") refreshState();
    });
    connect(socket, &QWebSocket::disconnected, this, [this, path, epoch, reconnect] {
        if (epoch != endpointEpoch_) return;
        if (path == "/traffic") setConnected(false);
        if (path == "/connections") emit connectionsUpdated({}, 0, 0);
        reconnect();
    });
    connect(socket, &QWebSocket::errorOccurred, this,
            [this, socket, path, epoch, reconnect](QAbstractSocket::SocketError) {
                if (epoch != endpointEpoch_) return;
                if (path == "/traffic") setConnected(false);
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
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) return;
    const QJsonObject sample = document.object();
    emit trafficSample(static_cast<quint64>(sample.value("up").toDouble()),
                       static_cast<quint64>(sample.value("down").toDouble()));
}

void MihomoClient::handleConnectionsMessage(const QString &message) {
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) return;
    const QJsonObject snapshot = document.object();
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
        connection.sourcePort = metadata.value("sourcePort").toVariant().toString();
        connection.destinationIp = metadata.value("destinationIP").toString();
        connection.destinationPort = metadata.value("destinationPort").toVariant().toString();
        connection.process = metadata.value("process").toString();
        connection.processPath = metadata.value("processPath").toString();
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
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) return;
    const QJsonObject object = document.object();

    LogEntry entry;
    entry.level = object.value("type").toString();
    entry.payload = object.value("payload").toString();
    entry.time = QDateTime::currentDateTime();

    emit logReceived(entry);
}

void MihomoClient::handleMemoryMessage(const QString &message) {
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) return;
    const QJsonObject sample = document.object();
    emit memorySample(static_cast<quint64>(sample.value("inuse").toDouble()),
                      static_cast<quint64>(sample.value("oslimit").toDouble()));
}

}  // namespace core
