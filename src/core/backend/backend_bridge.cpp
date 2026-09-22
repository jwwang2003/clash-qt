#include "core/backend/backend_bridge.h"

#include <QJsonDocument>
#include <QMetaType>

namespace core::backend {
namespace {

// Registered so a queued connection to any of these signals is legal rather
// than a runtime "unable to handle unregistered datatype" warning. Qt 6 derives
// the metatype from the complete type, so no Q_DECLARE_METATYPE is needed in
// core/types.h - which is leased to another worker and must not be touched.
void registerMetaTypes() {
    static const bool once = [] {
        qRegisterMetaType<CoreState>("core::backend::CoreState");
        qRegisterMetaType<Ownership>("core::backend::Ownership");
        qRegisterMetaType<ServiceState>("core::backend::ServiceState");
        qRegisterMetaType<Endpoint>("core::backend::Endpoint");
        qRegisterMetaType<BaseConfig>("core::backend::BaseConfig");
        qRegisterMetaType<LogEntry>("core::backend::LogEntry");
        qRegisterMetaType<QVector<ProxyGroup>>("QVector<core::backend::ProxyGroup>");
        qRegisterMetaType<QHash<QString, ProxyNode>>("QHash<QString,core::backend::ProxyNode>");
        qRegisterMetaType<QVector<Rule>>("QVector<core::backend::Rule>");
        qRegisterMetaType<QVector<Connection>>("QVector<core::backend::Connection>");
        qRegisterMetaType<QVector<Provider>>("QVector<core::backend::Provider>");
        return true;
    }();
    (void)once;
}

// The text a consumer shows. Empty exactly on success, which is the invariant
// every one of the outcome-bearing handlers tests with isEmpty().
QString errorText(const Completion &completion) {
    if (completion.isOk()) return {};
    if (!completion.error.message.isEmpty()) return completion.error.message;
    switch (completion.status) {
        case CompletionStatus::Cancelled:
            return QObject::tr("The operation was cancelled.");
        case CompletionStatus::Superseded:
            return QObject::tr("The controller connection changed before the operation finished.");
        case CompletionStatus::Rejected:
            return QObject::tr("The operation was rejected.");
        case CompletionStatus::Failed:
        case CompletionStatus::Ok:
            break;
    }
    return QObject::tr("The operation failed.");
}

QString errorText(const ErrorInfo &error) {
    if (!error.isFailure()) return {};
    return error.message.isEmpty() ? QObject::tr("The operation failed.") : error.message;
}

// ---------------------------------------------------------------------------
// SPAN MATERIALISATION. The single place a borrowed Span becomes owned data.
// Called at the top of a callback, before any emission; nothing derived from
// the Span survives the return.

template <typename T>
QVector<T> materialise(Span<T> span) {
    QVector<T> out;
    out.reserve(static_cast<qsizetype>(span.size()));
    for (const T &value : span) out.append(value);
    return out;
}

// observer.h: "`nodes` is keyed by ProxyNode::name; it is a span rather than a
// hash so no container crosses the boundary." This restores the hash the UI
// indexes by.
QHash<QString, ProxyNode> materialiseNodes(Span<ProxyNode> span) {
    QHash<QString, ProxyNode> out;
    out.reserve(static_cast<qsizetype>(span.size()));
    for (const ProxyNode &node : span) out.insert(node.name, node);
    return out;
}

}  // namespace

// ------------------------------------------------------------- construction

BackendBridge::BackendBridge(MihomoBackend &backend, QObject *parent)
    : QObject(parent), backend_(backend) {
    registerMetaTypes();
    lastObserved_ = backend_.generation();
    // Registration is the last thing the constructor does: the bridge must be
    // fully built before an event can reach its sink.
    backend_.addObserver(&sink_);
}

BackendBridge::~BackendBridge() {
    // observer.h LIFETIME: "An observer must be removed before it is
    // destroyed." Safe from anywhere, including from inside a callback.
    backend_.removeObserver(&sink_);
}

// ------------------------------------------------------- generation guard

void BackendBridge::observe(Generation generation) noexcept {
    if (lastObserved_ < generation) {
        lastObserved_ = generation;
        // Nothing outlives the work it belonged to: a provider failure whose
        // global error never arrived is abandoned with its generation.
        providerFailures_.clear();
        EmitScope scope(this);
        // Before the events of the new generation, per the section 2 ordering
        // rule: the bump reaches the consumer first.
        emit generationChanged(number(generation));
    }
}

void BackendBridge::observeTerminal(Generation generation) noexcept { observe(generation); }

void BackendBridge::reject(const char *channel) noexcept {
    ++supersededCount_;
    EmitScope scope(this);
    emit eventSuperseded(QString::fromLatin1(channel));
}

bool BackendBridge::admit(Generation generation, const char *channel) noexcept {
    if (isSuperseded(generation, lastObserved_)) {
        reject(channel);
        return false;
    }
    observe(generation);
    return true;
}

bool BackendBridge::admit(const Completion &completion, const char *channel) noexcept {
    if (isSuperseded(completion.generation, lastObserved_)) {
        reject(channel);
        return false;
    }
    observe(completion.generation);
    // The backend may know the work was abandoned even when the stamp still
    // compares current. Both are supersession; both are dropped.
    if (completion.status == CompletionStatus::Superseded) {
        reject(channel);
        return false;
    }
    return true;
}

bool BackendBridge::payloadOk(const Completion &completion, ErrorChannel channel) noexcept {
    if (completion.isOk()) return true;
    // observer.h: "A completion callback whose `completion.status` is not Ok
    // carries no meaningful payload; the payload parameters are then empty, not
    // stale." Emitting an empty payload would clear a view that nothing
    // invalidated, so nothing is emitted here at all.
    //
    // Nor is the failure re-announced: the backend publishes it on
    // errorOccurred() carrying this same RequestId, and emitting from both
    // places reported every failure twice. The provider channel only takes a
    // note, so that the global error naming this request can be re-aimed at
    // providerError() when it arrives.
    if (channel == ErrorChannel::Provider && completion.request != RequestId::Invalid)
        providerFailures_.insert(number(completion.request));
    return false;
}

// ---------------------------------------------------------------------- sink
//
// Nothing but forwarding. Kept in one block so the mapping from contract
// callback to bridge handler is checkable at a glance.

void BackendBridge::Sink::coreStateChanged(Generation generation, CoreState state,
                                           Ownership ownership) noexcept {
    bridge_->onCoreStateChanged(generation, state, ownership);
}

void BackendBridge::Sink::coreReady(const Completion &completion,
                                    const Endpoint &endpoint) noexcept {
    bridge_->onCoreReady(completion, endpoint);
}

void BackendBridge::Sink::coreLogLine(Generation generation, const QString &line) noexcept {
    bridge_->onCoreLogLine(generation, line);
}

void BackendBridge::Sink::coreFailed(const Completion &completion) noexcept {
    bridge_->onCoreFailed(completion);
}

void BackendBridge::Sink::coreStopped(Generation generation) noexcept {
    bridge_->onCoreStopped(generation);
}

void BackendBridge::Sink::stopCompleted(const StopCompleted &result) noexcept {
    bridge_->onStopCompleted(result);
}

void BackendBridge::Sink::endpointChanged(Generation generation, const Endpoint &endpoint,
                                          Ownership ownership) noexcept {
    bridge_->onEndpointChanged(generation, endpoint, ownership);
}

void BackendBridge::Sink::connectedChanged(Generation generation, bool connected) noexcept {
    bridge_->onConnectedChanged(generation, connected);
}

void BackendBridge::Sink::configReceived(const Completion &completion,
                                         const BaseConfig &config) noexcept {
    bridge_->onConfigReceived(completion, config);
}

void BackendBridge::Sink::modeChanged(const Completion &completion,
                                      const QString &mode) noexcept {
    bridge_->onModeChanged(completion, mode);
}

void BackendBridge::Sink::tunChangeCompleted(const TunChangeCompleted &result) noexcept {
    bridge_->onTunChangeCompleted(result);
}

void BackendBridge::Sink::nodeSelected(const Completion &completion, const QString &group,
                                       const QString &node) noexcept {
    bridge_->onNodeSelected(completion, group, node);
}

void BackendBridge::Sink::geoDatabasesUpdated(const Completion &completion) noexcept {
    bridge_->onGeoDatabasesUpdated(completion);
}

void BackendBridge::Sink::dnsQueryFinished(const Completion &completion, const QString &name,
                                           const QString &resultJson) noexcept {
    bridge_->onDnsQueryFinished(completion, name, resultJson);
}

void BackendBridge::Sink::dnsCacheFlushed(const Completion &completion, bool fakeIp) noexcept {
    bridge_->onDnsCacheFlushed(completion, fakeIp);
}

void BackendBridge::Sink::versionReceived(const Completion &completion,
                                          const QString &version) noexcept {
    bridge_->onVersionReceived(completion, version);
}

void BackendBridge::Sink::proxiesUpdated(const Completion &completion, Span<ProxyGroup> groups,
                                         Span<ProxyNode> nodes) noexcept {
    bridge_->onProxiesUpdated(completion, groups, nodes);
}

void BackendBridge::Sink::rulesUpdated(const Completion &completion,
                                       Span<Rule> rules) noexcept {
    bridge_->onRulesUpdated(completion, rules);
}

void BackendBridge::Sink::trafficSample(Generation generation, quint64 up,
                                        quint64 down) noexcept {
    bridge_->onTrafficSample(generation, up, down);
}

void BackendBridge::Sink::memorySample(Generation generation, quint64 inuse,
                                       quint64 oslimit) noexcept {
    bridge_->onMemorySample(generation, inuse, oslimit);
}

void BackendBridge::Sink::connectionsUpdated(Generation generation,
                                             Span<Connection> connections, quint64 uploadTotal,
                                             quint64 downloadTotal) noexcept {
    bridge_->onConnectionsUpdated(generation, connections, uploadTotal, downloadTotal);
}

void BackendBridge::Sink::logReceived(Generation generation, const LogEntry &entry) noexcept {
    bridge_->onLogReceived(generation, entry);
}

void BackendBridge::Sink::providersReceived(const Completion &completion, bool rules,
                                            Span<Provider> providers) noexcept {
    bridge_->onProvidersReceived(completion, rules, providers);
}

void BackendBridge::Sink::providerBusyChanged(Generation generation, bool busy) noexcept {
    bridge_->onProviderBusyChanged(generation, busy);
}

void BackendBridge::Sink::providerOperationFinished(const Completion &completion,
                                                    const QString &message) noexcept {
    bridge_->onProviderOperationFinished(completion, message);
}

void BackendBridge::Sink::privilegedServiceStatus(const Completion &completion,
                                                  const PrivilegedServiceStatus &status) noexcept {
    bridge_->onPrivilegedServiceStatus(completion, status);
}

void BackendBridge::Sink::errorOccurred(const Completion &completion) noexcept {
    bridge_->onErrorOccurred(completion);
}

// ============================================================ managed core

void BackendBridge::onCoreStateChanged(Generation generation, CoreState state,
                                     Ownership ownership) noexcept {
    observeTerminal(generation);
    coreState_ = state;
    coreOwnership_ = ownership;
    EmitScope scope(this);
    emit coreStateChanged(state, ownership);
}

void BackendBridge::onCoreReady(const Completion &completion, const Endpoint &endpoint) noexcept {
    observeTerminal(completion.generation);
    if (!completion.isOk()) {
        EmitScope scope(this);
        emit coreFailed(errorText(completion));
        return;
    }
    EmitScope scope(this);
    emit coreReady(endpoint);
}

void BackendBridge::onCoreLogLine(Generation generation, const QString &line) noexcept {
    observeTerminal(generation);
    EmitScope scope(this);
    emit coreLogLine(line);
}

void BackendBridge::onCoreFailed(const Completion &completion) noexcept {
    observeTerminal(completion.generation);
    EmitScope scope(this);
    emit coreFailed(errorText(completion));
}

void BackendBridge::onCoreStopped(Generation generation) noexcept {
    observeTerminal(generation);
    EmitScope scope(this);
    emit coreStopped();
}

void BackendBridge::onStopCompleted(const StopCompleted &result) noexcept {
    // NEVER filtered. main.cpp blocks quit on this event and raises a shutdown
    // warning when !confirmed; dropping it wedges the quit path. See the
    // exemption note in the header and backend-r3 B1.
    observeTerminal(result.generation);
    EmitScope scope(this);
    emit stopFinished(result.confirmed, errorText(result.reason));
}

// ============================================================== attachment

void BackendBridge::onEndpointChanged(Generation generation, const Endpoint &endpoint,
                                    Ownership ownership) noexcept {
    if (!admit(generation, "endpointChanged")) return;
    endpoint_ = endpoint;
    endpointOwnership_ = ownership;
    EmitScope scope(this);
    emit endpointChanged(endpoint, ownership);
}

void BackendBridge::onConnectedChanged(Generation generation, bool connected) noexcept {
    if (!admit(generation, "connectedChanged")) return;
    connected_ = connected;
    EmitScope scope(this);
    emit connectedChanged(connected);
}

void BackendBridge::onConfigReceived(const Completion &completion,
                                   const BaseConfig &config) noexcept {
    if (!admit(completion, "configReceived")) return;
    if (!payloadOk(completion, ErrorChannel::Global)) return;
    EmitScope scope(this);
    emit configReceived(config);
}

// ================================================================== control

void BackendBridge::onModeChanged(const Completion &completion, const QString &mode) noexcept {
    if (!admit(completion, "modeChanged")) return;
    if (!payloadOk(completion, ErrorChannel::Global)) return;
    EmitScope scope(this);
    emit modeChanged(mode);
}

void BackendBridge::onTunChangeCompleted(const TunChangeCompleted &result) noexcept {
    if (!admit(result.generation, "tunChangeCompleted")) return;
    // Outcome-bearing: RoutingControls clears tunPending_ here and must hear
    // about a failure too. `actual` is the read-back, never an echo.
    EmitScope scope(this);
    emit tunChangeFinished(result.requested, result.actual, errorText(result.error));
}

void BackendBridge::onNodeSelected(const Completion &completion, const QString &group,
                                 const QString &node) noexcept {
    if (!admit(completion, "nodeSelected")) return;
    if (!payloadOk(completion, ErrorChannel::Global)) return;
    EmitScope scope(this);
    emit nodeSelected(group, node);
}

void BackendBridge::onGeoDatabasesUpdated(const Completion &completion) noexcept {
    if (!admit(completion, "geoDatabasesUpdated")) return;
    // Outcome-bearing: SettingsPage re-enables the button either way.
    EmitScope scope(this);
    emit geoDatabasesUpdated(errorText(completion));
}

void BackendBridge::onDnsQueryFinished(const Completion &completion, const QString &name,
                                     const QString &resultJson) noexcept {
    if (!admit(completion, "dnsQueryFinished")) return;
    // The parse belongs on this side of the boundary: contract section 9 keeps
    // QJsonObject out of the published surface, and HomePage reads the object.
    QJsonObject result;
    QString error = errorText(completion);
    if (completion.isOk()) {
        QJsonParseError parse{};
        const QJsonDocument document = QJsonDocument::fromJson(resultJson.toUtf8(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject())
            error = tr("The controller returned an unreadable DNS answer.");
        else
            result = document.object();
    }
    EmitScope scope(this);
    emit dnsQueryFinished(name, result, error);
}

void BackendBridge::onDnsCacheFlushed(const Completion &completion, bool fakeIp) noexcept {
    if (!admit(completion, "dnsCacheFlushed")) return;
    EmitScope scope(this);
    emit dnsCacheFlushed(fakeIp, errorText(completion));
}

// ================================================================ telemetry

void BackendBridge::onVersionReceived(const Completion &completion,
                                    const QString &version) noexcept {
    if (!admit(completion, "versionReceived")) return;
    if (!payloadOk(completion, ErrorChannel::Global)) return;
    EmitScope scope(this);
    emit versionReceived(version);
}

void BackendBridge::onProxiesUpdated(const Completion &completion, Span<ProxyGroup> groups,
                                   Span<ProxyNode> nodes) noexcept {
    if (!admit(completion, "proxiesUpdated")) return;
    // The live-state clear: two empty spans with RequestId::Invalid, published
    // when an endpoint change or a disconnect drops the snapshot. It is an Ok
    // completion, so it passes payloadOk and the empty containers below are the
    // point rather than a loss.
    const bool cleared = completion.request == RequestId::Invalid && groups.isEmpty() &&
                         nodes.isEmpty() && completion.isOk();
    if (!payloadOk(completion, ErrorChannel::Global)) return;
    // Copy at the boundary, before any emission.
    const QVector<ProxyGroup> ownedGroups = materialise(groups);
    const QHash<QString, ProxyNode> ownedNodes = materialiseNodes(nodes);
    EmitScope scope(this);
    emit proxiesUpdated(ownedGroups, ownedNodes);
    if (cleared) emit liveStateCleared();
}

void BackendBridge::onRulesUpdated(const Completion &completion, Span<Rule> rules) noexcept {
    if (!admit(completion, "rulesUpdated")) return;
    if (!payloadOk(completion, ErrorChannel::Global)) return;
    const QVector<Rule> owned = materialise(rules);
    EmitScope scope(this);
    emit rulesUpdated(owned);
}

void BackendBridge::onTrafficSample(Generation generation, quint64 up, quint64 down) noexcept {
    if (!admit(generation, "trafficSample")) return;
    EmitScope scope(this);
    emit trafficSample(up, down);
}

void BackendBridge::onMemorySample(Generation generation, quint64 inuse, quint64 oslimit) noexcept {
    if (!admit(generation, "memorySample")) return;
    EmitScope scope(this);
    emit memorySample(inuse, oslimit);
}

void BackendBridge::onConnectionsUpdated(Generation generation, Span<Connection> connections,
                                       quint64 uploadTotal, quint64 downloadTotal) noexcept {
    if (!admit(generation, "connectionsUpdated")) return;
    const QVector<Connection> owned = materialise(connections);
    EmitScope scope(this);
    emit connectionsUpdated(owned, uploadTotal, downloadTotal);
}

void BackendBridge::onLogReceived(Generation generation, const LogEntry &entry) noexcept {
    if (!admit(generation, "logReceived")) return;
    EmitScope scope(this);
    emit logReceived(entry);
}

// ================================================================ providers

void BackendBridge::onProvidersReceived(const Completion &completion, bool rules,
                                      Span<Provider> providers) noexcept {
    if (!admit(completion, "providersReceived")) return;
    if (!payloadOk(completion, ErrorChannel::Provider)) return;
    const QVector<Provider> owned = materialise(providers);
    EmitScope scope(this);
    emit providersReceived(rules, owned);
}

void BackendBridge::onProviderBusyChanged(Generation generation, bool busy) noexcept {
    if (!admit(generation, "providerBusyChanged")) return;
    providerBusy_ = busy;
    EmitScope scope(this);
    emit providerBusyChanged(busy);
}

void BackendBridge::onProviderOperationFinished(const Completion &completion,
                                              const QString &message) noexcept {
    if (!admit(completion, "providerOperationFinished")) return;
    if (!payloadOk(completion, ErrorChannel::Provider)) return;
    EmitScope scope(this);
    emit providerOperationFinished(message);
}

// ============================================================= capabilities

void BackendBridge::onPrivilegedServiceStatus(const Completion &completion,
                                            const PrivilegedServiceStatus &status) noexcept {
    if (!admit(completion, "privilegedServiceStatus")) return;
    // Outcome-bearing: ServiceSettings must render "could not ask" as a state,
    // not as silence.
    const QString error =
        completion.isOk() ? errorText(status.error) : errorText(completion);
    EmitScope scope(this);
    emit privilegedServiceStatus(status.state, status.version, error, status.coreRunning);
}

// =================================================================== errors

void BackendBridge::onErrorOccurred(const Completion &completion) noexcept {
    if (!admit(completion, "errorOccurred")) return;
    // The one place a failure becomes visible. If a provider operation already
    // claimed this RequestId, the failure is that operation's and belongs on
    // the provider channel, which ProvidersPage owns; otherwise it is global.
    const bool provider = completion.request != RequestId::Invalid &&
                          providerFailures_.remove(number(completion.request));
    EmitScope scope(this);
    if (provider)
        emit providerError(errorText(completion));
    else
        emit errorOccurred(errorText(completion));
}

// ================================================================== intent
//
// Thin forwards. Nothing here is called from a callback, so none of them can
// close a re-entrancy loop; a consumer that calls one from inside a handler is
// re-entering the BACKEND, which queues what it produces and delivers after
// returning.

RequestId BackendBridge::startCore(const QString &configPath, const QString &workDir) {
    return backend_.start(configPath, workDir);
}
RequestId BackendBridge::stopCore() { return backend_.stop(); }

RequestId BackendBridge::attach(const Endpoint &endpoint) { return backend_.attach(endpoint); }
RequestId BackendBridge::detach() { return backend_.detach(); }
RequestId BackendBridge::refreshConfig() { return backend_.refreshConfig(); }

RequestId BackendBridge::setMode(const QString &mode) { return backend_.setMode(mode); }
RequestId BackendBridge::setTunEnabled(bool enabled) { return backend_.setTunEnabled(enabled); }
RequestId BackendBridge::selectNode(const QString &group, const QString &node) {
    return backend_.selectNode(group, node);
}
RequestId BackendBridge::resetGroupSelection(const QString &group) {
    return backend_.resetGroupSelection(group);
}
RequestId BackendBridge::testGroupDelay(const QString &group) {
    return backend_.testGroupDelay(group);
}
RequestId BackendBridge::testNodeDelay(const QString &node) { return backend_.testNodeDelay(node); }
RequestId BackendBridge::closeConnection(const QString &id) { return backend_.closeConnection(id); }
RequestId BackendBridge::closeAllConnections() { return backend_.closeAllConnections(); }
RequestId BackendBridge::updateGeoDatabases() { return backend_.updateGeoDatabases(); }
RequestId BackendBridge::queryDns(const QString &name, const QString &type) {
    return backend_.queryDns(name, type);
}
RequestId BackendBridge::flushDnsCache(bool fakeIp) { return backend_.flushDnsCache(fakeIp); }

RequestId BackendBridge::refreshVersion() { return backend_.refreshVersion(); }
RequestId BackendBridge::refreshProxies() { return backend_.refreshProxies(); }
RequestId BackendBridge::refreshRules() { return backend_.refreshRules(); }
RequestId BackendBridge::openTrafficStream() { return backend_.openTrafficStream(); }
RequestId BackendBridge::closeTrafficStream() { return backend_.closeTrafficStream(); }
RequestId BackendBridge::openConnectionsStream() { return backend_.openConnectionsStream(); }
RequestId BackendBridge::closeConnectionsStream() { return backend_.closeConnectionsStream(); }
RequestId BackendBridge::openLogStream(const QString &level) {
    return backend_.openLogStream(level);
}
RequestId BackendBridge::closeLogStream() { return backend_.closeLogStream(); }
RequestId BackendBridge::openMemoryStream() { return backend_.openMemoryStream(); }
RequestId BackendBridge::closeMemoryStream() { return backend_.closeMemoryStream(); }

RequestId BackendBridge::fetchProviders(bool rules) { return backend_.fetchProviders(rules); }
RequestId BackendBridge::updateProvider(bool rules, const QString &name) {
    return backend_.updateProvider(rules, name);
}
RequestId BackendBridge::healthCheckProvider(const QString &name) {
    return backend_.healthCheckProvider(name);
}

RequestId BackendBridge::requestPrivilegedServiceStatus() {
    return backend_.requestPrivilegedServiceStatus();
}

}  // namespace core::backend
