// Contract suite for the MihomoBackend facade.
// Specification: .refactor/BACKEND_CONTRACT.md revision backend-r1, section 10.
//
// Every case here is one the contract names, and every case was validated by
// temporarily inverting the behaviour it protects and confirming this suite
// fails. The inversions are listed in the worker report, not re-run here - with
// one exception: the generation/abort ordering is a knob on the fake, because
// the contract requires the suite itself to tell the two orders apart.
//
// The suite runs against the fake backend. A passing fake proves the contract
// is implementable and that a consumer written to it is correct; it proves
// nothing about the engine. MOD-CORE runs this same suite against the real
// backend.
//
// Nothing here sleeps. Time is advanced explicitly, every asynchronous step is
// released explicitly, and a deadline only bounds a failure.

#include <QtTest>

#include <type_traits>

#include "support/backend/fake_backend.h"
#include "support/backend/recording_observer.h"

using core::backend::BackendTimings;
using core::backend::Completion;
using core::backend::CompletionStatus;
using core::backend::CoreState;
using core::backend::Endpoint;
using core::backend::ErrorCode;
using core::backend::ExecutionMode;
using core::backend::Feature;
using core::backend::Generation;
using core::backend::MihomoBackend;
using core::backend::Ownership;
using core::backend::RequestId;

using testsupport::backend::AbortOrdering;
using testsupport::backend::FakeBackend;
using testsupport::backend::Gate;
using testsupport::backend::RecordingObserver;
using testsupport::backend::RequestKind;
using testsupport::backend::RequestOutcome;
using testsupport::backend::VersionResponse;

namespace {

constexpr BackendTimings kTimings{};

Endpoint endpointAt(const char *host, quint16 port, const char *secret = "") {
    Endpoint endpoint;
    endpoint.host = QString::fromLatin1(host);
    endpoint.port = port;
    endpoint.secret = QString::fromLatin1(secret);
    return endpoint;
}

// Brings a managed core from Stopped to Running, releasing each gate by hand.
bool bringUpCore(FakeBackend &fake, const QString &configPath, const Endpoint &endpoint) {
    fake.mapConfigFile(configPath, endpoint);
    fake.start(configPath, QStringLiteral("/work"));
    if (!fake.completeValidation(true)) return false;
    fake.advanceTime(kTimings.probeIntervalMs);
    fake.flushEvents();
    return fake.state() == CoreState::Running;
}

RequestId pendingOfKind(const FakeBackend &fake, RequestKind kind) {
    for (RequestId id : fake.pendingRequests())
        if (fake.pendingKind(id) == kind) return id;
    return RequestId::Invalid;
}

}  // namespace

class BackendContractTest : public QObject {
    Q_OBJECT

  private slots:

    // ------------------------------------------------- ABI-readiness (sec. 9)

    void publishedShapesAreAbiReady() {
        static_assert(std::is_same_v<std::underlying_type_t<CoreState>, std::uint8_t>);
        static_assert(std::is_same_v<std::underlying_type_t<Ownership>, std::uint8_t>);
        static_assert(std::is_same_v<std::underlying_type_t<ExecutionMode>, std::uint8_t>);
        static_assert(std::is_same_v<std::underlying_type_t<CompletionStatus>, std::uint8_t>);
        static_assert(std::is_same_v<std::underlying_type_t<ErrorCode>, std::int32_t>);
        static_assert(std::is_same_v<std::underlying_type_t<Feature>, std::uint32_t>);
        static_assert(std::is_same_v<std::underlying_type_t<RequestId>, std::uint64_t>);
        static_assert(std::is_same_v<std::underlying_type_t<Generation>, std::uint64_t>);
        // Endpoint is data only and is returned by value, never by reference.
        static_assert(std::is_standard_layout_v<Endpoint>);
        static_assert(std::is_same_v<decltype(std::declval<MihomoBackend &>().managedEndpoint()),
                                     Endpoint>);
        static_assert(std::is_same_v<decltype(std::declval<MihomoBackend &>().currentEndpoint()),
                                     Endpoint>);
        // An unknown state from a newer module is a value this enum can hold,
        // not undefined behaviour.
        const auto future = static_cast<CoreState>(40);
        QVERIFY(!core::backend::isKnownCoreState(future));
        QVERIFY(core::backend::isKnownCoreState(CoreState::Failed));
        // The contract's own timings, not a consumer's guess at a timeout.
        FakeBackend fake;
        QCOMPARE(fake.timings().idleDeadlineMs, 10000u);
        QCOMPARE(fake.timings().serviceIdleDeadlineMs, 60000u);
        QCOMPARE(fake.timings().hardCapMs, 180000u);
        QCOMPARE(fake.timings().probeIntervalMs, 200u);
        QCOMPARE(fake.timings().probeTimeoutMs, 2000u);
        QCOMPARE(fake.timings().terminateWaitMs, 3000u);
    }

    // --- 1. detach leaves an attached controller running; stop terminates only
    //        a managed core

    void detachLeavesTheControllerRunningAndStopTerminatesOnlyTheManagedCore() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        QVERIFY(fake.addObserver(&observer));

        const Endpoint external = endpointAt("127.0.0.1", 9091);
        fake.addExternalController(external);
        fake.attach(external);
        QVERIFY(fake.flushEvents());
        QVERIFY(fake.isAttached());
        QCOMPARE(fake.attachmentOwnership(), Ownership::Attached);
        QVERIFY(fake.isControllerRunning(external));

        fake.detach();
        QVERIFY(fake.flushEvents());
        QVERIFY(!fake.isAttached());
        // The whole point: detaching is not a kill switch.
        QVERIFY(fake.isControllerRunning(external));
        QCOMPARE(fake.attachmentOwnership(), Ownership::None);

        const Endpoint managed = endpointAt("127.0.0.1", 9090);
        QVERIFY(bringUpCore(fake, QStringLiteral("/cfg-a.yaml"), managed));
        QVERIFY(fake.isControllerRunning(managed));
        QCOMPARE(fake.ownership(), Ownership::Managed);

        // Attached to the external controller while our own core runs: the two
        // are independently observable.
        fake.attach(external);
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.attachmentOwnership(), Ownership::Attached);
        QCOMPARE(fake.state(), CoreState::Running);

        fake.stop();
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Stopped);
        QVERIFY(!fake.isControllerRunning(managed));  // stop terminated ours
        QVERIFY(fake.isControllerRunning(external));  // and only ours
        QVERIFY(fake.removeObserver(&observer));
    }

    void ownershipIsReportedOnEveryStateEvent() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        QVERIFY(bringUpCore(fake, QStringLiteral("/cfg-a.yaml"), endpointAt("127.0.0.1", 9090)));
        QVERIFY(!observer.states.empty());
        for (const auto &event : observer.states) {
            if (event.state == CoreState::Starting || event.state == CoreState::Running)
                QCOMPARE(event.ownership, Ownership::Managed);
        }
        // "external core connected" without comparing endpoints by hand.
        const Endpoint external = endpointAt("127.0.0.1", 9099);
        fake.addExternalController(external);
        fake.stop();
        fake.releaseChildExit();
        fake.attach(external);
        fake.releaseAllRequests();
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Stopped);
        QVERIFY(fake.isConnected());
        QVERIFY(fake.isExternalControllerConnected());
        fake.removeObserver(&observer);
    }

    // --- 2. a completion stamped with a superseded generation is rejected

    void aCompletionFromASupersededGenerationIsRejected() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        fake.setRequestGate(Gate::Held);

        fake.attach(endpointAt("127.0.0.1", 9090));
        QVERIFY(fake.flushEvents());
        const RequestId version = pendingOfKind(fake, RequestKind::RefreshVersion);
        QVERIFY(version != RequestId::Invalid);
        const Generation submitted = fake.submittedGeneration(version);

        // A managed start is an invalidating event: it bumps the generation.
        fake.mapConfigFile(QStringLiteral("/cfg.yaml"), endpointAt("127.0.0.1", 9090));
        fake.start(QStringLiteral("/cfg.yaml"), QStringLiteral("/work"));
        QVERIFY(fake.completeValidation(true));
        QVERIFY(fake.flushEvents());
        QVERIFY(fake.generation() > submitted);
        QVERIFY(observer.lastObserved() > submitted);

        fake.staged().version = QStringLiteral("stale-9.9.9");
        QVERIFY(fake.releaseRequest(version, RequestOutcome::Success));
        QVERIFY(fake.flushEvents());

        QCOMPARE(observer.acceptedCount(QStringLiteral("version")), 0);
        QCOMPARE(static_cast<int>(observer.rejectedByGeneration.size()), 1);
        QCOMPARE(observer.rejectedByGeneration.front().completion.request, version);
        QCOMPARE(observer.rejectedByGeneration.front().completion.generation, submitted);
        fake.removeObserver(&observer);
    }

    // --- 3. generation is bumped BEFORE in-flight work is aborted

    void theGenerationIsBumpedBeforeInFlightWorkIsAborted_data() {
        QTest::addColumn<int>("ordering");
        QTest::addColumn<bool>("expectLiveDataFromTheOldGeneration");
        QTest::newRow("bump-then-abort (the contract)")
            << static_cast<int>(AbortOrdering::BumpThenAbort) << false;
        QTest::newRow("abort-then-bump (the hazard)")
            << static_cast<int>(AbortOrdering::AbortThenBump) << true;
    }

    void theGenerationIsBumpedBeforeInFlightWorkIsAborted() {
        QFETCH(int, ordering);
        QFETCH(bool, expectLiveDataFromTheOldGeneration);

        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        fake.setAbortOrdering(static_cast<AbortOrdering>(ordering));
        fake.setRequestGate(Gate::Held);

        fake.attach(endpointAt("127.0.0.1", 9090, "a"));
        QVERIFY(fake.flushEvents());
        const RequestId version = pendingOfKind(fake, RequestKind::RefreshVersion);
        QVERIFY(version != RequestId::Invalid);
        fake.staged().version = QStringLiteral("from-the-old-endpoint");

        // The endpoint changes. The in-flight replies of the previous endpoint
        // are aborted, and finished() can run synchronously inside that abort.
        fake.attach(endpointAt("127.0.0.1", 9091, "b"));
        QVERIFY(fake.flushEvents());
        QVERIFY(!fake.isPending(version));

        const bool live = observer.acceptedCount(QStringLiteral("version")) > 0;
        if (expectLiveDataFromTheOldGeneration) {
            // Proof that the fake can exhibit the hazard: without this the
            // assertion below would pass against a backend that never aborts.
            QVERIFY2(live,
                     "abort-then-bump must deliver the superseded reply as live data, "
                     "otherwise the ordering assertion proves nothing");
            bool carriedTheOldPayload = false;
            for (const auto &event : observer.accepted) {
                if (event.channel == QStringLiteral("version") &&
                    event.payload == QStringLiteral("from-the-old-endpoint"))
                    carriedTheOldPayload = true;
            }
            QVERIFY(carriedTheOldPayload);
        } else {
            QVERIFY2(!live,
                     "a completion aborted by an endpoint change was delivered as live data: "
                     "the generation was bumped after the abort, not before");
            // attach() re-reads version, proxies, rules and config, so four
            // requests were in flight and all four must be abandoned.
            QCOMPARE(static_cast<int>(observer.supersededByBackend.size()), 4);
            bool sawTheVersionRequest = false;
            for (const auto &event : observer.supersededByBackend) {
                QCOMPARE(event.completion.status, CompletionStatus::Superseded);
                if (event.completion.request == version) sawTheVersionRequest = true;
            }
            QVERIFY(sawTheVersionRequest);
        }
        fake.removeObserver(&observer);
    }

    // --- 4. readiness requires 200 + a string `version`, not process start

    void readinessRequiresAVersionStringNotAStartedProcess_data() {
        QTest::addColumn<int>("response");
        QTest::addColumn<bool>("ready");
        QTest::newRow("200 + string version") << static_cast<int>(VersionResponse::Ok) << true;
        QTest::newRow("no answer") << static_cast<int>(VersionResponse::Unreachable) << false;
        QTest::newRow("500") << static_cast<int>(VersionResponse::ServerError) << false;
        QTest::newRow("404") << static_cast<int>(VersionResponse::NotFound) << false;
        QTest::newRow("200, no version field")
            << static_cast<int>(VersionResponse::VersionMissing) << false;
        QTest::newRow("200, version is a number")
            << static_cast<int>(VersionResponse::VersionNotString) << false;
        QTest::newRow("200, body is not an object")
            << static_cast<int>(VersionResponse::NotAnObject) << false;
    }

    void readinessRequiresAVersionStringNotAStartedProcess() {
        QFETCH(int, response);
        QFETCH(bool, ready);

        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        fake.setVersionResponse(static_cast<VersionResponse>(response));
        const Endpoint endpoint = endpointAt("127.0.0.1", 9090);
        fake.mapConfigFile(QStringLiteral("/cfg.yaml"), endpoint);

        fake.start(QStringLiteral("/cfg.yaml"), QStringLiteral("/work"));
        QVERIFY(fake.completeValidation(true));
        QVERIFY(fake.flushEvents());
        // The process is up. That is NOT readiness.
        QCOMPARE(fake.state(), CoreState::Starting);
        QVERIFY(fake.isChildRunning());
        QVERIFY(observer.readyEndpoints.empty());

        fake.advanceTime(5 * kTimings.probeIntervalMs);
        QVERIFY(fake.flushEvents());

        if (ready) {
            QCOMPARE(fake.state(), CoreState::Running);
            QCOMPARE(static_cast<int>(observer.readyEndpoints.size()), 1);
            QCOMPARE(observer.readyEndpoints.front().port, endpoint.port);
            QVERIFY(core::backend::isValid(fake.managedEndpoint()));
        } else {
            QCOMPARE(fake.state(), CoreState::Starting);
            QVERIFY(observer.readyEndpoints.empty());
            QVERIFY(!core::backend::isValid(fake.managedEndpoint()));
        }
        fake.removeObserver(&observer);
    }

    // --- 5. the idle deadline is refreshed by log output; the hard cap is not

    void logOutputRefreshesTheIdleDeadlineButNotTheHardCap() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        fake.setVersionResponse(VersionResponse::Unreachable);
        fake.mapConfigFile(QStringLiteral("/cfg.yaml"), endpointAt("127.0.0.1", 9090));
        fake.start(QStringLiteral("/cfg.yaml"), QStringLiteral("/work"));
        QVERIFY(fake.completeValidation(true));
        QCOMPARE(fake.state(), CoreState::Starting);

        // Just inside the idle deadline.
        fake.advanceTime(kTimings.idleDeadlineMs - 1000);
        QCOMPARE(fake.state(), CoreState::Starting);
        // One log line, and the whole deadline is available again.
        fake.emitCoreLogLine(QStringLiteral("[INFO] still downloading geodata"));
        fake.advanceTime(kTimings.idleDeadlineMs - 1000);
        QVERIFY2(fake.state() == CoreState::Starting,
                 "a core that keeps logging keeps its deadline alive");
        QVERIFY(fake.elapsedMs() > kTimings.idleDeadlineMs);

        // Keep logging, indefinitely. The hard cap is absolute.
        while (fake.state() == CoreState::Starting && fake.elapsedMs() < 4 * kTimings.hardCapMs) {
            fake.emitCoreLogLine(QStringLiteral("[INFO] still working"));
            fake.advanceTime(kTimings.idleDeadlineMs / 2);
        }
        // The readiness deadline expired; the child it was waiting on still has
        // to be reaped before the failure is terminal.
        QCOMPARE(fake.state(), CoreState::Stopping);
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Failed);
        QVERIFY2(fake.elapsedMs() >= kTimings.hardCapMs,
                 "the core failed before the hard cap: the idle deadline was not refreshed");
        QVERIFY2(fake.elapsedMs() < kTimings.hardCapMs + kTimings.idleDeadlineMs,
                 "the core outlived the hard cap: log output refreshed an absolute deadline");
        QCOMPARE(static_cast<int>(observer.failures.size()), 1);
        QCOMPARE(observer.failures.front().error.code, ErrorCode::ReadyTimeout);
        fake.removeObserver(&observer);
    }

    // --- 6. validation failure leaves the running configuration intact

    void aValidationFailureLeavesTheRunningConfigurationIntact() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        const Endpoint running = endpointAt("127.0.0.1", 9090);
        QVERIFY(bringUpCore(fake, QStringLiteral("/cfg-a.yaml"), running));
        const QStringList before = fake.activeConfigPaths();
        QCOMPARE(before, QStringList{QStringLiteral("/cfg-a.yaml")});

        fake.start(QStringLiteral("/cfg-b.yaml"), QStringLiteral("/work"));
        // The old core stays live while its replacement is validated.
        QCOMPARE(fake.state(), CoreState::Running);
        QVERIFY(fake.isChildRunning());
        QVERIFY(fake.activeConfigPaths().contains(QStringLiteral("/cfg-a.yaml")));
        QVERIFY(fake.activeConfigPaths().contains(QStringLiteral("/cfg-b.yaml")));

        QVERIFY(fake.completeValidation(false, QStringLiteral("unknown field: tun.stakc")));
        QVERIFY(fake.flushEvents());

        QCOMPARE(fake.state(), CoreState::Running);
        QVERIFY(fake.isChildRunning());
        QVERIFY(fake.isControllerRunning(running));
        QCOMPARE(fake.managedEndpoint().port, running.port);
        QCOMPARE(fake.activeConfigPaths(), before);
        QVERIFY(!fake.isRestartPending());
        QCOMPARE(static_cast<int>(observer.failures.size()), 1);
        QCOMPARE(observer.failures.front().error.code, ErrorCode::ValidationFailed);
        fake.removeObserver(&observer);
    }

    // --- 7. a restart consumes its pending launch only after the prior exit

    void aPendingLaunchIsConsumedOnlyAfterThePriorExitIsObserved() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        QVERIFY(bringUpCore(fake, QStringLiteral("/cfg-a.yaml"), endpointAt("127.0.0.1", 9090)));
        fake.mapConfigFile(QStringLiteral("/cfg-b.yaml"), endpointAt("127.0.0.1", 9095));

        fake.start(QStringLiteral("/cfg-b.yaml"), QStringLiteral("/work"));
        QVERIFY(fake.completeValidation(true));
        QVERIFY(fake.flushEvents());

        QCOMPARE(fake.state(), CoreState::Stopping);
        QVERIFY(fake.isChildTerminating());
        QVERIFY2(fake.isRestartPending(), "the launch is held, not applied");
        QVERIFY2(!fake.isChildRunning(), "the replacement was launched before the prior exit");
        QVERIFY(fake.activeConfigPaths().contains(QStringLiteral("/cfg-b.yaml")));
        // The reload gate the application applies today.
        QVERIFY(fake.isManagedCoreActive());

        // Termination escalates to a kill, and even that does not consume it.
        fake.advanceTime(kTimings.terminateWaitMs);
        QVERIFY(fake.wasChildKilled());
        QVERIFY(!fake.isChildRunning());
        QVERIFY(fake.isRestartPending());

        // Only the observed exit consumes the pending launch.
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Starting);
        QVERIFY(fake.isChildRunning());
        QVERIFY(!fake.isRestartPending());
        QCOMPARE(fake.activeConfigPaths(), QStringList{QStringLiteral("/cfg-b.yaml")});
        fake.removeObserver(&observer);
    }

    // --- 8. a cancelled validation keeps its configuration path active

    void aCancelledValidationKeepsItsConfigurationPathActiveUntilItExits() {
        FakeBackend fake;
        fake.start(QStringLiteral("/cfg-a.yaml"), QStringLiteral("/work"));
        QVERIFY(fake.isValidating());
        QVERIFY(fake.activeConfigPaths().contains(QStringLiteral("/cfg-a.yaml")));

        // A second launch cancels the first validator. Its child is dying, not
        // dead, and its snapshot must not be deleted out from under it.
        fake.start(QStringLiteral("/cfg-b.yaml"), QStringLiteral("/work"));
        QCOMPARE(fake.validatingConfigPath(), QStringLiteral("/cfg-b.yaml"));
        QVERIFY(fake.cancellingValidationPaths().contains(QStringLiteral("/cfg-a.yaml")));
        QVERIFY2(fake.activeConfigPaths().contains(QStringLiteral("/cfg-a.yaml")),
                 "a cancelled validator's config left the active set before it exited");
        QVERIFY(fake.activeConfigPaths().contains(QStringLiteral("/cfg-b.yaml")));

        QVERIFY(fake.completeCancelledValidation(QStringLiteral("/cfg-a.yaml")));
        QVERIFY(!fake.activeConfigPaths().contains(QStringLiteral("/cfg-a.yaml")));
        QVERIFY(fake.activeConfigPaths().contains(QStringLiteral("/cfg-b.yaml")));
        QVERIFY(fake.flushEvents());
    }

    // --- 9. confirmed == false is reported as such and is not success

    void anUnconfirmedStopIsReportedAsUnconfirmed() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        fake.setServiceSupported(true);
        QVERIFY(fake.setExecutionMode(ExecutionMode::PrivilegedService));
        QVERIFY(bringUpCore(fake, QStringLiteral("/cfg.yaml"), endpointAt("127.0.0.1", 9090)));
        QVERIFY(fake.usesPrivilegedService());

        const RequestId stopRequest = fake.stop();
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Stopping);
        QVERIFY2(observer.stops.empty(), "stop answered before anything confirmed the exit");

        // The service disconnects before confirming the child exited.
        QVERIFY(fake.disconnectPrivilegedService());
        QVERIFY(fake.flushEvents());

        QCOMPARE(static_cast<int>(observer.stops.size()), 1);
        const auto &result = observer.stops.front();
        QCOMPARE(result.request, stopRequest);
        QVERIFY2(!result.confirmed, "an unconfirmed stop was reported as confirmed");
        QVERIFY(result.reason.isFailure());
        QCOMPARE(result.reason.code, ErrorCode::ServiceDisconnected);
        QVERIFY(!result.reason.message.isEmpty());
        QCOMPARE(fake.state(), CoreState::Failed);
        QCOMPARE(observer.stoppedCount, 0);
        fake.removeObserver(&observer);
    }

    void aConfirmedStopIsReportedAsConfirmed() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        QVERIFY(bringUpCore(fake, QStringLiteral("/cfg.yaml"), endpointAt("127.0.0.1", 9090)));
        const RequestId stopRequest = fake.stop();
        QVERIFY(fake.flushEvents());
        QVERIFY2(observer.stops.empty(), "stop answered before the child's exit was observed");
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        QCOMPARE(static_cast<int>(observer.stops.size()), 1);
        QCOMPARE(observer.stops.front().request, stopRequest);
        QVERIFY(observer.stops.front().confirmed);
        QVERIFY(!observer.stops.front().reason.isFailure());
        QCOMPARE(observer.stoppedCount, 1);
        QCOMPARE(fake.state(), CoreState::Stopped);
        fake.removeObserver(&observer);
    }

    // --- 10. no observer is invoked re-entrantly from within a mutating call

    void noObserverIsInvokedFromInsideAMutatingCall() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);

        // Every mutating path in the surface, including the one that emits five
        // events from inside a single call today (clearLiveState).
        fake.attach(endpointAt("127.0.0.1", 9090));
        QVERIFY(!observer.sawReentrantDelivery);
        QVERIFY(fake.flushEvents());
        fake.setRequestGate(Gate::Held);
        const RequestId mode = fake.setMode(QStringLiteral("global"));
        fake.releaseRequest(mode, RequestOutcome::Success);
        fake.setRequestGate(Gate::Immediate);
        fake.attach(endpointAt("127.0.0.1", 9091));  // clears live state
        fake.detach();
        fake.mapConfigFile(QStringLiteral("/cfg.yaml"), endpointAt("127.0.0.1", 9090));
        fake.start(QStringLiteral("/cfg.yaml"), QStringLiteral("/work"));
        fake.completeValidation(true);
        fake.emitCoreLogLine(QStringLiteral("[INFO] hello"));
        fake.advanceTime(kTimings.probeIntervalMs);
        fake.disconnectController();
        fake.stop();
        fake.releaseChildExit();
        QVERIFY(fake.flushEvents());

        QVERIFY2(!observer.sawReentrantDelivery,
                 "an observer was invoked from inside a mutating call");
        QVERIFY2(observer.callbackCount > 20,
                 "the observer received almost nothing: the assertion above is vacuous");
        QVERIFY2(observer.liveStateClears >= 2, "live state was never cleared");
        fake.removeObserver(&observer);
    }

    void aMutationFromInsideACallbackIsAlsoDeliveredLater() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        // Re-entering the backend from a callback must not produce a nested
        // delivery either.
        observer.onNextEvent = [&fake] { fake.refreshVersion(); };
        fake.attach(endpointAt("127.0.0.1", 9090));
        QVERIFY(fake.flushEvents());
        QVERIFY(!observer.sawReentrantDelivery);
        QVERIFY(observer.acceptedCount(QStringLiteral("version")) > 0);
        fake.removeObserver(&observer);
    }

    // --- 11. removing an observer during delivery is safe

    void removingAnObserverDuringDeliveryIsSafe() {
        FakeBackend fake;
        RecordingObserver first(&fake, QStringLiteral("first"));
        RecordingObserver second(&fake, QStringLiteral("second"));
        RecordingObserver third(&fake, QStringLiteral("third"));
        QVERIFY(fake.addObserver(&first));
        QVERIFY(fake.addObserver(&second));
        QVERIFY(fake.addObserver(&third));
        QVERIFY(!fake.addObserver(&first));
        QVERIFY(!fake.addObserver(nullptr));

        // The second observer removes itself and the one behind it, from inside
        // the very first callback it receives.
        second.onNextEvent = [&] {
            fake.removeObserver(&second);
            fake.removeObserver(&third);
        };

        fake.attach(endpointAt("127.0.0.1", 9090));
        QVERIFY(fake.flushEvents());

        QVERIFY2(first.callbackCount > 3, "the surviving observer stopped receiving events");
        QCOMPARE(second.callbackCount, 1);
        QVERIFY2(third.callbackCount == 0,
                 "an observer removed during delivery was invoked afterwards");
        QVERIFY(!fake.removeObserver(&second));
        QVERIFY(fake.removeObserver(&first));
    }

    void anObserverAddedDuringDeliverySeesOnlyLaterEvents() {
        FakeBackend fake;
        RecordingObserver first(&fake);
        RecordingObserver late(&fake);
        fake.addObserver(&first);
        first.onNextEvent = [&] { fake.addObserver(&late); };
        fake.attach(endpointAt("127.0.0.1", 9090));
        QVERIFY(fake.flushEvents());
        QVERIFY(first.callbackCount > late.callbackCount);
        QVERIFY(!late.sawReentrantDelivery);
        fake.removeObserver(&first);
        fake.removeObserver(&late);
    }

    // ------------------------------------------- supporting contract semantics

    void aCancelledProbeDeliversNothing() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        fake.setProbeGate(Gate::Held);
        fake.mapConfigFile(QStringLiteral("/cfg.yaml"), endpointAt("127.0.0.1", 9090));
        fake.start(QStringLiteral("/cfg.yaml"), QStringLiteral("/work"));
        QVERIFY(fake.completeValidation(true));
        fake.advanceTime(kTimings.probeIntervalMs);
        QVERIFY2(fake.isProbeInFlight(), "no probe was issued");

        fake.stop();  // cancels the probe: disconnect before abort
        QVERIFY(fake.flushEvents());
        QVERIFY2(!fake.releaseProbeAnswer(),
                 "a cancelled probe still had a live answer to deliver");
        QVERIFY(fake.flushEvents());
        QVERIFY(observer.readyEndpoints.empty());
        QVERIFY(fake.state() != CoreState::Running);
        fake.removeObserver(&observer);
    }

    void theTunChangeReportsAReadBackNotAnEcho() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        fake.setRequestGate(Gate::Held);
        const RequestId change = fake.setTunEnabled(true);
        QVERIFY(change != RequestId::Invalid);
        QVERIFY(fake.isTunChangePending());
        // Further calls are ignored while one is pending.
        QCOMPARE(fake.setTunEnabled(false), RequestId::Invalid);

        // The controller did not actually enable it.
        fake.staged().tunActual = false;
        QVERIFY(fake.releaseRequest(change, RequestOutcome::Success));
        QVERIFY(fake.flushEvents());
        QCOMPARE(static_cast<int>(observer.tunChanges.size()), 1);
        QCOMPARE(observer.tunChanges.front().request, change);
        QVERIFY(observer.tunChanges.front().requested);
        QVERIFY2(!observer.tunChanges.front().actual,
                 "the completion echoed the request instead of reading the state back");
        QVERIFY(!fake.isTunChangePending());
        fake.removeObserver(&observer);
    }

    // Evidence for the contract's first open question: ProviderClient's pending
    // key set is a COALESCING key, and a per-submission RequestId cannot express
    // it - two identical submissions must produce one request, not two ids.
    void anIdenticalProviderRequestIsCoalescedOntoTheOutstandingOne() {
        FakeBackend fake;
        RecordingObserver observer(&fake);
        fake.addObserver(&observer);
        fake.setRequestGate(Gate::Held);

        const RequestId first = fake.fetchProviders(true);
        const RequestId again = fake.fetchProviders(true);
        const RequestId other = fake.fetchProviders(false);
        QCOMPARE(again, first);
        QVERIFY(other != first);
        QCOMPARE(static_cast<int>(fake.pendingRequests().size()), 2);
        QVERIFY(fake.isProviderBusy());

        fake.releaseAllRequests();
        QVERIFY(fake.flushEvents());
        QCOMPARE(observer.acceptedCount(QStringLiteral("providers/rules")), 1);
        QCOMPARE(observer.acceptedCount(QStringLiteral("providers/proxies")), 1);
        QVERIFY(!fake.isProviderBusy());
        fake.removeObserver(&observer);
    }

    void executionModeIsRefusedWhileTheCoreIsLive() {
        FakeBackend fake;
        QVERIFY(bringUpCore(fake, QStringLiteral("/cfg.yaml"), endpointAt("127.0.0.1", 9090)));
        QVERIFY2(!fake.setExecutionMode(ExecutionMode::PrivilegedService),
                 "the execution mode changed under a running core");
        QCOMPARE(fake.executionMode(), ExecutionMode::Managed);
        fake.stop();
        fake.releaseChildExit();
        QVERIFY(fake.flushEvents());
        QVERIFY(fake.setExecutionMode(ExecutionMode::PrivilegedService));
    }

    void capabilitiesAreQueriedFromTheInstance() {
        FakeBackend fake;
        // No process-global statics: two instances answer independently.
        FakeBackend other;
        other.setServiceSupported(false);
        QVERIFY(fake.serviceSupported());
        QVERIFY(!other.serviceSupported());
        QVERIFY(!other.serviceAvailable());
        QVERIFY(fake.features().has(Feature::ConfirmedTunChange));
        QVERIFY(fake.features().has(Feature::ManagedLifecycle));
        QVERIFY(!other.features().has(Feature::PrivilegedService));
        QCOMPARE(fake.identity().interfaceRevision, 1u);
    }
};

QTEST_GUILESS_MAIN(BackendContractTest)
#include "backend_contract_test.moc"
