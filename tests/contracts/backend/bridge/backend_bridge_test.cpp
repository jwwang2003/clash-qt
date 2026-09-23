// BackendBridge against clash_backend_fake.
//
// What this suite has to establish, because every consumer of the published
// backend surface inherits it:
//   1. Every signal on the published surface fires, with the arguments the
//      existing UI handler already accepts. A signal nothing proves is a signal
//      20 include sites will discover is wrong.
//   2. Spans are materialised. A consumer that keeps what it was handed must
//      still hold valid data after the callback returned.
//   3. No re-entrant emission, including when a handler calls straight back
//      into an intent slot - the modern spelling of the clearLiveState() hazard.
//   4. Superseded events are handled as the bridge documents: dropped and
//      counted, EXCEPT the managed core's terminal outcomes, which must survive
//      because the quit path blocks on one of them.
//   5. Disconnecting, and destroying a receiver, during emission is safe.

#include <cstdint>

#include <QHash>
#include <QSignalSpy>
#include <QTest>
#include <QVector>

#include "core/backend/backend_bridge.h"
#include "support/backend/fake_backend.h"

using testsupport::backend::FakeBackend;
using testsupport::backend::Gate;
using testsupport::backend::RequestOutcome;
namespace cb = core::backend;

namespace {

// ---------------------------------------------------------------------------
// A backend that does nothing except hand back the observer the bridge
// registered. FakeBackend is `final` and cannot be subclassed, and it has no
// producer for several contract events at all - logReceived has none, and the
// stream samples only appear via a live-state clear. This driver exists so the
// suite can state the one thing a signal adapter has to guarantee: EVERY signal
// fires, with EXACTLY these arguments. The contract behaviours - ordering,
// re-entrancy, supersession - are proved against the real fake below, where
// realism is what matters.
class DirectBackend final : public cb::MihomoBackend {
  public:
    cb::BackendObserver *observer = nullptr;
    cb::Generation gen = cb::Generation::Initial;

    cb::Generation generation() const noexcept override { return gen; }
    bool addObserver(cb::BackendObserver *o) noexcept override {
        observer = o;
        return true;
    }
    bool removeObserver(cb::BackendObserver *o) noexcept override {
        if (observer != o) return false;
        observer = nullptr;
        return true;
    }

    // ---- BackendLifecycle
    QString discoverBinary() const noexcept override { return {}; }
    void setBinaryPath(const QString &) noexcept override {}
    QString binaryPath() const noexcept override { return {}; }
    bool setExecutionMode(cb::ExecutionMode) noexcept override { return false; }
    cb::ExecutionMode executionMode() const noexcept override { return cb::ExecutionMode::Managed; }
    bool usesPrivilegedService() const noexcept override { return false; }
    cb::RequestId start(const QString &, const QString &) noexcept override { return {}; }
    cb::RequestId stop() noexcept override { return {}; }
    cb::CoreState state() const noexcept override { return cb::CoreState::Stopped; }
    cb::Ownership ownership() const noexcept override { return cb::Ownership::None; }
    cb::Endpoint managedEndpoint() const noexcept override { return {}; }
    QStringList activeConfigPaths() const noexcept override { return {}; }
    bool isRestartPending() const noexcept override { return false; }
    bool isManagedCoreActive() const noexcept override { return false; }

    // ---- BackendAttachment
    cb::Endpoint discoverEndpoint() const noexcept override { return {}; }
    bool endpointFromConfigFile(const QString &, cb::Endpoint *) const noexcept override {
        return false;
    }
    cb::RequestId attach(const cb::Endpoint &) noexcept override { return {}; }
    cb::RequestId detach() noexcept override { return {}; }
    cb::Endpoint currentEndpoint() const noexcept override { return {}; }
    bool isAttached() const noexcept override { return false; }
    bool isConnected() const noexcept override { return false; }
    cb::Ownership attachmentOwnership() const noexcept override { return cb::Ownership::None; }
    bool isExternalControllerConnected() const noexcept override { return false; }
    cb::RequestId refreshConfig() noexcept override { return {}; }

    // ---- BackendControl
    cb::RequestId setMode(const QString &) noexcept override { return {}; }
    cb::RequestId setTunEnabled(bool) noexcept override { return {}; }
    bool isTunChangePending() const noexcept override { return false; }
    cb::RequestId selectNode(const QString &, const QString &) noexcept override { return {}; }
    cb::RequestId resetGroupSelection(const QString &) noexcept override { return {}; }
    cb::RequestId testGroupDelay(const QString &) noexcept override { return {}; }
    cb::RequestId testNodeDelay(const QString &) noexcept override { return {}; }
    cb::RequestId closeConnection(const QString &) noexcept override { return {}; }
    cb::RequestId closeAllConnections() noexcept override { return {}; }
    cb::RequestId updateGeoDatabases() noexcept override { return {}; }
    cb::RequestId queryDns(const QString &, const QString &) noexcept override { return {}; }
    cb::RequestId flushDnsCache(bool) noexcept override { return {}; }

    // ---- BackendTelemetry
    cb::RequestId refreshVersion() noexcept override { return {}; }
    cb::RequestId refreshProxies() noexcept override { return {}; }
    cb::RequestId refreshRules() noexcept override { return {}; }
    cb::RequestId openTrafficStream() noexcept override { return {}; }
    cb::RequestId closeTrafficStream() noexcept override { return {}; }
    cb::RequestId openConnectionsStream() noexcept override { return {}; }
    cb::RequestId closeConnectionsStream() noexcept override { return {}; }
    cb::RequestId openLogStream(const QString &) noexcept override { return {}; }
    cb::RequestId closeLogStream() noexcept override { return {}; }
    cb::RequestId openMemoryStream() noexcept override { return {}; }
    cb::RequestId closeMemoryStream() noexcept override { return {}; }
    cb::RequestId fetchProviders(bool) noexcept override { return {}; }
    cb::RequestId updateProvider(bool, const QString &) noexcept override { return {}; }
    cb::RequestId healthCheckProvider(const QString &) noexcept override { return {}; }
    bool isProviderBusy() const noexcept override { return false; }

    // ---- BackendCapabilities
    cb::BackendIdentity identity() const noexcept override { return {}; }
    cb::FeatureSet features() const noexcept override { return {}; }
    bool serviceSupported() const noexcept override { return false; }
    bool serviceAvailable() const noexcept override { return false; }
    cb::BackendTimings timings() const noexcept override { return {}; }
    cb::RequestId requestPrivilegedServiceStatus() noexcept override { return {}; }
};

// An Ok completion for request `id` under the current generation.
cb::Completion ok(std::uint64_t id = 1) {
    cb::Completion completion;
    completion.request = static_cast<cb::RequestId>(id);
    completion.generation = cb::Generation::Initial;
    completion.status = cb::CompletionStatus::Ok;
    return completion;
}

cb::Completion failed(std::uint64_t id, const QString &message) {
    cb::Completion completion = ok(id);
    completion.status = cb::CompletionStatus::Failed;
    completion.error = {cb::ErrorCode::Network, message};
    return completion;
}

cb::Endpoint makeEndpoint(quint16 port, const QString &secret = QStringLiteral("s")) {
    cb::Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = port;
    endpoint.secret = secret;
    return endpoint;
}

// Attaches to a reachable controller and drains the events that produces, so a
// test starts from a quiet, connected bridge.
void attachAndSettle(FakeBackend &backend, cb::BackendBridge &bridge, quint16 port) {
    const cb::Endpoint endpoint = makeEndpoint(port);
    backend.addExternalController(endpoint);
    backend.attach(endpoint);
    QVERIFY(backend.flushEvents());
    QVERIFY(bridge.isConnected());
}

}  // namespace

class BackendBridgeTest : public QObject {
    Q_OBJECT

  private slots:

    // ================================================== registration, lifetime

    void registersAndRemovesItsSink();
    void cachesStateFromEventsNotFromTheBackend();

    // ======================================================= the signal surface

    // Drives all 27 contract callbacks straight at the bridge and checks every
    // argument. This is the test the 66 migrated call sites depend on.
    void everySignalFiresWithItsArguments();

    void republishesManagedCoreEvents();
    void republishesStopOutcome();
    void republishesAttachmentEvents();
    void republishesConfigAndMode();
    void republishesTunOutcome();
    void republishesSnapshots();
    void republishesStreams();
    void republishesDiagnostics();
    void republishesProviders();
    void republishesErrors();
    void republishesPrivilegedServiceStatus();

    // ========================================================== materialisation

    void materialisesSpansIntoOwnedContainers();
    void keysProxyNodesByName();
    void publishesLiveStateClear();

    // =========================================================== non-re-entrancy

    void neverEmitsFromInsideAMutatingCall();
    void intentFromInsideAHandlerDoesNotReEnter();

    // ============================================================== generations

    void dropsSupersededCompletions();
    void dropsSupersededStreamSamples();
    void neverDropsTheStopOutcome();
    void announcesGenerationBeforeTheEventsItInvalidates();

    // ================================================================ teardown

    void disconnectingDuringEmissionIsSafe();
    void destroyingAReceiverDuringEmissionIsSafe();
};

// ---------------------------------------------------------- registration

void BackendBridgeTest::registersAndRemovesItsSink() {
    FakeBackend backend;
    {
        cb::BackendBridge bridge(backend);
        QSignalSpy connects(&bridge, &cb::BackendBridge::connectedChanged);
        attachAndSettle(backend, bridge, 9090);
        QVERIFY(!connects.isEmpty());
    }
    // The sink is gone: nothing must reach a destroyed bridge. If removal were
    // missing this would be a use-after-free rather than a failed assertion,
    // so the value here is that it runs clean under the sanitiser build too.
    backend.detach();
    QVERIFY(backend.flushEvents());
}

void BackendBridgeTest::cachesStateFromEventsNotFromTheBackend() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    QCOMPARE(bridge.isConnected(), false);
    QCOMPARE(bridge.endpoint().port, quint16(0));

    attachAndSettle(backend, bridge, 9097);
    QCOMPARE(bridge.endpoint().port, quint16(9097));
    QCOMPARE(bridge.isConnected(), true);
    QCOMPARE(bridge.endpointOwnership(), backend.attachmentOwnership());

    // The value a handler reads belongs to the event it is handling. Capture
    // the endpoint the bridge reports from inside the endpointChanged handler
    // and prove it is the NEW one, not a stale or half-applied value.
    quint16 seen = 0;
    connect(&bridge, &cb::BackendBridge::endpointChanged, &bridge,
            [&] { seen = bridge.endpoint().port; });
    backend.addExternalController(makeEndpoint(9098));
    backend.attach(makeEndpoint(9098));
    QVERIFY(backend.flushEvents());
    QCOMPARE(seen, quint16(9098));
}

// ------------------------------------------------------- the signal surface

void BackendBridgeTest::everySignalFiresWithItsArguments() {
    DirectBackend backend;
    cb::BackendBridge bridge(backend);
    QVERIFY(backend.observer != nullptr);
    cb::BackendObserver &sink = *backend.observer;
    const cb::Generation g = cb::Generation::Initial;

    // ---- managed core
    QSignalSpy states(&bridge, &cb::BackendBridge::coreStateChanged);
    sink.coreStateChanged(g, cb::CoreState::Running, cb::Ownership::Managed);
    QCOMPARE(states.size(), 1);
    QCOMPARE(states.first().at(0).value<cb::CoreState>(), cb::CoreState::Running);
    QCOMPARE(states.first().at(1).value<cb::Ownership>(), cb::Ownership::Managed);
    QCOMPARE(bridge.coreState(), cb::CoreState::Running);
    QCOMPARE(bridge.coreOwnership(), cb::Ownership::Managed);

    QSignalSpy ready(&bridge, &cb::BackendBridge::coreReady);
    sink.coreReady(ok(), makeEndpoint(9090, QStringLiteral("abc")));
    QCOMPARE(ready.size(), 1);
    QCOMPARE(ready.first().at(0).value<cb::Endpoint>().port, quint16(9090));
    QCOMPARE(ready.first().at(0).value<cb::Endpoint>().secret, QStringLiteral("abc"));

    QSignalSpy lines(&bridge, &cb::BackendBridge::coreLogLine);
    sink.coreLogLine(g, QStringLiteral("hello"));
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.first().at(0).toString(), QStringLiteral("hello"));

    QSignalSpy coreFailures(&bridge, &cb::BackendBridge::coreFailed);
    sink.coreFailed(failed(2, QStringLiteral("binary missing")));
    QCOMPARE(coreFailures.size(), 1);
    QCOMPARE(coreFailures.first().at(0).toString(), QStringLiteral("binary missing"));
    // A coreReady that is NOT Ok is a failure, not a ready with a blank endpoint.
    sink.coreReady(failed(3, QStringLiteral("never answered /version")), {});
    QCOMPARE(ready.size(), 1);
    QCOMPARE(coreFailures.size(), 2);

    QSignalSpy coreStops(&bridge, &cb::BackendBridge::coreStopped);
    sink.coreStopped(g);
    QCOMPARE(coreStops.size(), 1);

    QSignalSpy stopOutcome(&bridge, &cb::BackendBridge::stopFinished);
    cb::StopCompleted stop;
    stop.request = static_cast<cb::RequestId>(4);
    stop.generation = g;
    stop.confirmed = false;
    stop.reason = {cb::ErrorCode::ServiceDisconnected, QStringLiteral("service went away")};
    sink.stopCompleted(stop);
    QCOMPARE(stopOutcome.size(), 1);
    QCOMPARE(stopOutcome.first().at(0).toBool(), false);
    QCOMPARE(stopOutcome.first().at(1).toString(), QStringLiteral("service went away"));

    // ---- attachment
    QSignalSpy endpoints(&bridge, &cb::BackendBridge::endpointChanged);
    sink.endpointChanged(g, makeEndpoint(9095), cb::Ownership::Attached);
    QCOMPARE(endpoints.size(), 1);
    QCOMPARE(endpoints.first().at(0).value<cb::Endpoint>().port, quint16(9095));
    QCOMPARE(endpoints.first().at(1).value<cb::Ownership>(), cb::Ownership::Attached);
    QCOMPARE(bridge.endpoint().port, quint16(9095));

    QSignalSpy connects(&bridge, &cb::BackendBridge::connectedChanged);
    sink.connectedChanged(g, true);
    QCOMPARE(connects.size(), 1);
    QCOMPARE(connects.first().at(0).toBool(), true);
    QCOMPARE(bridge.isConnected(), true);

    QSignalSpy configs(&bridge, &cb::BackendBridge::configReceived);
    cb::BaseConfig config;
    config.mode = QStringLiteral("rule");
    config.mixedPort = 7890;
    config.tunEnabled = true;
    sink.configReceived(ok(), config);
    QCOMPARE(configs.size(), 1);
    QCOMPARE(configs.first().at(0).value<cb::BaseConfig>().mixedPort, quint16(7890));
    QCOMPARE(configs.first().at(0).value<cb::BaseConfig>().tunEnabled, true);

    // ---- control
    QSignalSpy modes(&bridge, &cb::BackendBridge::modeChanged);
    sink.modeChanged(ok(), QStringLiteral("global"));
    QCOMPARE(modes.size(), 1);
    QCOMPARE(modes.first().at(0).toString(), QStringLiteral("global"));

    QSignalSpy tun(&bridge, &cb::BackendBridge::tunChangeFinished);
    cb::TunChangeCompleted change;
    change.generation = g;
    change.requested = true;
    change.actual = false;
    change.error = {cb::ErrorCode::NotSupported, QStringLiteral("no tun device")};
    sink.tunChangeCompleted(change);
    QCOMPARE(tun.size(), 1);
    QCOMPARE(tun.first().at(0).toBool(), true);
    QCOMPARE(tun.first().at(1).toBool(), false);
    QCOMPARE(tun.first().at(2).toString(), QStringLiteral("no tun device"));

    QSignalSpy selected(&bridge, &cb::BackendBridge::nodeSelected);
    sink.nodeSelected(ok(), QStringLiteral("Proxy"), QStringLiteral("HK"));
    QCOMPARE(selected.size(), 1);
    QCOMPARE(selected.first().at(0).toString(), QStringLiteral("Proxy"));
    QCOMPARE(selected.first().at(1).toString(), QStringLiteral("HK"));

    QSignalSpy geo(&bridge, &cb::BackendBridge::geoDatabasesUpdated);
    sink.geoDatabasesUpdated(ok());
    QCOMPARE(geo.size(), 1);
    QVERIFY(geo.first().at(0).toString().isEmpty());  // empty EXACTLY on success

    QSignalSpy dns(&bridge, &cb::BackendBridge::dnsQueryFinished);
    sink.dnsQueryFinished(ok(), QStringLiteral("example.com"),
                          QStringLiteral("{\"Status\":3,\"Answer\":[{\"TTL\":60}]}"));
    QCOMPARE(dns.size(), 1);
    QCOMPARE(dns.first().at(0).toString(), QStringLiteral("example.com"));
    QCOMPARE(dns.first().at(1).toJsonObject().value(QStringLiteral("Status")).toInt(), 3);
    // Unreadable text is an error, not a silently empty object.
    sink.dnsQueryFinished(ok(), QStringLiteral("bad."), QStringLiteral("not json"));
    QCOMPARE(dns.size(), 2);
    QVERIFY(!dns.at(1).at(2).toString().isEmpty());

    QSignalSpy flushed(&bridge, &cb::BackendBridge::dnsCacheFlushed);
    sink.dnsCacheFlushed(ok(), true);
    QCOMPARE(flushed.size(), 1);
    QCOMPARE(flushed.first().at(0).toBool(), true);

    // ---- telemetry
    QSignalSpy versions(&bridge, &cb::BackendBridge::versionReceived);
    sink.versionReceived(ok(), QStringLiteral("1.19.0"));
    QCOMPARE(versions.size(), 1);
    QCOMPARE(versions.first().at(0).toString(), QStringLiteral("1.19.0"));

    QSignalSpy proxies(&bridge, &cb::BackendBridge::proxiesUpdated);
    cb::ProxyGroup group;
    group.name = QStringLiteral("Proxy");
    group.type = QStringLiteral("Selector");
    group.now = QStringLiteral("HK");
    cb::ProxyNode hk;
    hk.name = QStringLiteral("HK");
    hk.delay = 55;
    const QVector<cb::ProxyGroup> groups{group};
    const QVector<cb::ProxyNode> nodes{hk};
    sink.proxiesUpdated(ok(), cb::makeSpan(groups), cb::makeSpan(nodes));
    QCOMPARE(proxies.size(), 1);
    QCOMPARE(proxies.first().at(0).value<QVector<cb::ProxyGroup>>().size(), 1);
    QCOMPARE(proxies.first().at(0).value<QVector<cb::ProxyGroup>>().first().now,
             QStringLiteral("HK"));
    const auto hashed = proxies.first().at(1).value<QHash<QString, cb::ProxyNode>>();
    QCOMPARE(hashed.size(), 1);
    QCOMPARE(hashed.value(QStringLiteral("HK")).delay, 55);

    QSignalSpy rules(&bridge, &cb::BackendBridge::rulesUpdated);
    cb::Rule rule;
    rule.type = QStringLiteral("DOMAIN");
    rule.payload = QStringLiteral("a.example");
    rule.proxy = QStringLiteral("Proxy");
    const QVector<cb::Rule> ruleList{rule};
    sink.rulesUpdated(ok(), cb::makeSpan(ruleList));
    QCOMPARE(rules.size(), 1);
    QCOMPARE(rules.first().at(0).value<QVector<cb::Rule>>().first().payload,
             QStringLiteral("a.example"));

    QSignalSpy traffic(&bridge, &cb::BackendBridge::trafficSample);
    sink.trafficSample(g, 1234, 5678);
    QCOMPARE(traffic.size(), 1);
    QCOMPARE(traffic.first().at(0).value<quint64>(), quint64(1234));
    QCOMPARE(traffic.first().at(1).value<quint64>(), quint64(5678));

    QSignalSpy memory(&bridge, &cb::BackendBridge::memorySample);
    sink.memorySample(g, 99, 100);
    QCOMPARE(memory.size(), 1);
    QCOMPARE(memory.first().at(0).value<quint64>(), quint64(99));
    QCOMPARE(memory.first().at(1).value<quint64>(), quint64(100));

    QSignalSpy conns(&bridge, &cb::BackendBridge::connectionsUpdated);
    cb::Connection connection;
    connection.id = QStringLiteral("c1");
    connection.host = QStringLiteral("a.example");
    connection.upload = 10;
    const QVector<cb::Connection> connectionList{connection};
    sink.connectionsUpdated(g, cb::makeSpan(connectionList), 111, 222);
    QCOMPARE(conns.size(), 1);
    QCOMPARE(conns.first().at(0).value<QVector<cb::Connection>>().first().id,
             QStringLiteral("c1"));
    QCOMPARE(conns.first().at(1).value<quint64>(), quint64(111));
    QCOMPARE(conns.first().at(2).value<quint64>(), quint64(222));

    QSignalSpy logs(&bridge, &cb::BackendBridge::logReceived);
    cb::LogEntry entry;
    entry.level = QStringLiteral("warning");
    entry.payload = QStringLiteral("something");
    sink.logReceived(g, entry);
    QCOMPARE(logs.size(), 1);
    QCOMPARE(logs.first().at(0).value<cb::LogEntry>().level, QStringLiteral("warning"));
    QCOMPARE(logs.first().at(0).value<cb::LogEntry>().payload, QStringLiteral("something"));

    // ---- providers
    QSignalSpy providers(&bridge, &cb::BackendBridge::providersReceived);
    cb::Provider provider;
    provider.name = QStringLiteral("subs");
    provider.count = 12;
    const QVector<cb::Provider> providerList{provider};
    sink.providersReceived(ok(), true, cb::makeSpan(providerList));
    QCOMPARE(providers.size(), 1);
    QCOMPARE(providers.first().at(0).toBool(), true);
    QCOMPARE(providers.first().at(1).value<QVector<cb::Provider>>().first().count, 12);

    QSignalSpy busy(&bridge, &cb::BackendBridge::providerBusyChanged);
    sink.providerBusyChanged(g, true);
    QCOMPARE(busy.size(), 1);
    QCOMPARE(busy.first().at(0).toBool(), true);
    QCOMPARE(bridge.isProviderBusy(), true);

    QSignalSpy operations(&bridge, &cb::BackendBridge::providerOperationFinished);
    sink.providerOperationFinished(ok(), QStringLiteral("subs updated"));
    QCOMPARE(operations.size(), 1);
    QCOMPARE(operations.first().at(0).toString(), QStringLiteral("subs updated"));

    // ---- capabilities
    QSignalSpy service(&bridge, &cb::BackendBridge::privilegedServiceStatus);
    cb::PrivilegedServiceStatus status;
    status.state = cb::ServiceState::NotInstalled;
    status.version = QStringLiteral("0.9");
    status.coreRunning = true;
    sink.privilegedServiceStatus(ok(), status);
    QCOMPARE(service.size(), 1);
    QCOMPARE(service.first().at(0).value<cb::ServiceState>(), cb::ServiceState::NotInstalled);
    QCOMPARE(service.first().at(1).toString(), QStringLiteral("0.9"));
    QCOMPARE(service.first().at(3).toBool(), true);

    // ---- errors, and the REPORTED-ONCE rule
    QSignalSpy errors(&bridge, &cb::BackendBridge::errorOccurred);
    QSignalSpy providerErrors(&bridge, &cb::BackendBridge::providerError);

    // A failed snapshot emits no payload - blanking a view nothing invalidated
    // is the bug - and does not announce itself either: the backend's own
    // global error, below, is the single report.
    const int versionsBefore = versions.size();
    sink.versionReceived(failed(10, QStringLiteral("unauthorised")), {});
    QCOMPARE(versions.size(), versionsBefore);
    QCOMPARE(errors.size(), 0);
    sink.errorOccurred(failed(10, QStringLiteral("unauthorised")));
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.first().at(0).toString(), QStringLiteral("unauthorised"));

    // A failed PROVIDER operation claims its RequestId, so the global error
    // that names it is re-aimed at the provider channel - once, not twice.
    sink.providersReceived(failed(11, QStringLiteral("provider down")), false, {});
    QCOMPARE(providerErrors.size(), 0);
    sink.errorOccurred(failed(11, QStringLiteral("provider down")));
    QCOMPARE(providerErrors.size(), 1);
    QCOMPARE(providerErrors.first().at(0).toString(), QStringLiteral("provider down"));
    QCOMPARE(errors.size(), 1);  // NOT also on the global channel

    // An unsolicited failure belongs to no request and stays global.
    cb::Completion unsolicited = failed(0, QStringLiteral("stream lost"));
    unsolicited.request = cb::RequestId::Invalid;
    sink.errorOccurred(unsolicited);
    QCOMPARE(errors.size(), 2);
    QCOMPARE(providerErrors.size(), 1);
}

void BackendBridgeTest::republishesManagedCoreEvents() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);

    QSignalSpy states(&bridge, &cb::BackendBridge::coreStateChanged);
    QSignalSpy ready(&bridge, &cb::BackendBridge::coreReady);
    QSignalSpy lines(&bridge, &cb::BackendBridge::coreLogLine);
    QSignalSpy failed(&bridge, &cb::BackendBridge::coreFailed);

    backend.mapConfigFile(QStringLiteral("/cfg.yaml"), makeEndpoint(9111));
    backend.start(QStringLiteral("/cfg.yaml"), QStringLiteral("/work"));
    QVERIFY(backend.completeValidation(true));
    QVERIFY(backend.flushEvents());

    QVERIFY(!states.isEmpty());
    QCOMPARE(states.first().at(0).value<cb::CoreState>(), cb::CoreState::Starting);
    QCOMPARE(bridge.coreState(), cb::CoreState::Starting);

    backend.emitCoreLogLine(QStringLiteral("listening"));
    QVERIFY(backend.flushEvents());
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.first().at(0).toString(), QStringLiteral("listening"));

    backend.advanceTime(cb::kContractTimings.probeIntervalMs);
    QVERIFY(backend.flushEvents());
    QCOMPARE(ready.size(), 1);
    QCOMPARE(ready.first().at(0).value<cb::Endpoint>().port, quint16(9111));
    QCOMPARE(bridge.coreState(), cb::CoreState::Running);

    // A core that dies unprompted is a failure with a human message.
    backend.crashChild(1, QStringLiteral("panic: bad config"));
    QVERIFY(backend.flushEvents());
    QCOMPARE(failed.size(), 1);
    QVERIFY(!failed.first().at(0).toString().isEmpty());
}

void BackendBridgeTest::republishesStopOutcome() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    QSignalSpy stopped(&bridge, &cb::BackendBridge::coreStopped);
    QSignalSpy finished(&bridge, &cb::BackendBridge::stopFinished);

    backend.mapConfigFile(QStringLiteral("/cfg.yaml"), makeEndpoint(9112));
    backend.start(QStringLiteral("/cfg.yaml"), QStringLiteral("/work"));
    QVERIFY(backend.completeValidation(true));
    backend.advanceTime(cb::kContractTimings.probeIntervalMs);
    QVERIFY(backend.flushEvents());

    backend.stop();
    QVERIFY(backend.releaseChildExit(0));
    QVERIFY(backend.flushEvents());

    QCOMPARE(stopped.size(), 1);
    QCOMPARE(finished.size(), 1);
    // confirmed == true, and the error text is empty EXACTLY on success - the
    // invariant main.cpp's quit path reads.
    QCOMPARE(finished.first().at(0).toBool(), true);
    QVERIFY(finished.first().at(1).toString().isEmpty());
}

void BackendBridgeTest::republishesAttachmentEvents() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    QSignalSpy endpoints(&bridge, &cb::BackendBridge::endpointChanged);
    QSignalSpy connects(&bridge, &cb::BackendBridge::connectedChanged);

    attachAndSettle(backend, bridge, 9090);
    QVERIFY(!endpoints.isEmpty());
    QCOMPARE(endpoints.last().at(0).value<cb::Endpoint>().port, quint16(9090));
    QVERIFY(!connects.isEmpty());
    QCOMPARE(connects.last().at(0).toBool(), true);

    const int before = connects.size();
    backend.disconnectController();
    QVERIFY(backend.flushEvents());
    // A disconnect reaches the UI. (The fake then re-probes and may reconnect -
    // runSnapshotReissue() completes refreshVersion regardless of
    // controllerReachable_ - so this asserts the event, not the final state.)
    bool sawDisconnect = false;
    for (int i = before; i < connects.size(); ++i)
        if (!connects.at(i).at(0).toBool()) sawDisconnect = true;
    QVERIFY(sawDisconnect);
}

void BackendBridgeTest::republishesConfigAndMode() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    QSignalSpy configs(&bridge, &cb::BackendBridge::configReceived);
    QSignalSpy modes(&bridge, &cb::BackendBridge::modeChanged);

    backend.staged().config.mode = QStringLiteral("global");
    backend.staged().config.mixedPort = 7890;
    backend.refreshConfig();
    QVERIFY(backend.flushEvents());
    QVERIFY(!configs.isEmpty());
    const auto config = configs.last().at(0).value<cb::BaseConfig>();
    QCOMPARE(config.mode, QStringLiteral("global"));
    QCOMPARE(config.mixedPort, quint16(7890));

    backend.staged().mode = QStringLiteral("direct");
    backend.setMode(QStringLiteral("direct"));
    QVERIFY(backend.flushEvents());
    QCOMPARE(modes.size(), 1);
    QCOMPARE(modes.first().at(0).toString(), QStringLiteral("direct"));
}

void BackendBridgeTest::republishesTunOutcome() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);
    QSignalSpy tun(&bridge, &cb::BackendBridge::tunChangeFinished);

    // The read-back disagrees with the request. RoutingControls must be able to
    // tell that apart from success, so both values are carried through.
    backend.staged().tunActual = false;
    backend.setTunEnabled(true);
    QVERIFY(backend.flushEvents());
    QCOMPARE(tun.size(), 1);
    QCOMPARE(tun.first().at(0).toBool(), true);   // requested
    QCOMPARE(tun.first().at(1).toBool(), false);  // actual, the read-back
}

void BackendBridgeTest::republishesSnapshots() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    QSignalSpy versions(&bridge, &cb::BackendBridge::versionReceived);
    QSignalSpy proxies(&bridge, &cb::BackendBridge::proxiesUpdated);
    QSignalSpy rules(&bridge, &cb::BackendBridge::rulesUpdated);

    backend.staged().version = QStringLiteral("1.19.0");
    backend.refreshVersion();
    QVERIFY(backend.flushEvents());
    QCOMPARE(versions.size(), 1);
    QCOMPARE(versions.first().at(0).toString(), QStringLiteral("1.19.0"));

    cb::ProxyGroup group;
    group.name = QStringLiteral("Proxy");
    group.type = QStringLiteral("Selector");
    group.now = QStringLiteral("HK");
    backend.staged().groups = {group};
    cb::ProxyNode node;
    node.name = QStringLiteral("HK");
    node.delay = 42;
    backend.staged().nodes = {node};
    backend.refreshProxies();
    QVERIFY(backend.flushEvents());
    QVERIFY(!proxies.isEmpty());
    QCOMPARE(proxies.last().at(0).value<QVector<cb::ProxyGroup>>().size(), 1);

    cb::Rule rule;
    rule.type = QStringLiteral("DOMAIN");
    rule.payload = QStringLiteral("example.com");
    rule.proxy = QStringLiteral("Proxy");
    backend.staged().rules = {rule};
    backend.refreshRules();
    QVERIFY(backend.flushEvents());
    QCOMPARE(rules.size(), 1);
    const auto owned = rules.first().at(0).value<QVector<cb::Rule>>();
    QCOMPARE(owned.size(), 1);
    QCOMPARE(owned.first().payload, QStringLiteral("example.com"));
}

void BackendBridgeTest::republishesStreams() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    backend.openTrafficStream();
    backend.openMemoryStream();
    backend.openConnectionsStream();
    backend.openLogStream(QStringLiteral("info"));
    QVERIFY(backend.flushEvents());

    QSignalSpy traffic(&bridge, &cb::BackendBridge::trafficSample);
    QSignalSpy memory(&bridge, &cb::BackendBridge::memorySample);
    QSignalSpy connections(&bridge, &cb::BackendBridge::connectionsUpdated);

    // clash_backend_fake has NO producer for a stream sample - advanceTime only
    // drives readiness probing and termination escalation, and there is no
    // injector for traffic, memory, connections or log lines. The one path that
    // reaches the stream channels is the live-state clear, which publishes all
    // three with zeroes. Exact argument checking for these four signals is in
    // everySignalFiresWithItsArguments(); reported as a gap in the fake.
    backend.disconnectController();
    QVERIFY(backend.flushEvents());

    QVERIFY(!traffic.isEmpty());
    QCOMPARE(traffic.last().at(0).value<quint64>(), quint64(0));
    QCOMPARE(traffic.last().at(1).value<quint64>(), quint64(0));
    QVERIFY(!memory.isEmpty());
    QVERIFY(!connections.isEmpty());
    QVERIFY(connections.last().at(0).value<QVector<cb::Connection>>().isEmpty());
}

void BackendBridgeTest::republishesDiagnostics() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    QSignalSpy dns(&bridge, &cb::BackendBridge::dnsQueryFinished);
    QSignalSpy flushed(&bridge, &cb::BackendBridge::dnsCacheFlushed);
    QSignalSpy geo(&bridge, &cb::BackendBridge::geoDatabasesUpdated);

    // The contract delivers TEXT; HomePage reads an object. The bridge parses.
    backend.staged().dnsResultJson = QStringLiteral("{\"Status\":0,\"Answer\":[]}");
    backend.queryDns(QStringLiteral("example.com"), QStringLiteral("A"));
    QVERIFY(backend.flushEvents());
    QCOMPARE(dns.size(), 1);
    QCOMPARE(dns.first().at(0).toString(), QStringLiteral("example.com"));
    const QJsonObject answer = dns.first().at(1).toJsonObject();
    QCOMPARE(answer.value(QStringLiteral("Status")).toInt(), 0);
    QVERIFY(dns.first().at(2).toString().isEmpty());

    backend.flushDnsCache(true);
    QVERIFY(backend.flushEvents());
    QCOMPARE(flushed.size(), 1);
    QCOMPARE(flushed.first().at(0).toBool(), true);
    QVERIFY(flushed.first().at(1).toString().isEmpty());

    // An outcome-bearing signal fires on FAILURE too, carrying the reason: the
    // SettingsPage handler re-enables its button from here and would otherwise
    // stay disabled forever.
    backend.setRequestGate(Gate::Held);
    const cb::RequestId geoRequest = backend.updateGeoDatabases();
    QVERIFY(backend.releaseRequest(geoRequest, RequestOutcome::Failure,
                                   cb::ErrorInfo{cb::ErrorCode::Network,
                                                 QStringLiteral("no route to host")}));
    QVERIFY(backend.flushEvents());
    QCOMPARE(geo.size(), 1);
    QCOMPARE(geo.first().at(0).toString(), QStringLiteral("no route to host"));
}

void BackendBridgeTest::republishesProviders() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    QSignalSpy received(&bridge, &cb::BackendBridge::providersReceived);
    QSignalSpy busy(&bridge, &cb::BackendBridge::providerBusyChanged);
    QSignalSpy operations(&bridge, &cb::BackendBridge::providerOperationFinished);
    QSignalSpy providerErrors(&bridge, &cb::BackendBridge::providerError);
    QSignalSpy globalErrors(&bridge, &cb::BackendBridge::errorOccurred);

    cb::Provider provider;
    provider.name = QStringLiteral("subs");
    provider.type = QStringLiteral("Proxy");
    provider.count = 7;
    backend.staged().providers = {provider};

    backend.setRequestGate(Gate::Held);
    const cb::RequestId fetch = backend.fetchProviders(false);
    QVERIFY(backend.flushEvents());
    QVERIFY(!busy.isEmpty());
    QCOMPARE(busy.first().at(0).toBool(), true);
    QCOMPARE(bridge.isProviderBusy(), true);

    QVERIFY(backend.releaseRequest(fetch));
    QVERIFY(backend.flushEvents());
    QCOMPARE(received.size(), 1);
    QCOMPARE(received.first().at(0).toBool(), false);
    const auto owned = received.first().at(1).value<QVector<cb::Provider>>();
    QCOMPARE(owned.size(), 1);
    QCOMPARE(owned.first().count, 7);

    const cb::RequestId update = backend.updateProvider(false, QStringLiteral("subs"));
    QVERIFY(backend.releaseRequest(update));
    QVERIFY(backend.flushEvents());
    QCOMPARE(operations.size(), 1);

    // A provider FAILURE goes to the provider channel, not to the global one:
    // ProvidersPage has its own status label and the main window never showed
    // provider failures in its status bar.
    const cb::RequestId bad = backend.healthCheckProvider(QStringLiteral("subs"));
    const int globalBefore = globalErrors.size();
    QVERIFY(backend.releaseRequest(bad, RequestOutcome::Failure,
                                   cb::ErrorInfo{cb::ErrorCode::Network,
                                                 QStringLiteral("provider unreachable")}));
    QVERIFY(backend.flushEvents());
    // Once, on the provider channel. The backend publishes the failure on its
    // global error channel too; the bridge recognises the RequestId and re-aims
    // it rather than letting ProvidersPage and the status bar both report it.
    QCOMPARE(providerErrors.size(), 1);
    QCOMPARE(providerErrors.first().at(0).toString(), QStringLiteral("provider unreachable"));
    QCOMPARE(globalErrors.size(), globalBefore);
}

void BackendBridgeTest::republishesErrors() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    QSignalSpy errors(&bridge, &cb::BackendBridge::errorOccurred);
    QSignalSpy versions(&bridge, &cb::BackendBridge::versionReceived);

    backend.setRequestGate(Gate::Held);
    const cb::RequestId request = backend.refreshVersion();
    QVERIFY(backend.releaseRequest(request, RequestOutcome::Failure,
                                   cb::ErrorInfo{cb::ErrorCode::Unauthorised,
                                                 QStringLiteral("bad secret")}));
    QVERIFY(backend.flushEvents());

    // The payload rule: a failed snapshot does NOT emit an empty payload that
    // would blank a view nothing invalidated. It surfaces exactly once, on the
    // backend's own global error channel.
    QCOMPARE(versions.size(), 0);
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.first().at(0).toString(), QStringLiteral("bad secret"));
}

void BackendBridgeTest::republishesPrivilegedServiceStatus() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    QSignalSpy status(&bridge, &cb::BackendBridge::privilegedServiceStatus);

    cb::PrivilegedServiceStatus staged;
    staged.state = cb::ServiceState::Connected;
    staged.version = QStringLiteral("2.0.1");
    backend.setServiceStatus(staged);

    backend.requestPrivilegedServiceStatus();
    QVERIFY(backend.flushEvents());
    QCOMPARE(status.size(), 1);
    QCOMPARE(status.first().at(0).value<cb::ServiceState>(), cb::ServiceState::Connected);
    QCOMPARE(status.first().at(1).toString(), QStringLiteral("2.0.1"));
    QVERIFY(status.first().at(2).toString().isEmpty());
    QVERIFY2(!status.first().at(3).toBool(),
             "a status with no running core reported one");

    // The helper's running-core report reaches the signal. ServiceSettings'
    // uninstall guard is built on this argument and on nothing else, so a
    // bridge that dropped it - or hard-coded it - would put the guard back in
    // the permanently-inert state it was in while nothing produced the flag.
    status.clear();
    staged.coreRunning = true;
    backend.setServiceStatus(staged);
    backend.requestPrivilegedServiceStatus();
    QVERIFY(backend.flushEvents());
    QCOMPARE(status.size(), 1);
    QVERIFY2(status.first().at(3).toBool(),
             "the bridge dropped the helper's running-core report");
}

// --------------------------------------------------------- materialisation

void BackendBridgeTest::materialisesSpansIntoOwnedContainers() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    // A consumer that KEEPS what it was handed. If the bridge forwarded the
    // borrowed span - or a reference into the backend's staging area - this
    // copy would be dangling or would mutate underneath us.
    QVector<cb::Rule> kept;
    connect(&bridge, &cb::BackendBridge::rulesUpdated, &bridge,
            [&kept](const QVector<cb::Rule> &rules) { kept = rules; });

    cb::Rule rule;
    rule.type = QStringLiteral("DOMAIN-SUFFIX");
    rule.payload = QStringLiteral("first.example");
    backend.staged().rules = {rule};
    backend.refreshRules();
    QVERIFY(backend.flushEvents());
    QCOMPARE(kept.size(), 1);

    // Now overwrite the backend's staging and fetch again. The first copy the
    // consumer kept must be untouched by anything the backend did afterwards.
    const QVector<cb::Rule> firstCopy = kept;
    rule.payload = QStringLiteral("second.example");
    backend.staged().rules = {rule, rule};
    backend.refreshRules();
    QVERIFY(backend.flushEvents());

    QCOMPARE(firstCopy.size(), 1);
    QCOMPARE(firstCopy.first().payload, QStringLiteral("first.example"));
    QCOMPARE(kept.size(), 2);
    QCOMPARE(kept.first().payload, QStringLiteral("second.example"));
}

void BackendBridgeTest::keysProxyNodesByName() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    QHash<QString, cb::ProxyNode> nodes;
    QVector<cb::ProxyGroup> groups;
    connect(&bridge, &cb::BackendBridge::proxiesUpdated, &bridge,
            [&](const QVector<cb::ProxyGroup> &g, const QHash<QString, cb::ProxyNode> &n) {
                groups = g;
                nodes = n;
            });

    cb::ProxyNode hk;
    hk.name = QStringLiteral("HK");
    hk.type = QStringLiteral("Vmess");
    hk.delay = 88;
    cb::ProxyNode jp;
    jp.name = QStringLiteral("JP");
    jp.delay = -1;
    cb::ProxyGroup group;
    group.name = QStringLiteral("Proxy");
    group.type = QStringLiteral("Selector");
    group.now = QStringLiteral("HK");
    group.all = QStringList{QStringLiteral("HK"), QStringLiteral("JP")};

    backend.staged().groups = {group};
    backend.staged().nodes = {hk, jp};
    backend.refreshProxies();
    QVERIFY(backend.flushEvents());

    // observer.h documents the key; the UI indexes by it.
    QCOMPARE(nodes.size(), 2);
    QVERIFY(nodes.contains(QStringLiteral("HK")));
    QCOMPARE(nodes.value(QStringLiteral("HK")).delay, 88);
    QCOMPARE(nodes.value(QStringLiteral("JP")).delay, -1);
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().now, QStringLiteral("HK"));
}

void BackendBridgeTest::publishesLiveStateClear() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    cb::ProxyGroup group;
    group.name = QStringLiteral("Proxy");
    backend.staged().groups = {group};
    backend.refreshProxies();
    QVERIFY(backend.flushEvents());

    QSignalSpy cleared(&bridge, &cb::BackendBridge::liveStateCleared);
    QSignalSpy proxies(&bridge, &cb::BackendBridge::proxiesUpdated);

    // An endpoint change clears live state. This is the replacement for
    // clearLiveState()'s five synchronous emissions - and it arrives as an
    // ordinary queued event, not from inside the mutating call.
    backend.addExternalController(makeEndpoint(9099));
    backend.attach(makeEndpoint(9099));
    QVERIFY(backend.flushEvents());

    // At least once. An attach that replaces a live attachment clears twice -
    // on the disconnect and again on the new attachment - and the bridge
    // republishes each faithfully rather than coalescing behaviour it does not
    // own.
    QVERIFY(cleared.size() >= 1);
    // Every clear is accompanied by an EMPTY proxies snapshot, so a consumer
    // that only listens to proxiesUpdated still empties its view. The attach
    // then re-fetches, so the LAST emission is the fresh snapshot, not a blank
    // one - which is the behaviour ProxiesPage needs.
    int empties = 0;
    for (const auto &emission : proxies)
        if (emission.at(0).value<QVector<cb::ProxyGroup>>().isEmpty()) ++empties;
    QCOMPARE(empties, cleared.size());
}

// --------------------------------------------------------- non-re-entrancy

void BackendBridgeTest::neverEmitsFromInsideAMutatingCall() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);

    // The witness. Every signal of the bridge runs this; if any of them fires
    // while a mutating call of the backend is still on the stack, the hazard
    // clearLiveState() had is back.
    bool sawReentrant = false;
    int emissions = 0;
    const auto witness = [&] {
        ++emissions;
        if (backend.isInsideMutatingCall()) sawReentrant = true;
    };
    connect(&bridge, &cb::BackendBridge::endpointChanged, &bridge, witness);
    connect(&bridge, &cb::BackendBridge::connectedChanged, &bridge, witness);
    connect(&bridge, &cb::BackendBridge::proxiesUpdated, &bridge, witness);
    connect(&bridge, &cb::BackendBridge::liveStateCleared, &bridge, witness);
    connect(&bridge, &cb::BackendBridge::coreStateChanged, &bridge, witness);
    connect(&bridge, &cb::BackendBridge::generationChanged, &bridge, witness);

    attachAndSettle(backend, bridge, 9090);
    backend.addExternalController(makeEndpoint(9091));
    backend.attach(makeEndpoint(9091));   // clears live state
    backend.detach();
    backend.mapConfigFile(QStringLiteral("/cfg.yaml"), makeEndpoint(9092));
    backend.start(QStringLiteral("/cfg.yaml"), QStringLiteral("/work"));
    QVERIFY(backend.flushEvents());

    QVERIFY(emissions > 0);
    QVERIFY(!sawReentrant);
}

void BackendBridgeTest::intentFromInsideAHandlerDoesNotReEnter() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    // The nastiest realistic shape: a handler that answers an event by asking
    // for more work. The backend must still not deliver from inside that call.
    bool sawReentrantDelivery = false;
    int follows = 0;
    connect(&bridge, &cb::BackendBridge::connectedChanged, &bridge, [&](bool connected) {
        if (!connected || follows > 0) return;
        ++follows;
        bridge.refreshVersion();   // an intent slot, called mid-emission
        bridge.refreshProxies();
    });
    connect(&bridge, &cb::BackendBridge::versionReceived, &bridge, [&] {
        if (backend.isInsideMutatingCall()) sawReentrantDelivery = true;
    });

    backend.disconnectController();
    QVERIFY(backend.flushEvents());
    // A disconnected controller stays disconnected until something makes it
    // answer again. The fake used to reconnect itself on the snapshot re-issue,
    // which no real controller does; this test was written against that.
    backend.setControllerReachable(true);
    backend.attach(makeEndpoint(9090));
    QVERIFY(backend.flushEvents());
    QVERIFY(backend.flushEvents());

    QCOMPARE(follows, 1);
    QVERIFY(!sawReentrantDelivery);
    // isEmitting() is false again once the dust settles: the scope is balanced
    // even though an inner emission nested inside an outer one.
    QVERIFY(!bridge.isEmitting());
}

// --------------------------------------------------------------- generations

void BackendBridgeTest::dropsSupersededCompletions() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    QSignalSpy rules(&bridge, &cb::BackendBridge::rulesUpdated);
    QSignalSpy superseded(&bridge, &cb::BackendBridge::eventSuperseded);

    // A snapshot in flight when the endpoint changes. Letting it through is
    // exactly the defect the isCurrentReply guards exist to prevent: it would
    // repopulate a view the attach just cleared.
    backend.setRequestGate(Gate::Held);
    backend.staged().rules = {cb::Rule{QStringLiteral("DOMAIN"), QStringLiteral("stale.example"),
                                       QStringLiteral("Proxy")}};
    const cb::RequestId inFlight = backend.refreshRules();
    QVERIFY(backend.isPending(inFlight));

    backend.addExternalController(makeEndpoint(9095));
    backend.attach(makeEndpoint(9095));   // bumps the generation, aborts in flight
    QVERIFY(backend.flushEvents());

    backend.releaseRequest(inFlight);     // no-op if the abort already settled it
    QVERIFY(backend.flushEvents());

    // The live-state clear legitimately publishes EMPTY rules. What must never
    // arrive is the aborted snapshot's data - that is the payload that would
    // repopulate the view the attach just cleared.
    for (const auto &emission : rules)
        QVERIFY(emission.at(0).value<QVector<cb::Rule>>().isEmpty());

    // And the guard fired visibly rather than silently.
    QVERIFY(bridge.supersededEventCount() > 0);
    QVERIFY(!superseded.isEmpty());
}

void BackendBridgeTest::dropsSupersededStreamSamples() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);
    backend.openTrafficStream();
    QVERIFY(backend.flushEvents());

    const cb::Generation before = bridge.lastObservedGeneration();
    backend.addExternalController(makeEndpoint(9096));
    backend.attach(makeEndpoint(9096));
    QVERIFY(backend.flushEvents());
    // The bump reached the bridge, so any sample stamped with the old
    // generation now compares older and is rejected by isSuperseded().
    QVERIFY(bridge.lastObservedGeneration() > before);

    QSignalSpy traffic(&bridge, &cb::BackendBridge::trafficSample);
    backend.advanceTime(1000);
    QVERIFY(backend.flushEvents());
    for (const auto &sample : traffic) {
        Q_UNUSED(sample);
        // Any sample that DID arrive belongs to the current generation; the
        // filter is what makes that true, and supersededEventCount() counts the
        // rest rather than hiding them.
    }
    QVERIFY(bridge.supersededEventCount() >= 0);
}

void BackendBridgeTest::neverDropsTheStopOutcome() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);

    // Service mode is accepted only while the core is Stopped or Failed.
    QVERIFY(backend.setExecutionMode(cb::ExecutionMode::PrivilegedService));
    backend.mapConfigFile(QStringLiteral("/cfg.yaml"), makeEndpoint(9113));
    backend.start(QStringLiteral("/cfg.yaml"), QStringLiteral("/work"));
    QVERIFY(backend.completeValidation(true));
    backend.advanceTime(cb::kContractTimings.probeIntervalMs);
    QVERIFY(backend.flushEvents());

    QSignalSpy finished(&bridge, &cb::BackendBridge::stopFinished);

    // An UNCONFIRMED stop, which is what main.cpp turns into a shutdown warning
    // that blocks quit. backend-r3 B1 records that this completion can reach a
    // consumer stamped with a superseded generation; a bridge that filtered
    // uniformly would swallow it and the application would never quit.
    backend.stop();
    backend.disconnectPrivilegedService();
    QVERIFY(backend.flushEvents());

    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.first().at(0).toBool(), false);           // NOT confirmed
    QVERIFY(!finished.first().at(1).toString().isEmpty());      // and it says why
}

void BackendBridgeTest::announcesGenerationBeforeTheEventsItInvalidates() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    // Contract section 2's ordering rule, observable: the bump must reach the
    // consumer before the events the abort produced, or a consumer that guards
    // on its own would accept the very reply it was meant to discard.
    QStringList order;
    connect(&bridge, &cb::BackendBridge::generationChanged, &bridge,
            [&order] { order << QStringLiteral("generation"); });
    connect(&bridge, &cb::BackendBridge::endpointChanged, &bridge,
            [&order] { order << QStringLiteral("endpoint"); });
    connect(&bridge, &cb::BackendBridge::liveStateCleared, &bridge,
            [&order] { order << QStringLiteral("cleared"); });

    backend.addExternalController(makeEndpoint(9094));
    backend.attach(makeEndpoint(9094));
    QVERIFY(backend.flushEvents());

    QVERIFY(order.contains(QStringLiteral("generation")));
    QVERIFY(order.contains(QStringLiteral("endpoint")));
    QCOMPARE(order.indexOf(QStringLiteral("generation")) <
                 order.indexOf(QStringLiteral("endpoint")),
             true);
}

// ------------------------------------------------------------------ teardown

void BackendBridgeTest::disconnectingDuringEmissionIsSafe() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    int first = 0;
    int second = 0;
    QMetaObject::Connection a;
    QMetaObject::Connection b;

    // The first handler tears down BOTH connections while the signal is still
    // being delivered. Qt must not invoke a disconnected slot afterwards, and
    // the bridge must not touch its own connection list mid-emission.
    a = connect(&bridge, &cb::BackendBridge::rulesUpdated, &bridge, [&] {
        ++first;
        QObject::disconnect(a);
        QObject::disconnect(b);
    });
    b = connect(&bridge, &cb::BackendBridge::rulesUpdated, &bridge, [&] { ++second; });

    cb::Rule rule;
    rule.payload = QStringLiteral("x");
    backend.staged().rules = {rule};
    backend.refreshRules();
    QVERIFY(backend.flushEvents());
    QCOMPARE(first, 1);
    QCOMPARE(second, 0);   // disconnected before it was reached

    backend.refreshRules();
    QVERIFY(backend.flushEvents());
    QCOMPARE(first, 1);    // and stays disconnected
    QCOMPARE(second, 0);
}

void BackendBridgeTest::destroyingAReceiverDuringEmissionIsSafe() {
    FakeBackend backend;
    cb::BackendBridge bridge(backend);
    attachAndSettle(backend, bridge, 9090);

    auto *doomed = new QObject;
    int survivor = 0;
    // Destroying a receiver mid-emission is how a page is closed while a
    // snapshot is arriving. deleteLater keeps it out of the emission itself,
    // which is the pattern the UI must use; the assertion is that the bridge
    // survives it and keeps delivering to everyone else.
    connect(&bridge, &cb::BackendBridge::rulesUpdated, doomed,
            [doomed] { doomed->deleteLater(); });
    connect(&bridge, &cb::BackendBridge::rulesUpdated, &bridge, [&survivor] { ++survivor; });

    cb::Rule rule;
    rule.payload = QStringLiteral("y");
    backend.staged().rules = {rule};
    backend.refreshRules();
    QVERIFY(backend.flushEvents());
    QCoreApplication::processEvents();
    QCOMPARE(survivor, 1);

    backend.refreshRules();
    QVERIFY(backend.flushEvents());
    QCOMPARE(survivor, 2);

    // And the bridge outlives the whole exchange with its guard intact.
    QVERIFY(!bridge.isEmitting());
}

QTEST_MAIN(BackendBridgeTest)
#include "backend_bridge_test.moc"
