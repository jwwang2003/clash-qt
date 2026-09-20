#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QVector>

#include "core/types.h"

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QWebSocket;

namespace core {

/// Talks to mihomo's external controller: REST for state and control,
/// WebSockets for the traffic / connections / logs / memory streams.
///
/// This header is the contract between the core and ui modules; changing a
/// signal signature here breaks the UI, so extend rather than reshape.
class MihomoClient : public QObject {
    Q_OBJECT

public:
    explicit MihomoClient(QObject *parent = nullptr);

    void setEndpoint(const Endpoint &endpoint);
    const Endpoint &endpoint() const { return endpoint_; }
    bool isConnected() const { return connected_; }

    // --- REST ---
    void fetchVersion();
    void fetchProxies();
    void fetchRules();
    void fetchConfigs();
    void selectNode(const QString &group, const QString &node);
    void resetGroupSelection(const QString &group);
    void testGroupDelay(const QString &group);
    void testNodeDelay(const QString &node);
    void patchMode(const QString &mode);
    void patchConfig(const QJsonObject &patch);
    /// Confirmed asynchronous TUN change. Further calls are ignored while pending.
    void setTunEnabled(bool enabled);
    bool isTunChangePending() const { return tunChangePending_; }
    void closeConnection(const QString &id);
    void closeAllConnections();
    void updateGeoDatabases();
    void queryDns(const QString &name, const QString &type = "A");
    void flushDnsCache(bool fakeIp = false);

    // --- streams ---
    void openTrafficStream();
    void closeTrafficStream();
    void openConnectionsStream();
    void closeConnectionsStream();
    void openLogStream(const QString &level = "info");
    void closeLogStream();
    void openMemoryStream();
    void closeMemoryStream();

signals:
    void endpointChanged();
    void connectedChanged(bool connected);
    void versionReceived(const QString &version);
    void proxiesUpdated(const QVector<ProxyGroup> &groups, const QHash<QString, ProxyNode> &nodes);
    void nodeSelected(const QString &group, const QString &node);
    void rulesUpdated(const QVector<Rule> &rules);
    void configReceived(const BaseConfig &config);
    void modeChanged(const QString &mode);
    void tunChangeFinished(bool requested, bool actual, const QString &error);
    void trafficSample(quint64 up, quint64 down);
    void memorySample(quint64 inuse, quint64 oslimit);
    void connectionsUpdated(const QVector<Connection> &connections, quint64 uploadTotal,
                            quint64 downloadTotal);
    void logReceived(const LogEntry &entry);
    void dnsQueryFinished(const QString &name, const QJsonObject &result, const QString &error);
    void dnsCacheFlushed(bool fakeIp, const QString &error);
    void geoDatabasesUpdated(const QString &error);
    void errorOccurred(const QString &message);

private:
    QNetworkReply *get(const QString &path);
    void applyAuth(QNetworkRequest &request) const;
    QNetworkReply *trackReply(QNetworkReply *reply);
    bool isCurrentReply(QNetworkReply *reply) const;
    void setConnected(bool connected);
    void clearLiveState();
    void refreshState();
    void publishConfig(const QJsonObject &object);
    void confirmTunChange(quint64 operation, const QString &patchError);
    void finishTunChange(bool actual, const QString &error);
    QWebSocket *openStream(const QString &path, void (MihomoClient::*handler)(const QString &));
    void closeStream(QWebSocket *&socket);

    void handleTrafficMessage(const QString &message);
    void handleConnectionsMessage(const QString &message);
    void handleLogMessage(const QString &message);
    void handleMemoryMessage(const QString &message);

    Endpoint endpoint_;
    quint64 endpointEpoch_ = 0;
    quint64 requestEpoch_ = 0;
    bool connected_ = false;
    bool tunChangePending_ = false;
    bool tunRequested_ = false;
    bool lastTunEnabled_ = false;
    quint64 tunChangeId_ = 0;
    QNetworkAccessManager *network_;
    QNetworkReply *geoUpdate_ = nullptr;
    QWebSocket *trafficSocket_ = nullptr;
    QWebSocket *connectionsSocket_ = nullptr;
    QWebSocket *logSocket_ = nullptr;
    QWebSocket *memorySocket_ = nullptr;
    QString logLevel_ = "info";
};

}  // namespace core
