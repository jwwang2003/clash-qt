// RoutingController: proxy restoration during normal operation, and one owner
// for routing intent.
//
// Each case pins one proxy-restoration behaviour that used to live inline in
// src/main.cpp, plus the requirement that a single controller owns routing
// intent and publishes only confirmed state. Each case names what it protects.
//
// The system proxy is a real platform::SystemProxyService with its Operation
// substituted through the constructor, so no OS proxy call and no preference
// write happens. The backend is the deterministic fake. Nothing sleeps: the OS
// operation is held on a semaphore and released, and the compressed grace
// window is driven by a marker timer, not by waiting on the clock.

#include <QtTest>

#include <QCoreApplication>

#include <atomic>
#include <memory>

#include <QMutex>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QVector>

#include "app/runtime/routing_controller.h"
#include "platform/proxy/system_proxy_service.h"
#include "support/backend/fake_backend.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

using app::runtime::RestoreDelays;
using app::runtime::RoutingController;

using core::backend::CoreState;
using core::backend::Endpoint;

using testsupport::backend::FakeBackend;
using testsupport::backend::Gate;
using testsupport::backend::RequestKind;
using testsupport::backend::RequestOutcome;

namespace {

constexpr int kDeadlineMs = 3000;
// The compressed grace window. Only the 5 s default's VALUE is asserted; its
// behaviour - the re-validation - is what this number lets us drive.
constexpr int kCompressedGraceMs = 30;

Endpoint endpointAt(quint16 port) {
    Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = port;
    return endpoint;
}

// What the OS was asked to do, recorded from the worker thread.
class ProxyOperations {
  public:
    QVector<platform::SystemProxyAction> actions() const {
        QMutexLocker locker(&mutex_);
        return actions_;
    }
    int count(platform::SystemProxyAction action) const {
        QMutexLocker locker(&mutex_);
        return static_cast<int>(std::count(actions_.begin(), actions_.end(), action));
    }
    void record(platform::SystemProxyAction action) {
        QMutexLocker locker(&mutex_);
        actions_.append(action);
    }

    std::atomic_bool restoreSucceeds{true};
    QSemaphore hold;               // 0 tickets and holdEnabled -> the worker blocks
    std::atomic_bool holdEnabled{false};

  private:
    mutable QMutex mutex_;
    QVector<platform::SystemProxyAction> actions_;
};

using Action = platform::SystemProxyAction;
using Config = platform::ProxyConfig;
using Result = platform::SystemProxyResult;

// A service whose OS calls are this in-process function. `owned` mirrors what
// the real backend reports after a confirmed enable.
std::unique_ptr<platform::SystemProxyService> makeService(
    const std::shared_ptr<ProxyOperations> &log, const std::shared_ptr<Config> &osState) {
    return std::make_unique<platform::SystemProxyService>(
        nullptr, [log, osState](Action action, const Config &config) {
            log->record(action);
            if (log->holdEnabled.load()) log->hold.tryAcquire(1, kDeadlineMs);
            Result result;
            result.state.supported = true;
            result.state.valid = true;
            switch (action) {
                case Action::Enable:
                    *osState = config;
                    result.state.owned = true;
                    break;
                case Action::Disable:
                    *osState = Config{};
                    break;
                case Action::Restore:
                    result.success = log->restoreSucceeds.load();
                    if (!result.success) {
                        result.error = QStringLiteral("the helper refused");
                        result.state.valid = true;
                        result.state.config = *osState;
                        return result;
                    }
                    *osState = Config{};
                    break;
                case Action::Refresh:
                    break;
            }
            result.state.config = *osState;
            return result;
        });
}

// Runs the event loop until every already-posted zero-delay timer has fired.
bool drainPostedTimers() {
    bool marked = false;
    QTimer::singleShot(0, QCoreApplication::instance(), [&marked] { QTimer::singleShot(0, QCoreApplication::instance(), [&marked] { marked = true; }); });
    return FakeBackend::waitFor([&marked] { return marked; }, kDeadlineMs);
}

// Runs the event loop past `ms`, then past every timer that expired with it.
bool drainPast(int ms) {
    bool marked = false;
    QTimer::singleShot(ms, QCoreApplication::instance(), [&marked] { QTimer::singleShot(0, QCoreApplication::instance(), [&marked] { marked = true; }); });
    return FakeBackend::waitFor([&marked] { return marked; }, kDeadlineMs);
}

bool bringUpCore(FakeBackend &fake, const QString &configPath, const Endpoint &endpoint) {
    fake.mapConfigFile(configPath, endpoint);
    fake.start(configPath, QStringLiteral("/work"));
    if (!fake.completeValidation(true)) return false;
    fake.advanceTime(fake.timings().probeIntervalMs);
    fake.flushEvents();
    return fake.state() == CoreState::Running;
}

// A backend observer that runs a callback on a state change. Registered AFTER
// the controller, so it runs inside the same delivery pass and can change the
// world before the controller's deferred re-validation fires.
class StateHook final : public core::backend::BackendObserver {
  public:
    std::function<void(CoreState)> onState;
    std::function<void(bool)> onConnected;
    void coreStateChanged(core::backend::Generation, CoreState state,
                          core::backend::Ownership) noexcept override {
        if (onState) onState(state);
    }
    void connectedChanged(core::backend::Generation, bool connected) noexcept override {
        if (onConnected) onConnected(connected);
    }
};

}  // namespace

class RoutingControllerTest : public QObject {
    Q_OBJECT

  private slots:

    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("modrc"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }

    void cleanupTestCase() {
        const QString escape = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escape.isEmpty(), qPrintable(escape));
        environment_.reset();
    }

    // --- group D, point 2. The shipped delays, asserted as values so a test
    //     that compresses them cannot quietly become the specification.

    void theShippedRestoreDelaysAreZeroAndFiveSeconds() {
        QCOMPARE(RoutingController::kCoreStoppedRestoreDelayMs, 0);
        QCOMPARE(RoutingController::kDisconnectedRestoreDelayMs, 5000);

        const RestoreDelays defaults;
        QCOMPARE(defaults.coreStoppedMs, RoutingController::kCoreStoppedRestoreDelayMs);
        QCOMPARE(defaults.disconnectedMs, RoutingController::kDisconnectedRestoreDelayMs);

        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get());
        QCOMPARE(controller.delays().coreStoppedMs, 0);
        QCOMPARE(controller.delays().disconnectedMs, 5000);
    }

    // --- group D, point 1. restoreOwned() only; never a blanket disable.

    void aStoppedCoreRestoresOnlyTheProxyTheApplicationOwns() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get());

        QVERIFY(bringUpCore(fake, QStringLiteral("/tmp/.runtime-a.yaml"), endpointAt(9090)));
        QCOMPARE(controller.restoreCount(), 0);

        fake.stop();
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        QVERIFY(drainPostedTimers());

        QCOMPARE(controller.restoreCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(log->count(Action::Restore), 1, kDeadlineMs);
        QCOMPARE(log->count(Action::Disable), 0);
        QCOMPARE(log->count(Action::Enable), 0);
    }

    // --- group D, point 2, the 0 ms hop. The DELAY is not the behaviour; the
    //     RE-VALIDATION after it is. A restart that has already re-entered
    //     Starting must keep its proxy.

    void aCoreThatRestartsBeforeTheHopFiresKeepsItsProxy() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get());

        const QString first = QStringLiteral("/tmp/.runtime-a.yaml");
        const QString second = QStringLiteral("/tmp/.runtime-b.yaml");
        fake.mapConfigFile(second, endpointAt(9191));
        QVERIFY(bringUpCore(fake, first, endpointAt(9090)));

        // Registered after the controller, so it runs in the same delivery pass
        // and the core is Starting again before the hop fires.
        StateHook hook;
        bool restarted = false;
        hook.onState = [&](CoreState state) {
            if (restarted) return;
            if (state != CoreState::Stopped && state != CoreState::Failed) return;
            restarted = true;
            fake.setValidationGate(Gate::Immediate);
            fake.start(second, QStringLiteral("/work"));
        };
        QVERIFY(fake.addObserver(&hook));

        fake.stop();
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        QVERIFY(drainPostedTimers());

        QVERIFY(restarted);
        QCOMPARE(fake.state(), CoreState::Starting);
        QCOMPARE(controller.restoreCount(), 0);
        QCOMPARE(log->count(Action::Restore), 0);
        QVERIFY(fake.removeObserver(&hook));
    }

    // --- group D, point 2, the 5 s grace window, compressed. Same rule: the
    //     re-validation, not the delay.

    void aControllerThatReturnsInsideTheGraceWindowKeepsItsProxy() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get(),
                                     RestoreDelays{0, kCompressedGraceMs});

        const Endpoint endpoint = endpointAt(9090);
        QVERIFY(bringUpCore(fake, QStringLiteral("/tmp/.runtime-a.yaml"), endpoint));
        fake.attach(endpoint);
        QVERIFY(fake.flushEvents());
        QVERIFY(fake.isConnected());

        // The controller blips and comes back inside the window.
        fake.disconnectController();
        QVERIFY(fake.flushEvents());
        QVERIFY(!fake.isConnected());
        fake.setControllerReachable(true);
        fake.attach(endpoint);
        QVERIFY(fake.flushEvents());
        QVERIFY(fake.isConnected());

        QVERIFY(drainPast(kCompressedGraceMs * 3));
        QCOMPARE(controller.restoreCount(), 0);
        QCOMPARE(log->count(Action::Restore), 0);

        // The control: a controller that does not come back IS restored, so
        // the case above is not passing because nothing was ever scheduled.
        fake.disconnectController();
        QVERIFY(fake.flushEvents());
        QVERIFY(drainPast(kCompressedGraceMs * 3));
        QCOMPARE(controller.restoreCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(log->count(Action::Restore), 1, kDeadlineMs);
    }

    // --- group D, point 4. main.cpp:249-252 emitted this through
    //     MihomoClient::errorOccurred - one object emitting another's signal.
    //     The controller owns its error channel now; the message is unchanged.

    void aFailedRestoreIsRaisedOnTheControllersOwnErrorChannel() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        log->restoreSucceeds = false;

        FakeBackend fake;
        RoutingController controller(fake, service.get());
        QSignalSpy errors(&controller, &RoutingController::errorOccurred);

        QVERIFY(bringUpCore(fake, QStringLiteral("/tmp/.runtime-a.yaml"), endpointAt(9090)));
        fake.stop();
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        QVERIFY(drainPostedTimers());

        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, kDeadlineMs);
        const QString message = errors.first().first().toString();
        QVERIFY2(message.startsWith(QStringLiteral("Could not restore the system proxy: ")),
                 qPrintable(message));
        QVERIFY(message.contains(QStringLiteral("the helper refused")));
        QCOMPARE(controller.lastError(), message);
    }

    // --- one controller, confirmed state. A TUN change in flight must not be
    //     reported as applied by any surface.

    void aTunChangeInFlightIsNeverReportedAsApplied() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get());

        const Endpoint endpoint = endpointAt(9090);
        fake.staged().config.tunEnabled = false;
        fake.attach(endpoint);
        QVERIFY(fake.flushEvents());
        QVERIFY(fake.isConnected());
        QCOMPARE(controller.tunEnabled(), false);
        QVERIFY(controller.tunAvailable());

        QSignalSpy confirmed(&controller, &RoutingController::tunConfirmed);
        fake.setRequestGate(Gate::Held);
        fake.staged().tunActual = true;
        controller.requestTun(true);

        // Held: pending, and the confirmed value has not moved.
        QVERIFY(controller.tunPending());
        QCOMPARE(controller.tunEnabled(), false);
        QVERIFY(!controller.tunAvailable());
        QVERIFY(fake.flushEvents());
        QCOMPARE(confirmed.size(), 0);
        QCOMPARE(controller.tunEnabled(), false);

        core::backend::RequestId tunRequest = core::backend::RequestId::Invalid;
        for (auto id : fake.pendingRequests())
            if (fake.pendingKind(id) == RequestKind::SetTun) tunRequest = id;
        QVERIFY(tunRequest != core::backend::RequestId::Invalid);
        QVERIFY(fake.releaseRequest(tunRequest, RequestOutcome::Success));
        QVERIFY(fake.flushEvents());

        QVERIFY(!controller.tunPending());
        QCOMPARE(controller.tunEnabled(), true);
        QCOMPARE(confirmed.size(), 1);
        QCOMPARE(confirmed.first().first().toBool(), true);
    }

    // --- the read-back is not an echo. A change the controller did not apply
    //     is not a success, however it completed.

    void aTunReadBackThatDisagreesWithTheRequestIsNotAnAppliedChange() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get());

        fake.staged().config.tunEnabled = false;
        fake.attach(endpointAt(9090));
        QVERIFY(fake.flushEvents());

        QSignalSpy confirmed(&controller, &RoutingController::tunConfirmed);
        // The controller reports back that TUN is still off.
        fake.staged().tunActual = false;
        controller.requestTun(true);
        QVERIFY(fake.flushEvents());

        QVERIFY(!controller.tunPending());
        QCOMPARE(controller.tunEnabled(), false);
        QCOMPARE(confirmed.size(), 0);
    }

    // --- settings, toolbar and tray read ONE confirmed answer, and none of
    //     them sees the requested value while the OS call is in flight.

    void everyRoutingSurfaceReadsTheSameConfirmedSystemProxyState() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get());

        fake.attach(endpointAt(9090));
        QVERIFY(fake.flushEvents());
        QVERIFY(fake.isConnected());
        Config target;
        target.host = QStringLiteral("127.0.0.1");
        target.port = 7890;
        controller.setProxyTarget(target);
        service->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!service->isBusy(), kDeadlineMs);

        struct Surface {
            QString name;
            QVector<QPair<bool, bool>> samples;  // (pending, confirmed-enabled)
        };
        Surface settings{QStringLiteral("settings"), {}};
        Surface toolbar{QStringLiteral("toolbar"), {}};
        Surface tray{QStringLiteral("tray"), {}};
        const auto sample = [&controller](Surface &surface) {
            surface.samples.append({controller.systemProxyPending(), controller.systemProxyEnabled()});
        };
        connect(&controller, &RoutingController::routingStateChanged, this,
                [&] { sample(settings); sample(toolbar); sample(tray); });

        // The OS call is held, so the pending window is observable rather than
        // a race.
        log->holdEnabled = true;
        controller.requestSystemProxy(true);
        QTRY_VERIFY_WITH_TIMEOUT(log->count(Action::Enable) == 1, kDeadlineMs);
        QVERIFY(controller.systemProxyPending());
        QCOMPARE(controller.systemProxyEnabled(), false);

        log->hold.release();
        QTRY_VERIFY_WITH_TIMEOUT(!service->isBusy(), kDeadlineMs);
        QTRY_VERIFY_WITH_TIMEOUT(controller.systemProxyEnabled(), kDeadlineMs);
        QVERIFY(!controller.systemProxyPending());

        // Every surface saw exactly the same sequence...
        QVERIFY(!settings.samples.isEmpty());
        QCOMPARE(settings.samples, toolbar.samples);
        QCOMPARE(settings.samples, tray.samples);
        // ...and none of them ever read "on" while the change was in flight.
        for (const auto &sampleValue : settings.samples)
            QVERIFY2(!(sampleValue.first && sampleValue.second),
                     "a pending change was reported as the confirmed state");
    }

    // --- the tray action and the hotkey are the same intent path as the
    //     settings toggle, and are inert when the change is not allowed.

    void theTrayActionAndTheHotkeyUseTheSameIntentPath() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get());

        // No connected core and no reported port: the hotkey must do nothing,
        // exactly as the disabled toggle it used to drive did nothing.
        QVERIFY(!controller.systemProxyAvailable());
        controller.toggleSystemProxy();
        QVERIFY(drainPostedTimers());
        QCOMPARE(log->count(Action::Enable), 0);

        // And an explicit request is refused with the reason, not silently.
        QSignalSpy errors(&controller, &RoutingController::errorOccurred);
        controller.requestSystemProxy(true);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().first().toString(),
                 QStringLiteral("Connect to a core with a reported proxy port first."));
        QCOMPARE(log->count(Action::Enable), 0);

        // Connected, with a target: now the hotkey drives the same call the
        // toolbar and the tray drive.
        fake.attach(endpointAt(9090));
        QVERIFY(fake.flushEvents());
        Config target;
        target.host = QStringLiteral("127.0.0.1");
        target.port = 7890;
        controller.setProxyTarget(target);
        service->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!service->isBusy(), kDeadlineMs);
        QVERIFY(controller.systemProxyAvailable());

        controller.toggleSystemProxy();
        QTRY_VERIFY_WITH_TIMEOUT(controller.systemProxyEnabled(), kDeadlineMs);
        QCOMPARE(log->count(Action::Enable), 1);

        // Toggling back goes through the same path and confirms off.
        controller.toggleSystemProxy();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.systemProxyEnabled(), kDeadlineMs);
        QCOMPARE(log->count(Action::Disable), 1);
    }

    // --- the mode box and the cycle hotkey: confirmed, not requested.

    void theRoutingModeIsConfirmedAndTheHotkeyCyclesIt() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get());

        fake.staged().config.mode = QStringLiteral("rule");
        fake.attach(endpointAt(9090));
        QVERIFY(fake.flushEvents());
        QCOMPARE(controller.mode(), QStringLiteral("rule"));

        QSignalSpy confirmed(&controller, &RoutingController::modeConfirmed);
        fake.setRequestGate(Gate::Held);
        controller.cycleMode();
        QVERIFY(controller.modePending());
        // The requested mode is not the reported one while it is in flight.
        QCOMPARE(controller.mode(), QStringLiteral("rule"));
        QVERIFY(fake.flushEvents());
        QCOMPARE(confirmed.size(), 0);

        core::backend::RequestId modeRequest = core::backend::RequestId::Invalid;
        for (auto id : fake.pendingRequests())
            if (fake.pendingKind(id) == RequestKind::SetMode) modeRequest = id;
        QVERIFY(modeRequest != core::backend::RequestId::Invalid);
        QVERIFY(fake.releaseRequest(modeRequest, RequestOutcome::Success));
        QVERIFY(fake.flushEvents());

        QVERIFY(!controller.modePending());
        QCOMPARE(controller.mode(), QStringLiteral("global"));
        QCOMPARE(confirmed.size(), 1);
        QCOMPARE(confirmed.first().first().toString(), QStringLiteral("global"));

        // The cycle wraps in the order the toolbar presents.
        fake.setRequestGate(Gate::Immediate);
        controller.cycleMode();
        QVERIFY(fake.flushEvents());
        QCOMPARE(controller.mode(), QStringLiteral("direct"));
        controller.cycleMode();
        QVERIFY(fake.flushEvents());
        QCOMPARE(controller.mode(), QStringLiteral("rule"));
        QCOMPARE(RoutingController::modes(),
                 (QStringList{QStringLiteral("rule"), QStringLiteral("global"),
                              QStringLiteral("direct")}));
    }

    // --- a failed mode change leaves the confirmed value where it was and
    //     does not strand the pending flag.

    void aFailedModeChangeDoesNotMoveTheConfirmedValue() {
        auto log = std::make_shared<ProxyOperations>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(log, osState);
        FakeBackend fake;
        RoutingController controller(fake, service.get());

        fake.staged().config.mode = QStringLiteral("rule");
        fake.attach(endpointAt(9090));
        QVERIFY(fake.flushEvents());

        QSignalSpy confirmed(&controller, &RoutingController::modeConfirmed);
        fake.setRequestGate(Gate::Held);
        controller.requestMode(QStringLiteral("direct"));
        core::backend::RequestId modeRequest = core::backend::RequestId::Invalid;
        for (auto id : fake.pendingRequests())
            if (fake.pendingKind(id) == RequestKind::SetMode) modeRequest = id;
        QVERIFY(modeRequest != core::backend::RequestId::Invalid);
        QVERIFY(fake.releaseRequest(modeRequest, RequestOutcome::Failure));
        QVERIFY(fake.flushEvents());

        QVERIFY(!controller.modePending());
        QCOMPARE(controller.mode(), QStringLiteral("rule"));
        QCOMPARE(confirmed.size(), 0);
    }

  private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(RoutingControllerTest)
#include "routing_controller_test.moc"
