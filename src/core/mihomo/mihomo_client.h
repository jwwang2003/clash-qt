#pragma once

#include <cstdint>
#include <vector>

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
    /// A collaborator whose own transport work belongs to this client's
    /// SESSION rather than to its address.
    ///
    /// ProviderClient owns a second QNetworkAccessManager, so nothing this
    /// class aborts reaches it, and it used to learn of an invalidation from
    /// endpointChanged - which a replacement at the same address deliberately
    /// does not emit. A signal is the wrong seam for it twice over: the
    /// subscription order decides whether the participant retires before or
    /// after the OWNER has advanced its generation, and ProviderClient is
    /// constructed before MihomoBackendImpl connects anything. So retirement is
    /// an explicit call made at one known point in beginSession(), after
    /// invalidating() has been announced and BEFORE any reply is aborted.
    class SessionParticipant {
      public:
        virtual ~SessionParticipant() = default;
        /// Retires everything outstanding against the session that is ending.
        /// It must not issue new work and must not emit anything that would
        /// make an owner invalidate a second time.
        virtual void retireSession() = 0;
    };
    /// Registered participants are retired by every session boundary, in
    /// registration order. Neither call takes ownership.
    void addSessionParticipant(SessionParticipant *participant);
    void removeSessionParticipant(SessionParticipant *participant);

    explicit MihomoClient(QObject *parent = nullptr);

    /// Points this client at a controller. A DIFFERENT address retires the
    /// previous attachment outright. The SAME address is not a no-op either:
    /// a managed reload replaces the engine behind that address, so the caller
    /// is announcing a NEW SESSION on the old one. Everything still in flight
    /// belongs to the process that has gone, and is invalidated and aborted
    /// before the new session's requests are issued. What does NOT happen there
    /// is an endpointChanged for an endpoint that did not change, or a
    /// connected/live-state clear the replacement would immediately undo.
    void setEndpoint(const Endpoint &endpoint);
    /// Stops talking to the current controller: bumps the endpoint epoch,
    /// aborts every in-flight request, clears live state and closes the
    /// streams. It NEVER terminates the controller - detaching is not a kill
    /// switch - and, unlike setEndpoint, it issues nothing afterwards.
    void detach();
    /// Retires the current session WITHOUT announcing an invalidation of its
    /// own: the caller is the owner that has ALREADY advanced its generation.
    ///
    /// This is what a managed start, a managed failure or a stop uses. Those
    /// invalidate everything outstanding, but they are not client events, and
    /// re-emitting invalidating() here would ask the owner to bump a second
    /// time for one logical boundary - the recursive path this seam exists to
    /// avoid. The epochs move before anything is aborted, the participants are
    /// retired and an outstanding TUN change is cancelled.
    ///
    /// The STREAMS are not: a lifecycle event changes no address, and the
    /// boundary that replaces a subscription is the attach() a new engine's
    /// readiness produces. See the body.
    ///
    /// It does NOT clear the endpoint, the connected flag or the live view: a
    /// lifecycle event decides none of those, and the snapshot re-issue its
    /// owner owes is what re-establishes the truth.
    void retireSession(const QString &reason);
    /// The identity of the current transport session: monotonic, and advanced
    /// by every boundary, including a replacement at an unchanged address that
    /// endpointChanged() deliberately does not report.
    quint64 sessionEpoch() const noexcept { return sessionEpoch_; }
    /// The controller this client was POINTED AT, and an invalid endpoint until
    /// something points it somewhere. It is never the discovery default on its
    /// own account: see detachedEndpoint().
    const Endpoint &endpoint() const { return endpoint_; }
    bool isConnected() const { return connected_; }
    /// "Attached to nothing", explicitly. core::Endpoint's default is the
    /// localhost DISCOVERY default (127.0.0.1:9090) and isValid() accepts it,
    /// so a freshly built client used to answer endpoint().isValid() - and
    /// therefore MihomoBackendImpl::isAttached() and an Attached ownership -
    /// for a controller nothing had pointed it at. Discovery still hands that
    /// default out (controller_discovery.cpp:73-77); a guess about where a
    /// controller might be is simply not an attachment until setEndpoint()
    /// accepts it.
    static Endpoint detachedEndpoint();

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
    /// Whether the ADDRESS moved as well as the session.
    enum class SessionChange : std::uint8_t { Session, Endpoint };
    /// Whether the boundary is announced to observers. Silent is the owner-
    /// driven path (retireSession): the owner has already bumped.
    enum class SessionAnnounce : std::uint8_t { Announce, Silent };
    /// Whether the STREAMS belong to the session that is ending. They do on
    /// every replacement; they do not on a plain disconnect, where the sockets
    /// are the current session's and re-dial themselves.
    enum class SessionStreams : std::uint8_t { Retire, Keep };
    /// THE one invalidation path. Every boundary - endpoint change, detach,
    /// same-address replacement, disconnect, owner-driven retirement - advances
    /// the epochs, announces, retires the participants, cancels an outstanding
    /// TUN change and only THEN aborts, in that order and nowhere else.
    void beginSession(SessionChange change, SessionAnnounce announce, SessionStreams streams,
                      const QString &reason);
    /// Re-opens every stream whose subscription is live, on the epoch the
    /// boundary just advanced. The pointer IS the subscription, so a stream
    /// that was open is replaced rather than dropped.
    void restartStreams();
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

    Endpoint endpoint_ = detachedEndpoint();
    quint64 operation_ = 0;
    quint64 currentOperation_ = 0;
    quint64 endpointEpoch_ = 0;
    quint64 requestEpoch_ = 0;
    /// Bumped by every session boundary, and what the STREAMS are gated on. It
    /// is deliberately not endpointEpoch_: a replacement at the same address
    /// moves the session without moving the address, and a socket gated on the
    /// address would go on publishing the retired engine's frames as the new
    /// session's telemetry. It is not requestEpoch_ either - that advances on a
    /// plain disconnect, where the socket is the current session's and its own
    /// reconnect is what heals the stream.
    quint64 streamEpoch_ = 0;
    quint64 sessionEpoch_ = 0;
    std::vector<SessionParticipant *> participants_;
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
