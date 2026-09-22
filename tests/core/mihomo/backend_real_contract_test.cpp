// The MihomoBackend contract, run against the REAL backend.
// Specification: docs/module-api.md, section 10.
//
// WHY THIS FILE EXISTS ALONGSIDE tests/contracts/backend/backend_contract_test.cpp
//
// The published contract suite is written against testsupport::backend::FakeBackend
// by name, not against a core::backend::MihomoBackend &. Every case drives knobs
// that exist only on a deterministic in-process double: a virtual clock
// (advanceTime/elapsedMs, used out to 4 x the 180 s hard cap), staged payloads
// (staged()), per-request gates (setRequestGate/releaseRequest/pendingRequests/
// submittedGeneration), and setAbortOrdering(AbortThenBump) - a deliberate
// inversion of the contract's own ordering rule, which a production backend must
// not be able to exhibit. None of those can be implemented over a real child
// process and a real socket without either editing the frozen suite or building
// the hazard into the shipping code. So the frozen suite is NOT run verbatim
// here; this file asserts the same eleven behaviours section 10 names, against
// the real BACKEND, with real child processes and a real loopback controller.
//
// It does NOT drive the real mihomo: the controller is a loopback fixture and
// the child is clash-qt-fake-core. That is deliberate - the hazards below need
// gates the engine has no way to offer - but it is also why backend-r3 B4
// refuses to let section 10's "additionally passes against the locally built
// mihomo" rest on this file's name. That claim is carried by
// backend_real_core_test.cpp, which names the bullets the engine cannot drive.
//
// The one case with no honest real-engine equivalent is the negative arm of
// "generation is bumped before in-flight work is aborted": proving the hazard
// requires producing it. What IS asserted here is the positive arm and its
// observable consequence - the completion an abort produces is marked
// Superseded and carries the generation it was submitted under, which is
// strictly stronger than the generation comparison alone (backend-r2 A2).
//
// Nothing here sleeps on a fixed duration. Gates are released explicitly, the
// readiness budget is made small through the published CoreTimings seam rather
// than waited out, and every deadline only bounds a failure.
//
// THE SHARED SUITE, AND WHY IT IS HOSTED HERE
//
// Everything above is a SPECIALIST case: it names core::MihomoBackendImpl and
// uses seams a module consumer does not get. Below, after them, the
// implementation-neutral cases of tests/contracts/backend/common/ run against
// FOUR implementations from this one executable - the in-process fake, this
// real backend, the shipping module through the production loader, and the
// test module - one QTest data row each. They reach their subject only through
// core::backend::MihomoBackend &, and the assertions live once, in
// backend_common_cases.cpp, rather than four times.
//
// They are hosted in THIS target on purpose. clash_mihomo_impl is
// component-private: a new executable linking it would be a sixth permanent
// exception to that rule, and the permanent allowance is for the targets that
// already exist. Nothing of the shared suite is specific to this
// file, and nothing here is weakened by them: the specialist cases above keep
// every claim they already made.

#include <QtTest>

#include <functional>
#include <memory>
#include <type_traits>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "contracts/backend/common/backend_common_cases.h"
#include "core/mihomo/mihomo_backend.h"
#include "support/backend/common_contract_driver.h"
#include "support/fake_core.h"
#include "support/loopback_server.h"
#include "support/scoped_environment.h"

namespace cb = core::backend;
namespace shared = testsupport::backend::common;
namespace scenarios = testsupport::backend::common::cases;

namespace {

// Records what reached an observer, and - the point of the exercise - whether
// anything reached it from inside a mutating call.
class Recorder final : public cb::BackendObserver {
  public:
    explicit Recorder(core::MihomoBackendImpl *backend, QString name = QStringLiteral("observer"))
        : backend_(backend), name_(std::move(name)) {}

    struct CompletionEvent {
        QString channel;
        cb::Completion completion;
        QString payload;
    };

    std::vector<CompletionEvent> events;
    std::vector<cb::Completion> failures;
    std::vector<cb::Completion> coreFailures;
    std::vector<cb::Endpoint> readyEndpoints;
    std::vector<cb::StopCompleted> stops;
    std::vector<cb::TunChangeCompleted> tunChanges;
    // Every connection transition, in order: which one a stale reply produced
    // is the whole question in theReplacementAtTheSameAddress... below, and a
    // count of "is it connected NOW" cannot answer it.
    std::vector<bool> connections;
    struct StateEvent { cb::Generation generation; cb::CoreState state; cb::Ownership ownership; };
    std::vector<StateEvent> states;
    /// Every traffic sample, with the generation it was stamped with. A stream
    /// frame completes no request, so it arrives on no completion channel and
    /// the only thing that can say which SESSION produced it is the number in
    /// the frame itself.
    struct Sample { cb::Generation generation; quint64 up; quint64 down; };
    std::vector<Sample> samples;
    bool sawSample(quint64 up) const {
        for (const Sample &sample : samples)
            if (sample.up == up) return true;
        return false;
    }
    int callbackCount = 0;
    int stoppedCount = 0;
    int liveStateClears = 0;
    bool sawReentrantDelivery = false;
    std::function<void()> onNextEvent;

    cb::Generation lastObserved() const { return lastObserved_; }

    int countOf(const QString &channel, cb::CompletionStatus status) const {
        int total = 0;
        for (const auto &event : events)
            if (event.channel == channel && event.completion.status == status) ++total;
        return total;
    }
    const CompletionEvent *find(const QString &channel, cb::RequestId request) const {
        for (const auto &event : events)
            if (event.channel == channel && event.completion.request == request) return &event;
        return nullptr;
    }

    // ---- BackendObserver
    void coreStateChanged(cb::Generation generation, cb::CoreState state,
                          cb::Ownership ownership) noexcept override {
        note(generation);
        states.push_back({generation, state, ownership});
    }
    void coreReady(const cb::Completion &completion, const cb::Endpoint &endpoint) noexcept override {
        note(completion.generation);
        readyEndpoints.push_back(endpoint);
    }
    void coreLogLine(cb::Generation generation, const QString &) noexcept override {
        note(generation);
    }
    void coreFailed(const cb::Completion &completion) noexcept override {
        note(completion.generation);
        coreFailures.push_back(completion);
    }
    void coreStopped(cb::Generation generation) noexcept override {
        note(generation);
        ++stoppedCount;
    }
    void stopCompleted(const cb::StopCompleted &result) noexcept override {
        note(result.generation);
        stops.push_back(result);
    }
    void endpointChanged(cb::Generation generation, const cb::Endpoint &,
                         cb::Ownership) noexcept override {
        note(generation);
    }
    void connectedChanged(cb::Generation generation, bool connected) noexcept override {
        note(generation);
        connections.push_back(connected);
    }
    void configReceived(const cb::Completion &completion, const cb::BaseConfig &config) noexcept override {
        record(QStringLiteral("config"), completion, config.mode);
    }
    void modeChanged(const cb::Completion &completion, const QString &mode) noexcept override {
        record(QStringLiteral("mode"), completion, mode);
    }
    void tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept override {
        note(result.generation);
        tunChanges.push_back(result);
    }
    void nodeSelected(const cb::Completion &completion, const QString &group,
                      const QString &node) noexcept override {
        record(QStringLiteral("node"), completion, group + QLatin1Char('/') + node);
    }
    void versionReceived(const cb::Completion &completion, const QString &version) noexcept override {
        record(QStringLiteral("version"), completion, version);
    }
    void proxiesUpdated(const cb::Completion &completion, cb::Span<cb::ProxyGroup> groups,
                        cb::Span<cb::ProxyNode> nodes) noexcept override {
        if (completion.request == cb::RequestId::Invalid && groups.isEmpty() && nodes.isEmpty())
            ++liveStateClears;
        record(QStringLiteral("proxies"), completion, QString::number(groups.size()));
    }
    void rulesUpdated(const cb::Completion &completion, cb::Span<cb::Rule> rules) noexcept override {
        record(QStringLiteral("rules"), completion, QString::number(rules.size()));
    }
    void providersReceived(const cb::Completion &completion, bool rules,
                           cb::Span<cb::Provider> providers) noexcept override {
        record(rules ? QStringLiteral("providers/rules") : QStringLiteral("providers/proxies"),
               completion, QString::number(providers.size()));
    }
    void providerBusyChanged(cb::Generation generation, bool) noexcept override { note(generation); }
    void providerOperationFinished(const cb::Completion &completion,
                                   const QString &message) noexcept override {
        record(QStringLiteral("provider-op"), completion, message);
    }
    void privilegedServiceStatus(const cb::Completion &completion,
                                 const cb::PrivilegedServiceStatus &status) noexcept override {
        record(QStringLiteral("service-status"), completion, status.version);
    }
    void errorOccurred(const cb::Completion &completion) noexcept override {
        note(completion.generation);
        failures.push_back(completion);
    }
    void trafficSample(cb::Generation generation, quint64 up, quint64 down) noexcept override {
        note(generation);
        samples.push_back({generation, up, down});
    }

  private:
    void note(cb::Generation generation) {
        ++callbackCount;
        // The contract obligation, enforced here rather than assumed.
        if (backend_ && backend_->isInsideMutatingCall()) sawReentrantDelivery = true;
        if (generation > lastObserved_) lastObserved_ = generation;
        if (onNextEvent) {
            const auto once = onNextEvent;
            onNextEvent = nullptr;
            once();
        }
    }
    void record(const QString &channel, const cb::Completion &completion, const QString &payload) {
        note(completion.generation);
        events.push_back({channel, completion, payload});
    }

    core::MihomoBackendImpl *backend_ = nullptr;
    QString name_;
    cb::Generation lastObserved_ = cb::Generation::Initial;
};

// A privileged core service with no socket at all. The injected seam is what
// makes this possible: before it, this case needed a QLocalServer speaking the
// helper's framing, because platform::PrivilegedServiceClient was baked into
// the constructor.
class StubPrivilegedService final : public core::PrivilegedCoreService {
  public:
    explicit StubPrivilegedService(cb::Endpoint endpoint) : endpoint_(std::move(endpoint)) {}

    void setListener(core::PrivilegedCoreServiceListener *listener) override {
        listener_ = listener;
    }
    bool isSupported() const override { return true; }
    bool isAvailable() const override { return true; }
    bool isConnected() const override { return connected_; }
    bool isBusy() const override { return false; }
    QString connectionError() const override { return connectionError_; }
    void requestStatus() override {
        if (!listener_) return;
        QJsonObject status;
        status.insert(QStringLiteral("state"), started_ ? QStringLiteral("running")
                                                        : QStringLiteral("stopped"));
        status.insert(QStringLiteral("version"), QStringLiteral("helper-1.2.3"));
        statusAnswers_.append(status);
    }
    void requestLogs() override {}
    void startCore(const QJsonObject &config) override {
        startedConfig_ = config;
        started_ = true;
        connected_ = true;
        if (!listener_) return;
        QJsonObject endpoint;
        endpoint.insert(QStringLiteral("host"), endpoint_.host);
        endpoint.insert(QStringLiteral("port"), endpoint_.port);
        endpoint.insert(QStringLiteral("secret"), endpoint_.secret);
        listener_->privilegedCoreStarted(endpoint);
    }
    void stopCore() override { stopRequested_ = true; }
    void close() override { connected_ = false; }

    // --- driving
    bool stopRequested() const { return stopRequested_; }
    bool started() const { return started_; }
    void confirmStopped() {
        started_ = false;
        if (listener_) listener_->privilegedCoreStopped();
    }
    /// The service drops the connection before confirming the child exited.
    void disconnectWithoutConfirming(const QString &reason) {
        connectionError_ = reason;
        connected_ = false;
        if (listener_) listener_->privilegedConnectedChanged(false);
    }
    void deliverStatus() {
        if (!listener_) return;
        for (const QJsonObject &status : std::as_const(statusAnswers_))
            listener_->privilegedStatusReceived(status);
        statusAnswers_.clear();
    }
    /// Hands the listener a helper status payload verbatim, in place of the
    /// queued one. What is under test is what the backend makes of the helper's
    /// OWN fields, so the case has to be able to spell them.
    void deliverStatusPayload(const QJsonObject &status) {
        if (listener_) listener_->privilegedStatusReceived(status);
        statusAnswers_.clear();
    }

  private:
    core::PrivilegedCoreServiceListener *listener_ = nullptr;
    cb::Endpoint endpoint_;
    QJsonObject startedConfig_;
    QList<QJsonObject> statusAnswers_;
    QString connectionError_;
    bool started_ = false;
    bool connected_ = false;
    bool stopRequested_ = false;
};

void scriptController(testsupport::LoopbackServer &server,
                      const QByteArray &version = R"({"version":"1.19.31"})") {
    using Reply = testsupport::LoopbackServer::Reply;
    server.route("GET", "/version", Reply::json(version));
    server.route("GET", "/proxies", Reply::json(R"({"proxies":{"GLOBAL":{"type":"Selector","now":"DIRECT","all":["DIRECT"]}}})"));
    server.route("GET", "/rules", Reply::json(R"({"rules":[{"type":"MATCH","payload":"","proxy":"DIRECT"}]})"));
    server.route("GET", "/configs", Reply::json(R"({"mode":"rule","tun":{"enable":false}})"));
}

cb::Endpoint endpointOf(const testsupport::LoopbackServer &server) {
    cb::Endpoint endpoint;
    endpoint.host = server.host();
    endpoint.port = server.port();
    endpoint.secret = server.secret();
    return endpoint;
}

}  // namespace

class BackendRealContractTest : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("mbk"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
        // The real preference store must not move under a test run.
        QVERIFY2(environment_->realPreferencesUnchanged(),
                 "the developer's own preferences changed before this test even ran");
    }
    void cleanup() {
        QVERIFY2(environment_->realPreferencesUnchanged(),
                 "this test wrote into the developer's own preference store");
        environment_.reset();
    }

    // ---------------------------------------------- ABI readiness (section 9)

    void publishedShapesAreAbiReady() {
        static_assert(std::is_same_v<std::underlying_type_t<cb::CoreState>, std::uint8_t>);
        static_assert(std::is_same_v<std::underlying_type_t<cb::Ownership>, std::uint8_t>);
        static_assert(std::is_same_v<std::underlying_type_t<cb::ExecutionMode>, std::uint8_t>);
        static_assert(std::is_same_v<std::underlying_type_t<cb::CompletionStatus>, std::uint8_t>);
        static_assert(std::is_same_v<std::underlying_type_t<cb::ErrorCode>, std::int32_t>);
        static_assert(std::is_same_v<std::underlying_type_t<cb::Feature>, std::uint32_t>);
        static_assert(std::is_same_v<std::underlying_type_t<cb::RequestId>, std::uint64_t>);
        static_assert(std::is_same_v<std::underlying_type_t<cb::Generation>, std::uint64_t>);
        static_assert(std::is_standard_layout_v<cb::Endpoint>);
        // The REAL backend is what has to satisfy these, not only the fake.
        static_assert(std::is_same_v<decltype(std::declval<core::MihomoBackendImpl &>().managedEndpoint()),
                                     cb::Endpoint>);
        static_assert(std::is_same_v<decltype(std::declval<core::MihomoBackendImpl &>().currentEndpoint()),
                                     cb::Endpoint>);
        static_assert(std::is_base_of_v<cb::MihomoBackend, core::MihomoBackendImpl>);

        core::MihomoBackendImpl backend;
        // Published as data because a consumer may not assume a fixed timeout,
        // and reported from the backend's REAL budget, not from a constant.
        QCOMPARE(backend.timings().idleDeadlineMs, 10000u);
        QCOMPARE(backend.timings().serviceIdleDeadlineMs, 60000u);
        QCOMPARE(backend.timings().hardCapMs, 180000u);
        QCOMPARE(backend.timings().probeIntervalMs, 200u);
        QCOMPARE(backend.timings().probeTimeoutMs, 2000u);
        QCOMPARE(backend.timings().terminateWaitMs, 3000u);
        QCOMPARE(backend.identity().interfaceRevision, 1u);
        QVERIFY(backend.features().has(cb::Feature::ManagedLifecycle));
        QVERIFY(backend.features().has(cb::Feature::ConfirmedTunChange));
        QVERIFY(backend.features().has(cb::Feature::ConfigValidation));
    }

    // --- 1. detach leaves an attached controller running

    void detachLeavesTheAttachedControllerRunning() {
        testsupport::LoopbackServer external;
        QVERIFY(external.listen());
        scriptController(external);

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        QVERIFY(backend.addObserver(&observer));

        backend.attach(endpointOf(external));
        QVERIFY(external.waitFor([&] { return backend.isConnected(); }));
        QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Attached);
        QVERIFY(backend.isExternalControllerConnected());

        backend.detach();
        QVERIFY(backend.drainPendingEvents());
        QVERIFY(!backend.isAttached());
        QCOMPARE(backend.attachmentOwnership(), cb::Ownership::None);

        // The whole point: detaching is not a kill switch. The controller is
        // still there, and re-attaching proves it rather than assuming it.
        backend.attach(endpointOf(external));
        QVERIFY(external.waitFor([&] { return backend.isConnected(); }));
        QVERIFY(!external.sawUnexpectedRequest());
        QVERIFY(backend.removeObserver(&observer));
    }

    // --- 2 & 3. superseded completions are MARKED, and the generation is
    //            bumped before the abort that produces them

    void anAbortedCompletionIsMarkedSupersededAndCarriesItsSubmitGeneration() {
        testsupport::LoopbackServer first;
        testsupport::LoopbackServer second;
        QVERIFY(first.listen());
        QVERIFY(second.listen());
        scriptController(first, R"({"version":"from-the-old-endpoint"})");
        scriptController(second);
        auto *held = first.hold("GET", "/version");

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);

        backend.attach(endpointOf(first));
        const cb::RequestId version = backend.refreshVersion();
        QVERIFY(version != cb::RequestId::Invalid);
        const cb::Generation submitted = backend.generation();
        QVERIFY(held->waitForPending(1));

        // The endpoint changes. MihomoClient bumps its epoch, this backend bumps
        // the generation on the same signal, and only then is the in-flight work
        // aborted - finished() can run synchronously inside that abort.
        backend.attach(endpointOf(second));
        QVERIFY(backend.generation() > submitted);
        QVERIFY(backend.drainPendingEvents());
        QVERIFY(first.waitFor([&] {
            return observer.find(QStringLiteral("version"), version) != nullptr;
        }));

        const auto *completion = observer.find(QStringLiteral("version"), version);
        QVERIFY(completion);
        QVERIFY2(completion->completion.status == cb::CompletionStatus::Superseded,
                 "an abandoned completion was not MARKED superseded (backend-r2 A2)");
        QCOMPARE(completion->completion.generation, submitted);
        QVERIFY2(cb::isSuperseded(completion->completion.generation, observer.lastObserved()),
                 "the completion did not compare older than the newest observed generation");
        QVERIFY2(completion->payload != QStringLiteral("from-the-old-endpoint"),
                 "a completion aborted by an endpoint change was delivered as live data");
        backend.removeObserver(&observer);
    }

    // A re-attach to the address we are ALREADY on is not a no-op: a reload
    // rebinds the replacement engine to the port the retired one held, so it
    // announces a new session on an unchanged endpoint. Everything the retired
    // process still owes has to be invalidated at that moment. Left current,
    // those replies land after the replacement has answered and clear the live
    // session - observed against the real engine, which produced ten refused
    // connections, a connected flag of false, and an explicit refreshVersion()
    // healing it with the generation unchanged.
    void aReplacementAtTheSameAddressCannotClearTheNewSession() {
        using Reply = testsupport::LoopbackServer::Reply;
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen());
        scriptController(controller, R"({"version":"first-session"})");

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);

        backend.attach(endpointOf(controller));
        QVERIFY(controller.waitFor([&] { return backend.isConnected(); }));
        QVERIFY(backend.drainPendingEvents());
        QCOMPARE(observer.connections.size(), std::size_t(1));
        QVERIFY(observer.connections.front());

        // What the retired engine still owes.
        auto *held = controller.hold("GET", "/version");
        const cb::RequestId stale = backend.refreshVersion();
        QVERIFY(stale != cb::RequestId::Invalid);
        const cb::Generation submitted = backend.generation();
        QVERIFY(held->waitForPending(1));

        // The engine is replaced at the same host and port. The endpoint is
        // byte-identical; the session behind it is not.
        controller.route("GET", "/configs",
                         Reply::json(R"({"mode":"global","tun":{"enable":false}})"));
        backend.attach(endpointOf(controller));
        QVERIFY2(backend.generation() > submitted,
                 "re-attaching to a replaced engine left the generation where it was, so "
                 "nothing the retired process owed was invalidated");

        // The replacement answers first: from here on, anything the retired
        // engine produces is arriving AFTER the new response.
        QVERIFY(controller.waitFor([&] {
            return observer.countOf(QStringLiteral("config"), cb::CompletionStatus::Ok) >= 2;
        }));
        const int clears = observer.liveStateClears;
        const std::size_t transitions = observer.connections.size();

        // Now the retired engine answers the way a process that has gone
        // answers: the socket closes with nothing on it.
        controller.route("GET", "/version", Reply::drop());
        held->releaseOne();
        QVERIFY2(controller.waitFor([&] {
                     return observer.find(QStringLiteral("version"), stale) != nullptr;
                 }),
                 qPrintable(controller.pendingReport()));

        const auto *completion = observer.find(QStringLiteral("version"), stale);
        QVERIFY(completion);
        QVERIFY2(completion->completion.status == cb::CompletionStatus::Superseded,
                 "a reply owed by the retired engine settled as live work");
        QCOMPARE(completion->completion.generation, submitted);
        QVERIFY2(observer.connections.size() == transitions,
                 "a reply owed by the retired engine reported the live session as "
                 "disconnected");
        QVERIFY2(observer.liveStateClears == clears,
                 "a reply owed by the retired engine cleared the live view the replacement "
                 "had just populated");
        QVERIFY(backend.isConnected());

        // And the replacement's own probe is unaffected by any of it.
        controller.route("GET", "/version", Reply::json(R"({"version":"second-session"})"));
        held->releaseOne();
        QVERIFY(controller.waitFor([&] {
            return observer.countOf(QStringLiteral("version"), cb::CompletionStatus::Ok) >= 2;
        }));
        QVERIFY(backend.isConnected());
        QCOMPARE(observer.connections.size(), transitions);
        QVERIFY(!controller.sawUnexpectedRequest());
        backend.removeObserver(&observer);
    }

    // --- the same boundary, for the subscription rather than for a request
    //
    // A stream is the one channel with NEITHER line of defence. A frame
    // completes no request, so it carries no submit-time stamp to compare and
    // no status to mark: it is published with whatever generation is current
    // when it arrives. So a socket held by a session that has gone cannot be
    // allowed to remain the live subscription - the only defence is that it
    // stops being one.

    void aRetiredSessionsStreamIsReplacedRatherThanInherited() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen());
        scriptController(controller, R"({"version":"first-session"})");
        controller.expectStream(QStringLiteral("/traffic"));

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.attach(endpointOf(controller));
        backend.openTrafficStream();
        QVERIFY2(controller.waitFor([&] {
                     return controller.streamHandshakes(QStringLiteral("/traffic")) >= 1 &&
                            backend.isConnected();
                 }),
                 qPrintable(controller.redactedTranscript()));
        QVERIFY(controller.sendText(QStringLiteral("/traffic"), R"({"up":11,"down":11})"));
        QVERIFY(controller.waitFor([&] { return observer.sawSample(11); }));

        const int handshakes = controller.streamHandshakes(QStringLiteral("/traffic"));
        const std::size_t transitions = observer.connections.size();
        const int clears = observer.liveStateClears;

        // The engine is replaced at the host and port it already held, and the
        // process that has gone writes one last frame on its socket.
        backend.attach(endpointOf(controller));
        QVERIFY2(controller.sendText(QStringLiteral("/traffic"), R"({"up":999,"down":999})"),
                 "the fixture had no socket left to write the retired frame on, so this case "
                 "would prove nothing");

        // The subscription is the pointer, not the socket: it survives the
        // boundary and re-dials the address we are still on.
        QVERIFY2(controller.waitFor([&] {
                     return controller.streamHandshakes(QStringLiteral("/traffic")) > handshakes;
                 }),
                 qPrintable(QStringLiteral("the replacement never re-opened the stream, so the "
                                           "retired session's socket WAS the live subscription: "
                                           "%1")
                                .arg(controller.redactedTranscript())));
        QVERIFY(controller.sendText(QStringLiteral("/traffic"), R"({"up":7,"down":7})"));
        QVERIFY2(controller.waitFor([&] { return observer.sawSample(7); }),
                 "the replacement's own stream published nothing");

        QVERIFY2(!observer.sawSample(999),
                 "a frame written by the RETIRED session was published as the replacement's "
                 "telemetry, stamped with the replacement's generation - where no consumer "
                 "rule can reject it");
        QVERIFY2(observer.connections.size() == transitions,
                 "the retired session's socket produced a connection transition for the "
                 "replacement");
        QVERIFY2(observer.liveStateClears == clears,
                 "the retired session's socket cleared the live view the replacement had just "
                 "populated");
        QVERIFY(backend.isConnected());
        backend.removeObserver(&observer);
    }

    void aRetiredStreamDropCannotDisconnectTheNewSession() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen());
        scriptController(controller, R"({"version":"first-session"})");
        controller.expectStream(QStringLiteral("/traffic"));

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.attach(endpointOf(controller));
        backend.openTrafficStream();
        QVERIFY2(controller.waitFor([&] {
                     return controller.streamHandshakes(QStringLiteral("/traffic")) >= 1 &&
                            backend.isConnected();
                 }),
                 qPrintable(controller.redactedTranscript()));

        const int handshakes = controller.streamHandshakes(QStringLiteral("/traffic"));
        const std::size_t transitions = observer.connections.size();
        const int clears = observer.liveStateClears;

        // The replacement, and then the retired session's socket dying the way
        // a process that has gone kills it: abortively, with no close frame.
        backend.attach(endpointOf(controller));
        QVERIFY2(controller.dropStream(QStringLiteral("/traffic")),
                 "the fixture had no retired socket to drop");

        QVERIFY2(controller.waitFor([&] {
                     return controller.streamHandshakes(QStringLiteral("/traffic")) > handshakes;
                 }),
                 qPrintable(controller.redactedTranscript()));
        QVERIFY(controller.sendText(QStringLiteral("/traffic"), R"({"up":5,"down":5})"));
        QVERIFY2(controller.waitFor([&] { return observer.sawSample(5); }),
                 "the stream did not resume after the retired socket died");

        QVERIFY2(observer.connections.size() == transitions,
                 "the death of the retired session's socket reported the healthy replacement "
                 "as disconnected");
        QVERIFY2(observer.liveStateClears == clears,
                 "the death of the retired session's socket cleared the replacement's live "
                 "view");
        QVERIFY(backend.isConnected());
        QVERIFY(!controller.sawUnexpectedRequest());
        backend.removeObserver(&observer);
    }

    void aCompletionSubmittedBeforeAManagedStartComparesOlder() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen());
        scriptController(controller);
        auto *held = controller.hold("GET", "/version");

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.attach(endpointOf(controller));
        const cb::RequestId version = backend.refreshVersion();
        const cb::Generation submitted = backend.generation();
        QVERIFY(held->waitForPending(1));

        // A managed start is an invalidating event of its own.
        backend.setBinaryPath(QStringLiteral("/nonexistent/mihomo"));
        backend.start(environment_->filePath(QStringLiteral("cfg.yaml")),
                      environment_->dataDir());
        QVERIFY(backend.generation() > submitted);

        held->release();
        QVERIFY(controller.waitFor([&] {
            return observer.find(QStringLiteral("version"), version) != nullptr;
        }));
        const auto *completion = observer.find(QStringLiteral("version"), version);
        QVERIFY(completion);
        QVERIFY2(cb::isSuperseded(completion->completion.generation, backend.generation()),
                 "a completion submitted before a managed start did not compare older");
        backend.removeObserver(&observer);
    }

    // --- the obligation that comes with ONE global generation

    void aNonEndpointGenerationBumpReIssuesTheSnapshotSet() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen());
        scriptController(controller);

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.attach(endpointOf(controller));
        QVERIFY(controller.waitFor([&] { return backend.isConnected(); }));
        QVERIFY(controller.waitFor([&] { return controller.requestCount("GET", "/rules") >= 1; }));

        const int rulesBefore = controller.requestCount("GET", "/rules");
        const int configsBefore = controller.requestCount("GET", "/configs");
        const cb::Generation before = backend.generation();

        // A managed stop bumps the generation and is NOT an endpoint change.
        // /rules and /configs have no recovery poll anywhere in the
        // application, so without the re-issue the rules list and BaseConfig
        // would stay stale until the next endpoint change.
        backend.stop();
        QVERIFY(backend.generation() > before);
        QVERIFY2(controller.waitFor([&] {
                     return controller.requestCount("GET", "/rules") > rulesBefore &&
                            controller.requestCount("GET", "/configs") > configsBefore;
                 }),
                 "a generation bump that was not an endpoint change did not re-issue "
                 "the snapshot set");
        backend.removeObserver(&observer);
    }

    // --- 4. readiness requires 200 + a string `version`, not a started process

    void readinessRequiresAVersionStringNotAStartedProcess_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("ready");
        QTest::newRow("200 + string version") << QByteArray(R"({"version":"1.19.31"})") << 200 << true;
        QTest::newRow("500") << QByteArray(R"({"version":"1.19.31"})") << 500 << false;
        QTest::newRow("404") << QByteArray(R"({"version":"1.19.31"})") << 404 << false;
        QTest::newRow("200, no version field") << QByteArray("{}") << 200 << false;
        QTest::newRow("200, version is a number") << QByteArray(R"({"version":1})") << 200 << false;
        QTest::newRow("200, body is not an object") << QByteArray(R"("1.19.31")") << 200 << false;
    }

    void readinessRequiresAVersionStringNotAStartedProcess() {
        QFETCH(QByteArray, body);
        QFETCH(int, status);
        QFETCH(bool, ready);

        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        using Reply = testsupport::LoopbackServer::Reply;
        controller.route("GET", "/version",
                         status == 200 ? Reply::json(body) : Reply::failure(status, body));

        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationSucceeds().printsLine(QStringLiteral("[INFO] up")).runsForever().commit());

        const QString config = writeConfig(controller.port());
        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.setBinaryPath(core.binaryPath());

        const cb::RequestId launch = backend.start(config, environment_->dataDir());
        QVERIFY(launch != cb::RequestId::Invalid);
        // The process comes up. That is NOT readiness.
        QVERIFY(controller.waitFor([&] { return core.invocationCount() >= 2; }));
        QVERIFY(controller.waitFor([&] { return controller.requestCount("GET", "/version") >= 1; }));

        if (ready) {
            QTRY_COMPARE(backend.state(), cb::CoreState::Running);
            QVERIFY(backend.drainPendingEvents());
            QCOMPARE(static_cast<int>(observer.readyEndpoints.size()), 1);
            QCOMPARE(observer.readyEndpoints.front().port, controller.port());
            QVERIFY(cb::isValid(backend.managedEndpoint()));
        } else {
            // Several probes have now been answered unusably and the core is
            // still only Starting.
            QVERIFY(controller.waitFor([&] { return controller.requestCount("GET", "/version") >= 3; }));
            QCOMPARE(backend.state(), cb::CoreState::Starting);
            QVERIFY(backend.drainPendingEvents());
            QVERIFY(observer.readyEndpoints.empty());
            QVERIFY(!cb::isValid(backend.managedEndpoint()));
        }
        backend.removeObserver(&observer);
        backend.stop();
        QTRY_VERIFY(backend.state() == cb::CoreState::Stopped ||
                    backend.state() == cb::CoreState::Failed);
    }

    // --- 5. the idle deadline is refreshed by log output; the hard cap is not

    void logOutputRefreshesTheIdleDeadlineButNotTheHardCap() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        // Never becomes ready: the deadline is the only thing that ends this.
        controller.route("GET", "/version",
                         testsupport::LoopbackServer::Reply::failure(503, "{}"));

        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        core.validationSucceeds();
        // Eight log lines, each released by the test. A core that keeps logging
        // keeps its deadline alive - and the hard cap still ends it.
        for (int index = 0; index < 8; ++index) {
            core.printsLine(QStringLiteral("[INFO] still downloading geodata %1").arg(index));
            core.waitsFor(QStringLiteral("line%1").arg(index));
        }
        QVERIFY(core.runsForever().commit());

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.setBinaryPath(core.binaryPath());
        // The published budget, shrunk through the seam rather than waited out.
        // timings() reports what the backend will REALLY do, which is what the
        // contract publishes it for.
        core::CoreTimings timings;
        timings.probeIntervalMs = 25;
        timings.probeTimeoutMs = 200;
        timings.idleDeadlineMs = 400;
        timings.hardCapMs = 1200;
        timings.terminateWaitMs = 200;
        backend.setTimings(timings);
        QCOMPARE(backend.timings().idleDeadlineMs, 400u);
        QCOMPARE(backend.timings().hardCapMs, 1200u);

        const QString config = writeConfig(controller.port());
        QElapsedTimer elapsed;
        elapsed.start();
        backend.start(config, environment_->dataDir());
        QTRY_COMPARE(backend.state(), cb::CoreState::Starting);

        // Keep it logging every 300 ms - inside the 400 ms idle deadline, so the
        // deadline never expires on its own - past the 1200 ms hard cap.
        for (int index = 0; index < 8 && backend.state() == cb::CoreState::Starting; ++index) {
            QTest::qWait(300);
            if (backend.state() != cb::CoreState::Starting) break;
            QVERIFY(core.release(QStringLiteral("line%1").arg(index)));
        }
        QVERIFY2(elapsed.elapsed() > timings.idleDeadlineMs,
                 "the run did not outlive one idle deadline: nothing was proven");

        QTRY_VERIFY_WITH_TIMEOUT(backend.state() == cb::CoreState::Failed ||
                                     backend.state() == cb::CoreState::Stopped,
                                 5000);
        const qint64 lifetime = elapsed.elapsed();
        QVERIFY2(lifetime >= timings.hardCapMs,
                 qPrintable(QStringLiteral("the core failed after %1 ms, before the %2 ms hard "
                                           "cap: the idle deadline was not refreshed by log output")
                                .arg(lifetime)
                                .arg(timings.hardCapMs)));
        QVERIFY(backend.drainPendingEvents());
        QVERIFY(!observer.coreFailures.empty());
        QCOMPARE(observer.coreFailures.front().error.code, cb::ErrorCode::ReadyTimeout);
        backend.removeObserver(&observer);
    }

    // --- 5, second half. The hard cap is ABSOLUTE.
    //
    // backend-r3 names this a weak-coverage item: the case above asserts only
    // that the core outlived the hard cap, so making readyHardDeadlineMs_
    // refreshable survives it. What follows bounds the run from ABOVE while log
    // output never stops, so a refreshable hard cap has nowhere to hide.

    void theReadinessHardCapIsNotRefreshedByLogOutput() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        // Never becomes ready: only a deadline can end this run.
        controller.route("GET", "/version",
                         testsupport::LoopbackServer::Reply::failure(503, "{}"));

        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        core.validationSucceeds();
        // Enough gated lines to keep talking for 3.6 s - six times the hard cap.
        for (int index = 0; index < kChatteringLines; ++index) {
            core.printsLine(QStringLiteral("[INFO] still downloading geodata %1").arg(index));
            core.waitsFor(QStringLiteral("line%1").arg(index));
        }
        QVERIFY(core.runsForever().commit());

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.setBinaryPath(core.binaryPath());
        core::CoreTimings timings;
        timings.probeIntervalMs = 25;
        timings.probeTimeoutMs = 200;
        // The idle deadline is set far beyond anything this case spans, so it
        // CANNOT be what ends the run. Only the hard cap can.
        timings.idleDeadlineMs = 5000;
        timings.hardCapMs = 600;
        timings.terminateWaitMs = 200;
        backend.setTimings(timings);

        QElapsedTimer elapsed;
        elapsed.start();
        backend.start(writeConfig(controller.port()), environment_->dataDir());
        QTRY_COMPARE(backend.state(), cb::CoreState::Starting);

        // A log line every 150 ms, without a pause, for as long as the core
        // lives. Every one of them refreshes the idle deadline; none of them may
        // touch the hard cap.
        int released = 0;
        while (released < kChatteringLines && backend.state() == cb::CoreState::Starting) {
            QTest::qWait(150);
            if (backend.state() != cb::CoreState::Starting) break;
            QVERIFY(core.release(QStringLiteral("line%1").arg(released)));
            ++released;
        }
        const qint64 lifetime = elapsed.elapsed();

        QVERIFY2(released >= 3,
                 qPrintable(QStringLiteral("only %1 log lines were released before the core "
                                           "died: it was never given the chance to refresh "
                                           "anything and this case proves nothing")
                                .arg(released)));
        QVERIFY2(backend.state() != cb::CoreState::Starting,
                 qPrintable(QStringLiteral("the core was still Starting after %1 ms of "
                                           "uninterrupted log output, %2 times its %3 ms hard "
                                           "cap: the hard cap was refreshed by log output and "
                                           "is not absolute")
                                .arg(lifetime)
                                .arg(lifetime / timings.hardCapMs)
                                .arg(timings.hardCapMs)));
        QVERIFY2(lifetime >= timings.hardCapMs,
                 "the core died before its own hard cap expired");
        QVERIFY2(lifetime < timings.idleDeadlineMs,
                 qPrintable(QStringLiteral("the run lasted %1 ms, past the %2 ms idle deadline, "
                                           "so the deadline that ended it cannot be shown to be "
                                           "the hard cap")
                                .arg(lifetime)
                                .arg(timings.idleDeadlineMs)));

        QTRY_VERIFY_WITH_TIMEOUT(backend.state() == cb::CoreState::Failed ||
                                     backend.state() == cb::CoreState::Stopped,
                                 5000);
        QVERIFY(backend.drainPendingEvents());
        QVERIFY(!observer.coreFailures.empty());
        QCOMPARE(observer.coreFailures.front().error.code, cb::ErrorCode::ReadyTimeout);
        backend.removeObserver(&observer);
    }

    // --- 1, second half. Stop terminates ONLY a managed core.
    //
    // backend-r3 names this a weak-coverage item too: no case in this suite ever
    // had a managed core and an external controller alive at the same moment, so
    // the word "only" was never under test. Here both are live, and the external
    // one has to still be serving after the managed child has been terminated.

    void stopTerminatesOnlyTheManagedCoreAndLeavesAnExternalControllerAlive() {
        testsupport::LoopbackServer managed;   // what the managed child listens on
        testsupport::LoopbackServer external;  // a controller nobody here started
        QVERIFY(managed.listen(QString()));
        QVERIFY(external.listen());
        scriptController(managed, R"({"version":"managed-core"})");
        scriptController(external, R"({"version":"external-core"})");

        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationSucceeds().printsLine(QStringLiteral("[INFO] up")).runsForever().commit());

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.setBinaryPath(core.binaryPath());
        backend.start(writeConfig(managed.port()), environment_->dataDir());
        QTRY_COMPARE(backend.state(), cb::CoreState::Running);
        const int childInvocations = core.invocationCount();

        // Both alive, and independently observable - which is what section 1
        // says the contract must answer directly instead of making a consumer
        // compare endpoints by hand.
        backend.attach(endpointOf(external));
        QVERIFY(external.waitFor([&] { return backend.isConnected(); }));
        QCOMPARE(backend.ownership(), cb::Ownership::Managed);
        QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Attached);
        QVERIFY(backend.isExternalControllerConnected());
        QCOMPARE(backend.managedEndpoint().port, managed.port());
        QCOMPARE(backend.currentEndpoint().port, external.port());
        const int externalVersionsBefore = external.requestCount("GET", "/version");

        const cb::RequestId stopRequest = backend.stop();
        QVERIFY(stopRequest != cb::RequestId::Invalid);

        // The managed child is gone. Stopped is reached only when its exit is
        // observed, so this is the child's death, not a state flag.
        QTRY_COMPARE(backend.state(), cb::CoreState::Stopped);
        QVERIFY(backend.drainPendingEvents());
        QCOMPARE(static_cast<int>(observer.stops.size()), 1);
        QCOMPARE(observer.stops.front().request, stopRequest);
        QVERIFY(observer.stops.front().confirmed);
        QVERIFY(!cb::isValid(backend.managedEndpoint()));
        QCOMPARE(backend.ownership(), cb::Ownership::None);
        QCOMPARE(core.invocationCount(), childInvocations);  // nothing relaunched

        // And the external controller was not touched by any of it: still
        // attached, still connected, and still answering. Re-issuing against it
        // proves that rather than assuming it.
        QVERIFY2(backend.isAttached(),
                 "stop() detached a controller this component never started");
        QCOMPARE(backend.currentEndpoint().port, external.port());
        QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Attached);
        const cb::RequestId probe = backend.refreshVersion();
        QVERIFY(probe != cb::RequestId::Invalid);
        QVERIFY2(external.waitFor([&] {
                     const auto *event = observer.find(QStringLiteral("version"), probe);
                     return event && event->completion.isOk();
                 }),
                 "the external controller stopped answering after a stop that was "
                 "supposed to reach only the managed core");
        QCOMPARE(observer.find(QStringLiteral("version"), probe)->payload,
                 QStringLiteral("external-core"));
        QVERIFY(external.requestCount("GET", "/version") > externalVersionsBefore);
        QVERIFY(!external.sawUnexpectedRequest());
        backend.removeObserver(&observer);
    }

    // --- section 5.2. The probe disconnects BEFORE it aborts.
    //
    // The third weak-coverage item backend-r3 names. Inverting the two
    // statements in cancelProbe() used to change nothing observable, because the
    // handler's own identity guard swallowed the late completion. CoreProcess
    // now counts what that guard swallows, so the ordering is a fact a test can
    // assert instead of a comment.

    void aCancelledReadinessProbeDisconnectsBeforeItAborts() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        // The probe is parked on the wire: sent, unanswered, and abortable.
        auto *held = controller.hold("GET", "/version");

        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationSucceeds().printsLine(QStringLiteral("[INFO] up")).runsForever().commit());

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.setBinaryPath(core.binaryPath());
        core::CoreTimings timings;
        timings.probeIntervalMs = 25;
        // Nothing here may expire: the cancellation is the only thing that ends
        // the held probe.
        timings.probeTimeoutMs = 30000;
        timings.idleDeadlineMs = 30000;
        timings.hardCapMs = 60000;
        timings.terminateWaitMs = 200;
        backend.setTimings(timings);

        backend.start(writeConfig(controller.port()), environment_->dataDir());
        QVERIFY2(held->waitForPending(1), qPrintable(controller.pendingReport()));
        QCOMPARE(backend.state(), cb::CoreState::Starting);
        QCOMPARE(backend.probeCompletionsAfterCancel(), 0);

        // Section 6: stop cancels the readiness probe. QNetworkReply::abort()
        // emits finished() synchronously, so by the time it runs the handler
        // must already be disconnected.
        backend.stop();
        QTRY_VERIFY(backend.state() == cb::CoreState::Stopped ||
                    backend.state() == cb::CoreState::Failed);

        QVERIFY2(backend.probeCompletionsAfterCancel() == 0,
                 "a cancelled readiness probe delivered a completion into a handler "
                 "that was being torn down: the abort ran before the disconnect "
                 "(contract section 5.2)");
        QVERIFY(backend.drainPendingEvents());
        QVERIFY2(observer.readyEndpoints.empty(),
                 "a cancelled probe still reported the core ready");
        QVERIFY(!cb::isValid(backend.managedEndpoint()));
        held->release();
        backend.removeObserver(&observer);
    }

    // --- backend-r3 B2. A cancelled TUN change is a SUPERSESSION.

    void aTunChangeCancelledByAnEndpointChangeIsSupersededNotAProtocolError() {
        testsupport::LoopbackServer first;
        testsupport::LoopbackServer second;
        QVERIFY(first.listen());
        QVERIFY(second.listen());
        scriptController(first);
        scriptController(second);
        // /version is left free, so the client still reaches "connected"; only
        // the TUN change's own /configs read is parked.
        auto *configs = first.hold("GET", "/configs");

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.attach(endpointOf(first));
        QVERIFY(first.waitFor([&] { return backend.isConnected(); }));
        const int parkedByAttach = configs->pending();

        const cb::RequestId change = backend.setTunEnabled(true);
        QVERIFY(change != cb::RequestId::Invalid);
        QVERIFY(backend.isTunChangePending());
        QVERIFY2(configs->waitForPending(parkedByAttach + 1),
                 qPrintable(first.pendingReport()));

        // The controller changes underneath the outstanding change. MihomoClient
        // cancels it; that is not the controller answering unusably.
        backend.attach(endpointOf(second));
        QVERIFY(backend.drainPendingEvents());
        QVERIFY(second.waitFor([&] { return !observer.tunChanges.empty(); }));

        const cb::TunChangeCompleted &result = observer.tunChanges.front();
        QCOMPARE(result.request, change);
        QVERIFY2(result.status == cb::CompletionStatus::Superseded,
                 "a TUN change cancelled by an endpoint change was not MARKED "
                 "superseded (backend-r2 A2, backend-r3 B2)");
        QVERIFY2(result.error.code != cb::ErrorCode::Protocol,
                 "a supersession was reported as a protocol error (backend-r3 B2)");
        QCOMPARE(result.error.code, cb::ErrorCode::Superseded);
        // The generation comparison stays the consumer's second line of defence.
        QVERIFY2(cb::isSuperseded(result.generation, observer.lastObserved()),
                 "the superseded TUN completion did not compare older than the "
                 "newest observed generation");
        QVERIFY(!backend.isTunChangePending());
        configs->release();
        backend.removeObserver(&observer);
    }

    // --- 6. a validation failure leaves the running configuration intact

    void aValidationFailureLeavesTheRunningConfigurationIntact() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);

        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationSucceeds().printsLine(QStringLiteral("[INFO] up")).runsForever().commit());

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.setBinaryPath(core.binaryPath());
        const QString running = writeConfig(controller.port(), QStringLiteral("cfg-a.yaml"));
        backend.start(running, environment_->dataDir());
        QTRY_COMPARE(backend.state(), cb::CoreState::Running);
        QCOMPARE(backend.activeConfigPaths(), QStringList{running});

        const QString candidate = writeConfig(controller.port(), QStringLiteral("cfg-b.yaml"));
        QVERIFY(core.validationFails(QStringLiteral("unknown field: tun.stakc")).runsForever().commit());
        backend.start(candidate, environment_->dataDir());
        // The old core stays live while its replacement is validated.
        QCOMPARE(backend.state(), cb::CoreState::Running);
        QVERIFY(backend.activeConfigPaths().contains(running));
        QVERIFY(backend.activeConfigPaths().contains(candidate));

        QTRY_VERIFY(!backend.isRestartPending());
        QVERIFY(backend.drainPendingEvents());
        QCOMPARE(backend.state(), cb::CoreState::Running);
        QCOMPARE(backend.activeConfigPaths(), QStringList{running});
        QCOMPARE(backend.managedEndpoint().port, controller.port());
        QVERIFY(!observer.coreFailures.empty());
        QCOMPARE(observer.coreFailures.back().error.code, cb::ErrorCode::ValidationFailed);
        backend.removeObserver(&observer);
        backend.stop();
        QTRY_VERIFY(backend.state() == cb::CoreState::Stopped);
    }

    // --- 7. a restart consumes its pending launch only after the prior exit

    void aPendingLaunchIsConsumedOnlyAfterThePriorExitIsObserved() {
#ifdef Q_OS_WIN
        QSKIP("terminate() cannot be refused by a console child on Windows");
#else
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);

        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationSucceeds()
                    .ignoresTerminate()
                    .printsLine(QStringLiteral("[INFO] up"))
                    .runsForever()
                    .commit());

        core::MihomoBackendImpl backend;
        backend.setBinaryPath(core.binaryPath());
        core::CoreTimings timings;
        timings.terminateWaitMs = 300;
        timings.probeIntervalMs = 25;
        backend.setTimings(timings);

        const QString first = writeConfig(controller.port(), QStringLiteral("cfg-a.yaml"));
        backend.start(first, environment_->dataDir());
        QTRY_COMPARE(backend.state(), cb::CoreState::Running);
        const int launchedOnce = core.invocationCount();

        const QString second = writeConfig(controller.port(), QStringLiteral("cfg-b.yaml"));
        backend.start(second, environment_->dataDir());
        QTRY_COMPARE(backend.state(), cb::CoreState::Stopping);
        QVERIFY2(backend.isRestartPending(), "the launch was applied instead of held");
        QVERIFY2(backend.isManagedCoreActive(),
                 "Stopping with a pending restart is the reload gate's active case");
        QVERIFY(backend.activeConfigPaths().contains(second));
        // The replacement must NOT be launched before the prior exit. The child
        // refuses terminate(), so this is a real window, not a formality.
        QCOMPARE(core.invocationCount(), launchedOnce + 1);  // the validator only

        // The kill escalation is what ends it; the exit is what consumes the
        // pending launch.
        QTRY_VERIFY_WITH_TIMEOUT(backend.state() == cb::CoreState::Starting ||
                                     backend.state() == cb::CoreState::Running,
                                 5000);
        QVERIFY(!backend.isRestartPending());
        QCOMPARE(backend.activeConfigPaths(), QStringList{second});
        backend.stop();
        QTRY_VERIFY(backend.state() == cb::CoreState::Stopped);
#endif
    }

    // --- 8. a cancelled validation keeps its configuration path active

    void aCancelledValidationKeepsItsConfigurationPathActiveUntilItExits() {
        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationWaitsFor(QStringLiteral("validate")).runsForever().commit());

        core::MihomoBackendImpl backend;
        backend.setBinaryPath(core.binaryPath());
        const QString first = writeConfig(9090, QStringLiteral("cfg-a.yaml"));
        const QString second = writeConfig(9090, QStringLiteral("cfg-b.yaml"));

        backend.start(first, environment_->dataDir());
        QVERIFY(backend.isRestartPending());
        QVERIFY(backend.activeConfigPaths().contains(first));

        // A second launch cancels the first validator. Its child is dying, not
        // dead, and its snapshot must not be deleted out from under it.
        backend.start(second, environment_->dataDir());
        QVERIFY2(backend.activeConfigPaths().contains(first),
                 "a cancelled validator's config left the active set before it exited");
        QVERIFY(backend.activeConfigPaths().contains(second));

        // Once the killed validator is reaped, and only then, its path goes.
        QTRY_VERIFY(!backend.activeConfigPaths().contains(first));
        QVERIFY(backend.activeConfigPaths().contains(second));
        backend.stop();
    }

    // --- 9. confirmed == false is reported as such and is not a success

    void anUnconfirmedStopIsReportedAsUnconfirmed() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);

        StubPrivilegedService service(endpointOf(controller));
        core::MihomoBackendImpl backend(&service);
        Recorder observer(&backend);
        backend.addObserver(&observer);

        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationSucceeds().runsForever().commit());
        backend.setBinaryPath(core.binaryPath());

        QVERIFY(backend.serviceSupported());
        QVERIFY(backend.features().has(cb::Feature::PrivilegedService));
        QVERIFY2(backend.setExecutionMode(cb::ExecutionMode::PrivilegedService),
                 "the injected privileged service was refused");

        backend.start(writeConfig(controller.port()), environment_->dataDir());
        QTRY_COMPARE(backend.state(), cb::CoreState::Running);
        QVERIFY(backend.usesPrivilegedService());

        const cb::RequestId stopRequest = backend.stop();
        QVERIFY(stopRequest != cb::RequestId::Invalid);
        QVERIFY(backend.drainPendingEvents());
        QCOMPARE(backend.state(), cb::CoreState::Stopping);
        QVERIFY(service.stopRequested());
        QVERIFY2(observer.stops.empty(), "stop answered before anything confirmed the exit");

        service.disconnectWithoutConfirming(
            QStringLiteral("the helper socket closed mid-shutdown"));
        QVERIFY(backend.drainPendingEvents());

        QCOMPARE(static_cast<int>(observer.stops.size()), 1);
        const cb::StopCompleted &result = observer.stops.front();
        QCOMPARE(result.request, stopRequest);
        QVERIFY2(!result.confirmed, "an unconfirmed stop was reported as confirmed");
        QVERIFY(result.reason.isFailure());
        QCOMPARE(result.reason.code, cb::ErrorCode::ServiceDisconnected);
        QVERIFY(!result.reason.message.isEmpty());
        QCOMPARE(backend.state(), cb::CoreState::Failed);
        QCOMPARE(observer.stoppedCount, 0);

        // backend-r3 B2: stated, not inferred from a bool. An unconfirmed stop
        // is a Failed completion; it is emphatically not Ok.
        QCOMPARE(result.status, cb::CompletionStatus::Failed);

        // backend-r3 B1, and the whole point of this arm.
        //
        // CoreProcess emits failed() BEFORE stopFinished() here
        // (core_process.cpp:486-491) and the failed handler bumps the
        // generation, so the delivered order is coreFailed(N+1) then
        // stopCompleted(N). Until the fix, this stop therefore compared OLDER
        // than an event already delivered, and a consumer applying section 2's
        // mandatory rejection rule dropped the unconfirmed stop - the shutdown
        // warning that blocks quit simply never appeared.
        QVERIFY2(!observer.coreFailures.empty(),
                 "no coreFailed preceded the unconfirmed stop: this arm proves nothing");
        const cb::Generation failedGeneration = observer.coreFailures.back().generation;
        qInfo().noquote() << "stopGen=" << cb::number(result.generation)
                          << " coreFailedGen=" << cb::number(failedGeneration)
                          << " lastObserved=" << cb::number(observer.lastObserved());
        QVERIFY2(!cb::isSuperseded(result.generation, failedGeneration),
                 "the stop completion was stamped older than the coreFailed its own "
                 "teardown produced (backend-r3 B1)");
        QVERIFY2(!cb::isSuperseded(result.generation, observer.lastObserved()),
                 "a consumer applying section 2's rejection rule would have DROPPED "
                 "this unconfirmed stop (backend-r3 B1)");
        backend.removeObserver(&observer);
    }

    void aConfirmedStopIsReportedAsConfirmed() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);

        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationSucceeds().printsLine(QStringLiteral("[INFO] up")).runsForever().commit());

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.setBinaryPath(core.binaryPath());
        backend.start(writeConfig(controller.port()), environment_->dataDir());
        QTRY_COMPARE(backend.state(), cb::CoreState::Running);

        const cb::RequestId stopRequest = backend.stop();
        QTRY_COMPARE(backend.state(), cb::CoreState::Stopped);
        QVERIFY(backend.drainPendingEvents());
        QCOMPARE(static_cast<int>(observer.stops.size()), 1);
        QCOMPARE(observer.stops.front().request, stopRequest);
        QVERIFY(observer.stops.front().confirmed);
        QVERIFY(!observer.stops.front().reason.isFailure());
        QCOMPARE(observer.stoppedCount, 1);
        QCOMPARE(observer.stops.front().status, cb::CompletionStatus::Ok);
        // The same B1 obligation on the path that does confirm.
        QVERIFY2(!cb::isSuperseded(observer.stops.front().generation, observer.lastObserved()),
                 "a confirmed stop compared older than the newest observed generation");
        backend.removeObserver(&observer);
    }

    // --- 10. no observer is invoked re-entrantly from within a mutating call

    void noObserverIsInvokedFromInsideAMutatingCall() {
        testsupport::LoopbackServer controller;
        testsupport::LoopbackServer other;
        QVERIFY(controller.listen());
        QVERIFY(other.listen());
        scriptController(controller);
        scriptController(other);

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);

        // Every mutating path, including the one that emits five signals from
        // inside a single call today (MihomoClient::clearLiveState).
        backend.attach(endpointOf(controller));
        QVERIFY(!observer.sawReentrantDelivery);
        QVERIFY(controller.waitFor([&] { return backend.isConnected(); }));
        backend.setMode(QStringLiteral("global"));
        backend.refreshVersion();
        backend.refreshProxies();
        backend.refreshRules();
        backend.refreshConfig();
        backend.attach(endpointOf(other));  // clears live state
        backend.detach();                   // clears it again
        backend.setBinaryPath(QStringLiteral("/nonexistent/mihomo"));
        backend.start(writeConfig(controller.port()), environment_->dataDir());
        backend.stop();
        QVERIFY(backend.drainPendingEvents());
        QTest::qWait(50);
        QVERIFY(backend.drainPendingEvents());

        QVERIFY2(!observer.sawReentrantDelivery,
                 "an observer was invoked from inside a mutating call");
        QVERIFY2(observer.callbackCount > 20,
                 "the observer received almost nothing: the assertion above is vacuous");
        QVERIFY2(observer.liveStateClears >= 2, "live state was never cleared");
        backend.removeObserver(&observer);
    }

    void aMutationFromInsideACallbackIsAlsoDeliveredLater() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen());
        scriptController(controller);

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        observer.onNextEvent = [&backend] { backend.refreshVersion(); };
        backend.attach(endpointOf(controller));
        QVERIFY(controller.waitFor([&] {
            return observer.countOf(QStringLiteral("version"), cb::CompletionStatus::Ok) > 0;
        }));
        QVERIFY(!observer.sawReentrantDelivery);
        backend.removeObserver(&observer);
    }

    // --- 11. removing an observer during delivery is safe

    void removingAnObserverDuringDeliveryIsSafe() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen());
        scriptController(controller);

        core::MihomoBackendImpl backend;
        Recorder first(&backend, QStringLiteral("first"));
        Recorder second(&backend, QStringLiteral("second"));
        Recorder third(&backend, QStringLiteral("third"));
        QVERIFY(backend.addObserver(&first));
        QVERIFY(backend.addObserver(&second));
        QVERIFY(backend.addObserver(&third));
        QVERIFY(!backend.addObserver(&first));
        QVERIFY(!backend.addObserver(nullptr));

        second.onNextEvent = [&] {
            backend.removeObserver(&second);
            backend.removeObserver(&third);
        };

        backend.attach(endpointOf(controller));
        QVERIFY(controller.waitFor([&] { return first.callbackCount > 3; }));
        QVERIFY(backend.drainPendingEvents());

        QCOMPARE(second.callbackCount, 1);
        QVERIFY2(third.callbackCount == 0,
                 "an observer removed during delivery was invoked afterwards");
        QVERIFY(!backend.removeObserver(&second));
        QVERIFY(backend.removeObserver(&first));
    }

    void anObserverAddedDuringDeliverySeesOnlyLaterEvents() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen());
        scriptController(controller);

        core::MihomoBackendImpl backend;
        Recorder first(&backend);
        Recorder late(&backend);
        backend.addObserver(&first);
        first.onNextEvent = [&] { backend.addObserver(&late); };
        backend.attach(endpointOf(controller));
        QVERIFY(controller.waitFor([&] { return first.callbackCount > 3; }));
        QVERIFY(backend.drainPendingEvents());
        QVERIFY(first.callbackCount > late.callbackCount);
        QVERIFY(!late.sawReentrantDelivery);
        backend.removeObserver(&first);
        backend.removeObserver(&late);
    }

    // ------------------------------------------ supporting contract semantics

    void theTunChangeReportsAReadBackNotAnEcho() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        using Reply = testsupport::LoopbackServer::Reply;
        scriptController(controller);
        // The controller accepts the PATCH and then reports TUN still disabled:
        // exactly what happens when the core cannot create the interface.
        controller.route("PATCH", "/configs", Reply::json("{}"));

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.attach(endpointOf(controller));
        QVERIFY(controller.waitFor([&] { return backend.isConnected(); }));

        const cb::RequestId change = backend.setTunEnabled(true);
        QVERIFY(change != cb::RequestId::Invalid);
        QVERIFY(backend.isTunChangePending());
        // Further calls are rejected while one is pending.
        QCOMPARE(backend.setTunEnabled(false), cb::RequestId::Invalid);

        QVERIFY(controller.waitFor([&] { return !observer.tunChanges.empty(); }));
        QCOMPARE(observer.tunChanges.front().request, change);
        QVERIFY(observer.tunChanges.front().requested);
        QVERIFY2(!observer.tunChanges.front().actual,
                 "the completion echoed the request instead of reading the state back");
        QVERIFY(observer.tunChanges.front().error.isFailure());
        QVERIFY(!backend.isTunChangePending());
        backend.removeObserver(&observer);
    }

    // backend-r2's answer to the first open question, asserted against the real
    // ProviderClient: pending_ is a COALESCING key, so a duplicate submission
    // returns the OUTSTANDING request's id instead of minting a second one.
    void anIdenticalProviderRequestIsCoalescedOntoTheOutstandingOne() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        using Reply = testsupport::LoopbackServer::Reply;
        controller.route("GET", "/providers/rules", Reply::json(R"({"providers":{}})"));
        controller.route("GET", "/providers/proxies", Reply::json(R"({"providers":{}})"));
        auto *held = controller.hold("GET", "/providers/rules");

        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        backend.attach(endpointOf(controller));
        QVERIFY(controller.waitFor([&] { return backend.isConnected(); }));

        const cb::RequestId first = backend.fetchProviders(true);
        const cb::RequestId again = backend.fetchProviders(true);
        const cb::RequestId other = backend.fetchProviders(false);
        QVERIFY(first != cb::RequestId::Invalid);
        QCOMPARE(again, first);
        QVERIFY(other != first);
        QVERIFY(backend.isProviderBusy());
        QVERIFY(held->waitForPending(1));
        QCOMPARE(held->pending(), 1);  // one issued request, not two

        held->release();
        QVERIFY(controller.waitFor([&] { return !backend.isProviderBusy(); }));
        QVERIFY(backend.drainPendingEvents());
        QCOMPARE(observer.countOf(QStringLiteral("providers/rules"), cb::CompletionStatus::Ok), 1);
        QCOMPARE(observer.countOf(QStringLiteral("providers/proxies"), cb::CompletionStatus::Ok), 1);
        backend.removeObserver(&observer);
    }

    void executionModeIsRefusedWhileTheCoreIsLive() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        testsupport::FakeCore core(environment_->dataDir());
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationSucceeds().printsLine(QStringLiteral("[INFO] up")).runsForever().commit());

        StubPrivilegedService service(endpointOf(controller));
        core::MihomoBackendImpl backend(&service);
        backend.setBinaryPath(core.binaryPath());
        backend.start(writeConfig(controller.port()), environment_->dataDir());
        QTRY_COMPARE(backend.state(), cb::CoreState::Running);

        QVERIFY2(!backend.setExecutionMode(cb::ExecutionMode::PrivilegedService),
                 "the execution mode changed under a running core");
        QCOMPARE(backend.executionMode(), cb::ExecutionMode::Managed);
        backend.stop();
        QTRY_COMPARE(backend.state(), cb::CoreState::Stopped);
        QVERIFY(backend.setExecutionMode(cb::ExecutionMode::PrivilegedService));
    }

    void capabilitiesAreQueriedFromTheInstance() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        StubPrivilegedService service(endpointOf(controller));
        // Two instances, answering independently: no process-global statics.
        core::MihomoBackendImpl withService(&service);
        core::MihomoBackendImpl withoutService;
        QVERIFY(withService.serviceSupported());
        QVERIFY(withService.serviceAvailable());
        QVERIFY2(!withoutService.serviceSupported(),
                 "a backend given no privileged service claimed to have one");
        QVERIFY(!withoutService.serviceAvailable());
        QVERIFY(withService.features().has(cb::Feature::PrivilegedService));
        QVERIFY(!withoutService.features().has(cb::Feature::PrivilegedService));
        // And a backend with no service refuses service mode rather than
        // selecting it and then wedging.
        QVERIFY(!withoutService.setExecutionMode(cb::ExecutionMode::PrivilegedService));
    }

    void aPrivilegedServiceStatusQueryIsAnsweredWithoutASecondConnection() {
        testsupport::LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        StubPrivilegedService service(endpointOf(controller));
        core::MihomoBackendImpl backend(&service);
        Recorder observer(&backend);
        backend.addObserver(&observer);

        const cb::RequestId status = backend.requestPrivilegedServiceStatus();
        QVERIFY(status != cb::RequestId::Invalid);
        service.deliverStatus();
        QVERIFY(backend.drainPendingEvents());
        const auto *event = observer.find(QStringLiteral("service-status"), status);
        QVERIFY(event);
        QCOMPARE(event->completion.status, cb::CompletionStatus::Ok);
        QCOMPARE(event->payload, QStringLiteral("helper-1.2.3"));
        backend.removeObserver(&observer);

        // A backend with no privileged service rejects the query outright.
        core::MihomoBackendImpl bare;
        Recorder bareObserver(&bare);
        bare.addObserver(&bareObserver);
        QCOMPARE(bare.requestPrivilegedServiceStatus(), cb::RequestId::Invalid);
        QVERIFY(bare.drainPendingEvents());
        QCOMPARE(bareObserver.countOf(QStringLiteral("service-status"),
                                      cb::CompletionStatus::Rejected), 1);
        bare.removeObserver(&bareObserver);
    }

    // The helper's own running-core report reaches the published status.
    //
    // The macOS helper keeps ONE core for the machine and answers `status` from
    // it on every connection, so its `state` field is "running" even when the
    // core belongs to another app session. That is the fact the UI's uninstall
    // guard is built on. When the page's second PrivilegedServiceClient was
    // removed, the contract did not yet carry this key, so the guard went inert;
    // this case is what keeps the producer honest.
    void theHelpersRunningCoreReportReachesThePublishedStatus() {
        struct Capture final : cb::BackendObserver {
            int answers = 0;
            cb::PrivilegedServiceStatus last;
            void privilegedServiceStatus(const cb::Completion &,
                                         const cb::PrivilegedServiceStatus &status) noexcept override {
                ++answers;
                last = status;
            }
        };

        // Out-parameter rather than a return value, so QVERIFY inside reports
        // the failure instead of turning into a compile error.
        const auto publishFor = [](const QString &helperState, Capture &capture) {
            testsupport::LoopbackServer controller;
            QVERIFY(controller.listen(QString()));
            StubPrivilegedService service(endpointOf(controller));
            core::MihomoBackendImpl backend(&service);
            backend.addObserver(&capture);
            QVERIFY(backend.requestPrivilegedServiceStatus() != cb::RequestId::Invalid);
            QJsonObject payload;
            payload.insert(QStringLiteral("state"), helperState);
            payload.insert(QStringLiteral("version"), QStringLiteral("helper-1.2.3"));
            service.deliverStatusPayload(payload);
            QVERIFY(backend.drainPendingEvents());
            backend.removeObserver(&capture);
        };

        Capture stopped;
        publishFor(QStringLiteral("stopped"), stopped);
        QCOMPARE(stopped.answers, 1);
        QCOMPARE(stopped.last.state, cb::ServiceState::Connected);
        QCOMPARE(stopped.last.version, QStringLiteral("helper-1.2.3"));
        QVERIFY2(!stopped.last.coreRunning,
                 "the helper reported no core and the backend published one");

        Capture running;
        publishFor(QStringLiteral("running"), running);
        QCOMPARE(running.answers, 1);
        QCOMPARE(running.last.state, cb::ServiceState::Connected);
        QVERIFY2(running.last.coreRunning,
                 "the helper reported a running core and the backend dropped it");
    }

    void aMissingManagedEngineFailsWithAnActionableMessage() {
        core::MihomoBackendImpl backend;
        Recorder observer(&backend);
        backend.addObserver(&observer);
        // No staged engine beside this test binary and no CLASH_QT_CORE_BINARY.
        environment_->unsetEnvironment("CLASH_QT_CORE_BINARY");
        QVERIFY2(backend.discoverBinary().isEmpty(),
                 "the managed path resolved an engine this application did not stage");

        backend.start(writeConfig(9090), environment_->dataDir());
        QVERIFY(backend.drainPendingEvents());
        QVERIFY(!observer.coreFailures.empty());
        const cb::Completion &failure = observer.coreFailures.front();
        QCOMPARE(failure.error.code, cb::ErrorCode::BinaryNotFound);
        QVERIFY2(failure.error.message.contains(QStringLiteral("make core")),
                 "the failure did not say how to fix it");
        QVERIFY2(!failure.error.message.contains(QStringLiteral("PATH or in a local Clash")),
                 "the message still advertises a fallback to another installation");
        backend.removeObserver(&observer);
    }

    // ===================================================================
    // The shared contract. One set of assertions, four implementations.
    //
    // Each pair below is a data-driven slot: the _data() half is the ONE row
    // builder every shared case uses, so a case cannot quietly run against
    // three of the four; the other half runs the implementation-neutral
    // scenario from tests/contracts/backend/common/. Nothing is asserted here
    // - the statements live in backend_common_cases.cpp and are reached
    // through core::backend::MihomoBackend & alone.
    // ===================================================================

    void sharedIdentityAndPublishedBudget_data() { shared::addSubjectRows(); }
    void sharedIdentityAndPublishedBudget() { runShared(&scenarios::identityAndPublishedBudget); }

    void sharedRequestIdentityAndLosslessPayloads_data() { shared::addSubjectRows(); }
    void sharedRequestIdentityAndLosslessPayloads() {
        runShared(&scenarios::requestIdentityAndLosslessPayloads);
    }

    void sharedObserverRegistrationIsExplicit_data() { shared::addSubjectRows(); }
    void sharedObserverRegistrationIsExplicit() {
        runShared(&scenarios::observerRegistrationIsExplicit);
    }

    void sharedRemovingAnObserverDuringDeliveryIsSafe_data() { shared::addSubjectRows(); }
    void sharedRemovingAnObserverDuringDeliveryIsSafe() {
        runShared(&scenarios::removingAnObserverDuringDeliveryStopsFurtherCallbacks);
    }

    void sharedObserverAddedDuringACallbackSeesOnlyLaterEvents_data() { shared::addSubjectRows(); }
    void sharedObserverAddedDuringACallbackSeesOnlyLaterEvents() {
        runShared(&scenarios::anObserverAddedDuringACallbackSeesOnlyLaterEvents);
    }

    void sharedObserverAddedBeforeDeliverySeesNoEarlierEvents_data() { shared::addSubjectRows(); }
    void sharedObserverAddedBeforeDeliverySeesNoEarlierEvents() {
        runShared(&scenarios::anObserverAddedBeforeDeliverySeesNoEarlierEvents);
    }

    void sharedDeliveryIsNeverReentrant_data() { shared::addSubjectRows(); }
    void sharedDeliveryIsNeverReentrant() { runShared(&scenarios::deliveryIsNeverReentrant); }

    void sharedAbandonedCompletionIsMarkedSuperseded_data() { shared::addSubjectRows(); }
    void sharedAbandonedCompletionIsMarkedSuperseded() {
        runShared(&scenarios::anAbandonedCompletionIsMarkedSuperseded);
    }

    void sharedNonEndpointBumpReissuesTheSnapshotSet_data() { shared::addSubjectRows(); }
    void sharedNonEndpointBumpReissuesTheSnapshotSet() {
        runShared(&scenarios::aNonEndpointBumpReissuesTheSnapshotSet);
    }

    void sharedDetachLeavesTheAttachedControllerRunning_data() { shared::addSubjectRows(); }
    void sharedDetachLeavesTheAttachedControllerRunning() {
        runShared(&scenarios::detachLeavesTheAttachedControllerRunning);
    }

    void sharedStopTerminatesOnlyTheManagedCore_data() { shared::addSubjectRows(); }
    void sharedStopTerminatesOnlyTheManagedCore() {
        runShared(&scenarios::stopTerminatesOnlyTheManagedCore);
    }

    void sharedFailedValidationLeavesTheRunningConfigurationIntact_data() {
        shared::addSubjectRows();
    }
    void sharedFailedValidationLeavesTheRunningConfigurationIntact() {
        runShared(&scenarios::aFailedValidationLeavesTheRunningConfigurationIntact);
    }

    void sharedDuplicateProviderRequestIsCoalesced_data() { shared::addSubjectRows(); }
    void sharedDuplicateProviderRequestIsCoalesced() {
        runShared(&scenarios::aDuplicateProviderRequestIsCoalesced);
    }

    void sharedReplacementAtTheSameAddressOpensANewSession_data() { shared::addSubjectRows(); }
    void sharedReplacementAtTheSameAddressOpensANewSession() {
        runShared(&scenarios::aReplacementAtTheSameAddressOpensANewSession);
    }

    void sharedLifecycleBumpRetiresOutstandingWork_data() { shared::addSubjectRows(); }
    void sharedLifecycleBumpRetiresOutstandingWork() {
        runShared(&scenarios::aLifecycleBumpRetiresOutstandingWork);
    }

    void sharedTunCompletionIsAReadBackNotAnEcho_data() { shared::addSubjectRows(); }
    void sharedTunCompletionIsAReadBackNotAnEcho() {
        runShared(&scenarios::theTunCompletionIsAReadBackNotAnEcho);
    }

    void sharedPrivilegedStatusCarriesTheHelpersRunningCore_data() { shared::addSubjectRows(); }
    void sharedPrivilegedStatusCarriesTheHelpersRunningCore() {
        runShared(&scenarios::thePrivilegedStatusCarriesTheHelpersRunningCore);
    }

  private:
    // Log lines the chattering core emits, one per 150 ms: 3.6 s of continuous
    // output against a 600 ms hard cap.
    static constexpr int kChatteringLines = 24;

    /// Builds the row's subject and runs one shared scenario against it.
    ///
    /// EVERY ROW IS REQUIRED. A module artifact the build did not supply, or
    /// one that will not load, FAILS by name: the integration note for this
    /// lane is explicit that a skipped required row while CTest prints Passed
    /// is the failure mode the shared suite exists to prevent, and that no mock
    /// may stand in for a module. The registration supplies
    /// CLASH_QT_BACKEND_MODULE and CLASH_QT_FAKE_MODULE and depends on both
    /// artifacts, so an empty reason here is the normal case and a non-empty
    /// one is a broken build or a broken loader, not an absent platform.
    void runShared(void (*scenario)(shared::BackendDriver &)) {
        QFETCH(int, subject);
        std::unique_ptr<shared::BackendDriver> driver =
            shared::makeDriver(static_cast<shared::Subject>(subject), *environment_);
        QVERIFY(driver != nullptr);
        const QString unavailable = driver->unavailableReason();
        QVERIFY2(unavailable.isEmpty(),
                 qPrintable(QStringLiteral("required subject '%1' is unavailable: %2")
                                .arg(driver->name(), unavailable)));
        QVERIFY2(driver->prepare(), qPrintable(driver->errorString()));
        scenario(*driver);
        // Teardown runs even when the scenario failed: a module must be closed
        // before it is unmapped, and a managed child must not outlive the row.
        driver->teardown();
    }

    QString writeConfig(quint16 port, const QString &name = QStringLiteral("config.yaml")) {
        const QString path = environment_->filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        file.write("external-controller: 127.0.0.1:" + QByteArray::number(port) + "\n");
        file.close();
        return path;
    }

    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(BackendRealContractTest)
#include "backend_real_contract_test.moc"
