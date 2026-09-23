#ifndef CLASHQT_CORE_BACKEND_BACKEND_BRIDGE_H
#define CLASHQT_CORE_BACKEND_BACKEND_BRIDGE_H

// BackendBridge: the Qt-native view of MihomoBackend.
// Contract: docs/module-api.md, sections 2, 7, 9.
//
// WHY THIS EXISTS
//   BackendObserver is a plain C++ sink with defaulted callbacks. That is what
//   lets it cross a module boundary: no QObject, no moc, no Qt type in any
//   signature. The UI cannot consume such a sink -- it subscribes to everything
//   through `connect()`, and a sink has nothing to connect to.
//
//   This class is the one adapter between those two vocabularies: one
//   BackendObserver in, Qt signals out, and the UI's intent forwarded back to
//   the five facets. It is deliberately the ONLY place in the tree that knows
//   both, so the module boundary stays free of Qt and the UI stays free of the
//   sink protocol.
//
//   The signal set matches the shapes the UI already handles rather than an
//   idealised redesign, so a reader comparing the two sides sees the same
//   types on each. Where a shape could not be preserved, the reason is stated
//   on the signal.
//
// ---------------------------------------------------------------------------
// OWNERSHIP RULE FOR SPANS - the answer to contract section 9
//
//   COPY AT THE BOUNDARY, BEFORE ANY EMISSION.
//
//   A Span handed to an observer is borrowed for the duration of that callback.
//   A Qt consumer keeps what it is given: ProxiesPage stores the groups,
//   TrayProxyMenu stores them, ConnectionsPage diffs this sample against the
//   last. So every Span is materialised into an owned Qt container - QVector by
//   element copy, QHash<QString, ProxyNode> keyed by ProxyNode::name, which is
//   the key observer.h documents and the shape the UI already indexes by - at
//   the top of the callback, and only then is the signal emitted.
//
//   No Span, and no pointer or reference derived from one, appears in any
//   signature below or outlives the callback that produced it. The container a
//   consumer receives is owned by the bridge's stack frame for the duration of
//   the emission; a direct connection reads it in place, a queued connection
//   copies it, and every type in these signatures is registered as a metatype
//   in the constructor so the queued case is legal rather than a runtime
//   warning.
//
// ---------------------------------------------------------------------------
// GENERATION RULE - THE BRIDGE FILTERS; CONSUMERS DO NOT
//
//   Contract section 2 obliges a consumer to reject any event whose generation
//   is older than the newest one it has observed. That obligation is discharged
//   HERE, once, and not republished.
//
//   Why not expose the generation and let each consumer guard?
//     * There are 66 call sites. A per-site guard is 66 chances to omit one,
//       and an omission is silent: an in-flight snapshot repopulates a view a
//       disconnect just cleared. That is the precise defect the ~20
//       isCurrentReply guards in mihomo_client.cpp exist to prevent, and moving
//       it from one file into 66 lambdas is not a refactor, it is a dispersal.
//     * Not one of the 66 sites wants a superseded event. They are views of one
//       controller; there is no reader for whom stale data is correct.
//     * A leading Generation parameter would defeat Qt's "a slot may take fewer
//       arguments" rule in the wrong direction: every 0-argument endpointChanged
//       lambda (10 of them) would have to grow a parameter it then ignores.
//
//   So the bridge keeps one lastObservedGeneration(), applies
//   backend::isSuperseded() to every event, and additionally drops any
//   completion the backend itself marked CompletionStatus::Superseded. Dropped
//   events are COUNTED, not silently discarded - supersededEventCount() and
//   eventSuperseded() make "the guard fired" distinguishable from "nothing
//   arrived", which is what the tests assert on.
//
//   The escape hatch is generationChanged(), emitted BEFORE the events of the
//   new generation, in the order contract section 2 requires. A future consumer
//   that genuinely needs the stamp reads lastObservedGeneration() from its own
//   handler; nothing today does.
//
//   ONE EXEMPTION, AND IT IS NOT A LOOPHOLE. The terminal outcomes of the
//   managed core - coreStateChanged, coreReady, coreFailed, coreStopped and
//   stopCompleted - advance lastObservedGeneration() but are NEVER dropped.
//   types.h states the reason as contract: an operation that bumps the
//   generation itself carries the generation AFTER the bump, because it reports
//   the core's new state and "is not work that the bump invalidated".
//
//   Filtering them would be actively unsafe, not merely pedantic. main.cpp
//   blocks quit until stopFinished arrives and raises a shutdown warning when
//   confirmed == false; a dropped stop wedges the quit path forever. That is
//   not hypothetical: a stop completion has been observed reaching a consumer
//   stamped with a superseded generation, so a bridge that filtered uniformly
//   would swallow exactly the event the application cannot proceed without. The
//   exemption follows the stamping rule and holds either way.
//
//   Stream samples (traffic, memory, connections, logs, provider busy) ARE
//   filtered. A sample produced before an endpoint change and delivered after
//   it is precisely the in-flight snapshot that repopulates a cleared view.
//
// ---------------------------------------------------------------------------
// NON-RE-ENTRANCY - the hazard this must not reintroduce
//
//   MihomoClient::clearLiveState() emits five signals synchronously from inside
//   setEndpoint()/setConnected(), so a handler could re-enter the client
//   mid-mutation. Contract section 7 forbids the backend from doing that, and
//   the bridge must not smuggle it back in on the Qt side.
//
//   The guarantee: THE BRIDGE ADDS NO SYNCHRONOUS EMISSION PATH OF ITS OWN.
//     * Every emission originates in a BackendObserver callback, which the
//       backend delivers only after a mutating call has returned.
//     * The bridge never calls a backend method - mutating or not - from inside
//       a callback. It cannot close the loop itself.
//     * It answers endpoint(), isConnected(), coreState() and the rest from
//       state CACHED OUT OF THE EVENTS, never by asking the backend. A handler
//       that reads bridge->endpoint() therefore sees the value belonging to the
//       event it is handling, not whatever the backend has since moved to. The
//       UI reads client_->endpoint() from inside its handlers today and that is
//       a latent mismatch; caching removes it.
//     * The intent slots do call the backend, and a consumer may invoke one
//       from inside a handler. That is the permitted direction: the backend
//       queues what such a call produces and delivers it after returning.
//       isEmitting() publishes the witness so a test can assert it.
//
//   Every sink override is noexcept, as observer.h requires. A slot that lets an
//   exception escape terminates the process rather than corrupting the
//   backend's queue - that is contract rule 4 working as specified, not an
//   oversight.
//
// LIFETIME
//   The bridge registers its sink with the backend on construction and removes
//   it on destruction. `backend` must outlive it. The bridge is destroyed
//   BEFORE the backend, as RoutingController is; the composition root owns that
//   ordering.

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include "core/backend/backend.h"

namespace core::backend {

// NOT itself a BackendObserver, deliberately. Twenty-three of the observer's
// callbacks share a name with the signal that republishes them, and a class
// holding both would make `&BackendBridge::proxiesUpdated` an ambiguous
// overload - which is the exact expression all 66 migrated call sites have to
// write. The sink is therefore a nested member that forwards inward, and the
// signal names stay unqualified and unambiguous.
class BackendBridge : public QObject {
    Q_OBJECT

  public:
    // `backend` must outlive this object; it is not owned.
    explicit BackendBridge(MihomoBackend &backend, QObject *parent = nullptr);
    ~BackendBridge() override;

    BackendBridge(const BackendBridge &) = delete;
    BackendBridge &operator=(const BackendBridge &) = delete;

    // ----------------------------------------------------- cached view
    //
    // Answered from the events, never by calling the backend. See the
    // non-re-entrancy note above for why that distinction matters.

    Generation lastObservedGeneration() const noexcept { return lastObserved_; }
    int supersededEventCount() const noexcept { return supersededCount_; }
    // True while a signal of this bridge is on the stack. The re-entrancy
    // witness the contract test asserts on.
    bool isEmitting() const noexcept { return emitting_ > 0; }

    Endpoint endpoint() const { return endpoint_; }
    Ownership endpointOwnership() const noexcept { return endpointOwnership_; }
    bool isConnected() const noexcept { return connected_; }
    CoreState coreState() const noexcept { return coreState_; }
    Ownership coreOwnership() const noexcept { return coreOwnership_; }
    bool isProviderBusy() const noexcept { return providerBusy_; }

    // The facets, for the queries that are not events: discoverBinary(),
    // activeConfigPaths(), isManagedCoreActive(), attachmentOwnership(),
    // serviceSupported(), features(), timings(). Published as a reference
    // rather than mirrored, because a query is not an event and duplicating it
    // here would be a second source of truth.
    MihomoBackend &backend() const noexcept { return backend_; }

  public slots:
    // --------------------------------------------------------- UI intent
    //
    // Thin forwards to the facets. These exist so a UI widget can be a connect
    // RECEIVER, which is how three of the 66 references are spelled today:
    //   connect(button,   &QPushButton::clicked, client_, &MihomoClient::fetchRules)
    //   connect(closeAll, &QPushButton::clicked, client_, &MihomoClient::closeAllConnections)
    //   connect(core, &CoreProcess::ready, client, &MihomoClient::setEndpoint)
    // The third becomes connect(bridge, &BackendBridge::coreReady,
    //                           bridge, &BackendBridge::attach).

    // ---- lifecycle
    RequestId startCore(const QString &configPath, const QString &workDir);
    RequestId stopCore();

    // ---- attachment
    RequestId attach(const Endpoint &endpoint);
    RequestId detach();
    RequestId refreshConfig();

    // ---- control
    RequestId setMode(const QString &mode);
    RequestId setTunEnabled(bool enabled);
    RequestId selectNode(const QString &group, const QString &node);
    RequestId resetGroupSelection(const QString &group);
    RequestId testGroupDelay(const QString &group);
    RequestId testNodeDelay(const QString &node);
    RequestId closeConnection(const QString &id);
    RequestId closeAllConnections();
    RequestId updateGeoDatabases();
    RequestId queryDns(const QString &name, const QString &type);
    RequestId flushDnsCache(bool fakeIp);

    // ---- telemetry
    RequestId refreshVersion();
    RequestId refreshProxies();
    RequestId refreshRules();
    RequestId openTrafficStream();
    RequestId closeTrafficStream();
    RequestId openConnectionsStream();
    RequestId closeConnectionsStream();
    RequestId openLogStream(const QString &level);
    RequestId closeLogStream();
    RequestId openMemoryStream();
    RequestId closeMemoryStream();

    // ---- providers
    RequestId fetchProviders(bool rules);
    RequestId updateProvider(bool rules, const QString &name);
    RequestId healthCheckProvider(const QString &name);

    // ---- capabilities
    RequestId requestPrivilegedServiceStatus();

  signals:
    // =================================================== managed core (12)
    //
    // Displaces core::CoreProcess. `state` is backend::CoreState, NOT
    // core::CoreState: the published enum has a fixed underlying type and a
    // reserved range, which the component-private one does not. That is the one
    // unavoidable spelling change on this group; the four stateChanged handlers
    // compare against ::Stopped / ::Failed / ::Running / ::Stopping, all of
    // which exist here with the same meaning.
    //
    // `ownership` is appended, not substituted - a handler that ignores it
    // connects unchanged, because Qt permits a slot to take fewer arguments.
    void coreStateChanged(core::backend::CoreState state,
                          core::backend::Ownership ownership);  // 4 sites
    // CoreProcess::ready. Fires on a 200 GET /version with a string `version`,
    // never merely on "the process started".
    void coreReady(const core::backend::Endpoint &endpoint);  // 3 sites
    void coreLogLine(const QString &line);                    // 1 site
    void coreFailed(const QString &reason);                   // 2 sites
    void coreStopped();                                       // 1 site
    // CoreProcess::stopFinished. confirmed == false is NOT success; main.cpp
    // turns it into a shutdown warning that blocks quit.
    void stopFinished(bool confirmed, const QString &error);  // 1 site

    // ==================================================== attachment (21)
    //
    // MihomoClient::endpointChanged carries no arguments today and every
    // handler then reads client_->endpoint(). Both arguments are appended, so
    // all ten 0-argument lambdas connect unchanged, and a handler that wants
    // the value reads the one that belongs to this event instead of racing the
    // backend for it.
    void endpointChanged(const core::backend::Endpoint &endpoint,
                         core::backend::Ownership ownership);  // 10 sites
    void connectedChanged(bool connected);                     // 11 sites

    // ============================================== configuration, mode (8)
    void configReceived(const core::backend::BaseConfig &config);  // 7 sites
    void modeChanged(const QString &mode);                         // 1 site

    // ==================================================== routing control (1)
    // MihomoClient::tunChangeFinished. `actual` is the controller's read-back,
    // never an echo of `requested`; RoutingControls depends on being able to
    // tell those apart.
    void tunChangeFinished(bool requested, bool actual, const QString &error);  // 1 site
    // No UI site today; the node picker calls selectNode() and waits for
    // proxiesUpdated. Republished because dropping a contract event would make
    // the bridge lossy for the next consumer.
    void nodeSelected(const QString &group, const QString &node);  // 0 sites

    // ================================================ telemetry snapshots (5)
    void versionReceived(const QString &version);  // 2 sites
    // Span<ProxyGroup> -> QVector, Span<ProxyNode> -> QHash keyed by name.
    void proxiesUpdated(const QVector<core::backend::ProxyGroup> &groups,
                        const QHash<QString, core::backend::ProxyNode> &nodes);  // 2 sites
    void rulesUpdated(const QVector<core::backend::Rule> &rules);                // 1 site

    // =================================================== telemetry streams (8)
    void trafficSample(quint64 up, quint64 down);           // 3 sites
    void memorySample(quint64 inuse, quint64 oslimit);      // 2 sites
    void connectionsUpdated(const QVector<core::backend::Connection> &connections,
                            quint64 uploadTotal, quint64 downloadTotal);  // 2 sites
    void logReceived(const core::backend::LogEntry &entry);               // 1 site

    // ====================================================== diagnostics (3)
    //
    // The contract delivers the DNS answer as response TEXT, because a parsed
    // QJsonObject cannot cross a module boundary. HomePage reads
    // response.value("Status") and iterates Answer/Authority/Additional. The
    // parse happens HERE - on the consumer side of the boundary, which is
    // exactly where contract section 9 puts it - so the handler is unchanged.
    void dnsQueryFinished(const QString &name, const QJsonObject &result,
                          const QString &error);              // 1 site
    void dnsCacheFlushed(bool fakeIp, const QString &error);  // 1 site
    void geoDatabasesUpdated(const QString &error);           // 1 site

    // ======================================================== providers (4)
    //
    // backend::Provider is field-identical to core::Provider; only the
    // qualification changes.
    void providersReceived(bool rules,
                           const QVector<core::backend::Provider> &providers);  // 1 site
    void providerBusyChanged(bool busy);                                        // 1 site
    void providerOperationFinished(const QString &message);                     // 1 site
    // ProviderClient has its own error channel and BackendObserver does not.
    // The bridge reconstructs it BY REQUEST ID: a failed completion on
    // providersReceived() or providerOperationFinished() records its RequestId,
    // and the global errorOccurred() that observer.h guarantees for that same
    // request is then routed here instead. So ProvidersPage's status label
    // keeps behaving as it does, a provider failure does not become a
    // main-window status-bar toast it never was, and - the part that matters -
    // a failure is reported EXACTLY ONCE rather than on both channels.
    void providerError(const QString &message);  // 1 site

    // =========================================================== errors (1)
    void errorOccurred(const QString &message);  // 1 site

    // ==================================================== capabilities (1)
    //
    // ServiceSettings' one status channel. It used to open a SECOND
    // PrivilegedServiceClient beside the one the backend owns - two live
    // connections to one privileged socket, which is a correctness hazard -
    // and this is what it migrated onto.
    //
    // `coreRunning` is APPENDED, not substituted: a slot may take fewer
    // arguments, so the three-argument handlers that predate it connect
    // unchanged. It is the helper's own running-core report and is meaningful
    // only when `state == ServiceState::Connected` and `error` is empty; see
    // PrivilegedServiceStatus::coreRunning.
    void privilegedServiceStatus(core::backend::ServiceState state, const QString &version,
                                 const QString &error, bool coreRunning);

    // ===================================================== bookkeeping (0)
    //
    // Live state was cleared by an endpoint change or a disconnect. The
    // contract publishes the clear as five empty events - proxies, rules,
    // connections, traffic, memory - each with RequestId::Invalid; this fires
    // once per clear, alongside the proxies one, so a consumer can distinguish
    // "the controller genuinely has no proxies" from "we were disconnected"
    // without counting five signals. It is the replacement for
    // clearLiveState()'s five SYNCHRONOUS emissions, and may legitimately fire
    // more than once for a single attach: the backend clears on the disconnect
    // and again on the new attachment.
    void liveStateCleared();
    // Emitted before any event of the new generation, per the ordering rule.
    void generationChanged(quint64 generation);
    // A dropped event, named by channel. Diagnostics and tests only - the guard
    // is already applied when this fires.
    void eventSuperseded(const QString &channel);

  private:
    // The BackendObserver, kept off BackendBridge's own interface. Its only job
    // is to turn a virtual call into a call on the enclosing bridge; all of the
    // adaptation lives in the on*() handlers below.
    class Sink final : public BackendObserver {
      public:
        explicit Sink(BackendBridge *bridge) noexcept : bridge_(bridge) {}

        void coreStateChanged(Generation generation, CoreState state,
                              Ownership ownership) noexcept override;
        void coreReady(const Completion &completion,
                       const Endpoint &endpoint) noexcept override;
        void coreLogLine(Generation generation, const QString &line) noexcept override;
        void coreFailed(const Completion &completion) noexcept override;
        void coreStopped(Generation generation) noexcept override;
        void stopCompleted(const StopCompleted &result) noexcept override;
        void endpointChanged(Generation generation, const Endpoint &endpoint,
                             Ownership ownership) noexcept override;
        void connectedChanged(Generation generation, bool connected) noexcept override;
        void configReceived(const Completion &completion,
                            const BaseConfig &config) noexcept override;
        void modeChanged(const Completion &completion, const QString &mode) noexcept override;
        void tunChangeCompleted(const TunChangeCompleted &result) noexcept override;
        void nodeSelected(const Completion &completion, const QString &group,
                          const QString &node) noexcept override;
        void geoDatabasesUpdated(const Completion &completion) noexcept override;
        void dnsQueryFinished(const Completion &completion, const QString &name,
                              const QString &resultJson) noexcept override;
        void dnsCacheFlushed(const Completion &completion, bool fakeIp) noexcept override;
        void versionReceived(const Completion &completion,
                             const QString &version) noexcept override;
        void proxiesUpdated(const Completion &completion, Span<ProxyGroup> groups,
                            Span<ProxyNode> nodes) noexcept override;
        void rulesUpdated(const Completion &completion, Span<Rule> rules) noexcept override;
        void trafficSample(Generation generation, quint64 up, quint64 down) noexcept override;
        void memorySample(Generation generation, quint64 inuse,
                          quint64 oslimit) noexcept override;
        void connectionsUpdated(Generation generation, Span<Connection> connections,
                                quint64 uploadTotal, quint64 downloadTotal) noexcept override;
        void logReceived(Generation generation, const LogEntry &entry) noexcept override;
        void providersReceived(const Completion &completion, bool rules,
                               Span<Provider> providers) noexcept override;
        void providerBusyChanged(Generation generation, bool busy) noexcept override;
        void providerOperationFinished(const Completion &completion,
                                       const QString &message) noexcept override;
        void privilegedServiceStatus(const Completion &completion,
                                     const PrivilegedServiceStatus &status) noexcept override;
        void errorOccurred(const Completion &completion) noexcept override;

      private:
        BackendBridge *bridge_;
    };

    // ------------------------------------------- inward handlers, one per event
    //
    // Named on*() rather than after the signal, so that &BackendBridge::<signal>
    // names exactly one function at every migrated call site.

    void onCoreStateChanged(Generation generation, CoreState state,
                            Ownership ownership) noexcept;
    void onCoreReady(const Completion &completion, const Endpoint &endpoint) noexcept;
    void onCoreLogLine(Generation generation, const QString &line) noexcept;
    void onCoreFailed(const Completion &completion) noexcept;
    void onCoreStopped(Generation generation) noexcept;
    void onStopCompleted(const StopCompleted &result) noexcept;
    void onEndpointChanged(Generation generation, const Endpoint &endpoint,
                           Ownership ownership) noexcept;
    void onConnectedChanged(Generation generation, bool connected) noexcept;
    void onConfigReceived(const Completion &completion, const BaseConfig &config) noexcept;
    void onModeChanged(const Completion &completion, const QString &mode) noexcept;
    void onTunChangeCompleted(const TunChangeCompleted &result) noexcept;
    void onNodeSelected(const Completion &completion, const QString &group,
                        const QString &node) noexcept;
    void onGeoDatabasesUpdated(const Completion &completion) noexcept;
    void onDnsQueryFinished(const Completion &completion, const QString &name,
                            const QString &resultJson) noexcept;
    void onDnsCacheFlushed(const Completion &completion, bool fakeIp) noexcept;
    void onVersionReceived(const Completion &completion, const QString &version) noexcept;
    void onProxiesUpdated(const Completion &completion, Span<ProxyGroup> groups,
                          Span<ProxyNode> nodes) noexcept;
    void onRulesUpdated(const Completion &completion, Span<Rule> rules) noexcept;
    void onTrafficSample(Generation generation, quint64 up, quint64 down) noexcept;
    void onMemorySample(Generation generation, quint64 inuse, quint64 oslimit) noexcept;
    void onConnectionsUpdated(Generation generation, Span<Connection> connections,
                              quint64 uploadTotal, quint64 downloadTotal) noexcept;
    void onLogReceived(Generation generation, const LogEntry &entry) noexcept;
    void onProvidersReceived(const Completion &completion, bool rules,
                             Span<Provider> providers) noexcept;
    void onProviderBusyChanged(Generation generation, bool busy) noexcept;
    void onProviderOperationFinished(const Completion &completion,
                                     const QString &message) noexcept;
    void onPrivilegedServiceStatus(const Completion &completion,
                                   const PrivilegedServiceStatus &status) noexcept;
    void onErrorOccurred(const Completion &completion) noexcept;

    // ------------------------------------------------------------- internals

    // Scopes one emission so isEmitting() is true for its duration. Counted,
    // not a flag: a handler may legally provoke a nested emission by calling an
    // intent slot, and a flag would be cleared by the inner scope.
    class EmitScope {
      public:
        explicit EmitScope(BackendBridge *self) noexcept : self_(self) { ++self_->emitting_; }
        ~EmitScope() { --self_->emitting_; }
        EmitScope(const EmitScope &) = delete;
        EmitScope &operator=(const EmitScope &) = delete;

      private:
        BackendBridge *self_;
    };

    // Contract section 2, applied once for all 66 call sites.
    // Returns false when the event must be dropped.
    bool admit(Generation generation, const char *channel) noexcept;
    bool admit(const Completion &completion, const char *channel) noexcept;
    // Advances the observed generation without ever dropping. The managed
    // core's terminal outcomes only; see the exemption note above.
    void observeTerminal(Generation generation) noexcept;
    void observe(Generation generation) noexcept;
    void reject(const char *channel) noexcept;
    // Which error signal the failure of this completion belongs to.
    enum class ErrorChannel { Global, Provider };

    // The payload rule: a payload-bearing signal fires only on Ok. A non-Ok
    // completion emits NOTHING on that channel and nothing of its own - the
    // backend publishes every failure on errorOccurred() already
    // (observer.h: "The global error channel. completion.request names the
    // operation that failed"), and re-routing here as well reported each
    // failure twice: once as a status-bar toast, once as whatever the typed
    // channel showed. A Provider channel only records the RequestId so the
    // global error that follows can be recognised and re-aimed.
    //
    // Outcome-bearing signals (geo, dns x2, tun, stop, service status) do not
    // use this. They fire either way and carry the error text, because their
    // consumers re-enable a control from that handler and would otherwise stay
    // disabled forever.
    bool payloadOk(const Completion &completion, ErrorChannel channel) noexcept;

    MihomoBackend &backend_;
    Sink sink_{this};

    Generation lastObserved_ = Generation::Initial;
    int supersededCount_ = 0;
    int emitting_ = 0;

    // RequestIds of provider operations that failed, awaiting the global error
    // that names them. Emptied on a generation bump: nothing outlives the work
    // it belonged to.
    QSet<quint64> providerFailures_;

    Endpoint endpoint_;
    Ownership endpointOwnership_ = Ownership::None;
    bool connected_ = false;
    CoreState coreState_ = CoreState::Stopped;
    Ownership coreOwnership_ = Ownership::None;
    bool providerBusy_ = false;
};

}  // namespace core::backend

#endif  // CLASHQT_CORE_BACKEND_BACKEND_BRIDGE_H
