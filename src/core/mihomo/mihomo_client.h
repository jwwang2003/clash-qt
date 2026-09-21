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
    /// Stops talking to the current controller: bumps the endpoint epoch,
    /// aborts every in-flight request, clears live state and closes the
    /// streams. It NEVER terminates the controller - detaching is not a kill
    /// switch - and, unlike setEndpoint, it issues nothing afterwards.
    void detach();
    const Endpoint &endpoint() const { return endpoint_; }
    bool isConnected() const { return connected_; }

    // --- REST ---
    //
    // Each mutating call returns the identity of the operation it submitted, or
    // 0 when it submitted nothing. Exactly one requestSettled(operation, ...) is
    // emitted for every non-zero id, after any payload signal that operation
    // produced. That is what lets MihomoBackend stamp a completion with the
    // RequestId and the Generation the work was SUBMITTED under, instead of
    // guessing which fire-and-forget call a bare signal belongs to.
    //
    // A caller that does not care may ignore the value; every existing call site
    // does, and a non-void slot connects exactly as a void one did.
    quint64 fetchVersion();
    quint64 fetchProxies();
    quint64 fetchRules();
    quint64 fetchConfigs();
    quint64 selectNode(const QString &group, const QString &node);
    quint64 resetGroupSelection(const QString &group);
    quint64 testGroupDelay(const QString &group);
    quint64 testNodeDelay(const QString &node);
    quint64 patchMode(const QString &mode);
    quint64 patchConfig(const QJsonObject &patch);
    /// Confirmed asynchronous TUN change. Further calls are ignored while
    /// pending, and 0 is returned then. The identity is the TUN change id, which
    /// is a per-operation REQUEST identity and not a generation.
    quint64 setTunEnabled(bool enabled);
    bool isTunChangePending() const { return tunChangePending_; }
    quint64 closeConnection(const QString &id);
    quint64 closeAllConnections();
    quint64 updateGeoDatabases();
    quint64 queryDns(const QString &name, const QString &type = "A");
    quint64 flushDnsCache(bool fakeIp = false);

    /// The operation whose reply handler is running right now, or 0. A payload
    /// signal emitted outside a reply handler - a stream sample, or the cleared
    /// live state - reports 0, which is what marks it unsolicited.
    quint64 currentOperation() const { return currentOperation_; }

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
    /// Emitted BEFORE any in-flight request is aborted, immediately after the
    /// epoch that invalidates it is bumped. mihomo_client.cpp documents why the
    /// order matters: finished() may run synchronously inside abort(), so an
    /// observer that learns of the invalidation afterwards would already have
    /// accepted the reply it was meant to discard.
    void invalidating();
    /// The single terminal event of a REST operation. `superseded` is true when
    /// the reply belonged to an endpoint or request epoch that has moved on;
    /// `error` is empty exactly on success.
    void requestSettled(quint64 operation, bool superseded, const QString &error);
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
    quint64 beginOperation();
    void settle(quint64 operation, bool superseded, const QString &error);
    /// Scopes currentOperation() to one reply handler.
    class ReplyScope {
      public:
        ReplyScope(MihomoClient *self, quint64 operation)
            : self_(self), previous_(self->currentOperation_) { self_->currentOperation_ = operation; }
        // Restores rather than clears: a nested handler runs whenever a reply is
        // aborted from inside this one, and the outer payload still belongs to
        // the outer operation.
        ~ReplyScope() { self_->currentOperation_ = previous_; }
        ReplyScope(const ReplyScope &) = delete;
        ReplyScope &operator=(const ReplyScope &) = delete;
      private:
        MihomoClient *self_;
        quint64 previous_ = 0;
    };
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
    quint64 operation_ = 0;
    quint64 currentOperation_ = 0;
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
