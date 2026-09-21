// Acceptance suite for app::lifecycle::ShutdownCoordinator.
//
// Specification: PRE-ARCH section 4b group E, "Preserve (acceptance checklist
// for MOD-LIFECYCLE)", items 1-10, plus backend-r2 section 6 for the
// unconfirmed stop. Every item has a named case below and the mapping is stated
// in the comment above each one.
//
// NOTHING HERE SLEEPS, and no outcome is decided by the 50 ms poll. Every
// condition is held explicitly, asserted to block, released explicitly, and the
// next step then asserted to run. The only waits are FakeBackend::waitFor and
// flushEvents, which bound a failure and never produce one; the zero-millisecond
// approval hop has to be observed across an event-loop turn, and observing it is
// the point of item 1.

#include <QtTest>

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include "app/lifecycle/quit_guard.h"
#include "app/lifecycle/shutdown_coordinator.h"
#include "app/lifecycle/shutdown_ports.h"
#include "support/backend/fake_backend.h"

using app::lifecycle::QuitGuard;
using app::lifecycle::ShutdownCoordinator;
using app::lifecycle::SystemProxyShutdown;
using app::lifecycle::TaskDrain;

using core::backend::BackendTimings;
using core::backend::CoreState;
using core::backend::Endpoint;
using core::backend::ErrorCode;
using core::backend::ExecutionMode;
using core::backend::Generation;
using core::backend::Ownership;
using core::backend::StopCompleted;

using testsupport::backend::FakeBackend;

namespace {

constexpr BackendTimings kTimings{};

Endpoint endpointAt(const char *host, quint16 port) {
    Endpoint endpoint;
    endpoint.host = QString::fromLatin1(host);
    endpoint.port = port;
    return endpoint;
}

// Brings a managed core from Stopped to Running, releasing each gate by hand.
// Deliberately a copy of the backend contract suite's helper rather than a
// shared one: a change there must not silently change what this suite starts
// from.
bool bringUpCore(FakeBackend &fake,
                 const QString &configPath = QStringLiteral("/tmp/.runtime-1.yaml")) {
    fake.mapConfigFile(configPath, endpointAt("127.0.0.1", 9090));
    fake.start(configPath, QStringLiteral("/work"));
    if (!fake.completeValidation(true)) return false;
    fake.advanceTime(kTimings.probeIntervalMs);
    fake.flushEvents();
    return fake.state() == CoreState::Running;
}

// The system proxy, held. requestShutdown() records the call and answers
// nothing until the test says so: that is what makes "the core stop strictly
// follows the proxy restore" assertable rather than merely likely.
class HeldProxy final : public SystemProxyShutdown {
  public:
    void requestShutdown() override { ++requests; }
    int requests = 0;
};

// The proxy, recording its position in a sequence.
class RecordingProxy final : public SystemProxyShutdown {
  public:
    explicit RecordingProxy(QStringList *log) : log_(log) {}
    void requestShutdown() override { log_->append(QStringLiteral("proxy")); }

  private:
    QStringList *log_;
};

// The global task pool, as a number the test owns.
class CountingDrain final : public TaskDrain {
  public:
    int activeTaskCount() const override { return count; }
    int count = 0;
};

// A busy source the test holds and releases.
struct Holdable {
    bool busy = false;
    std::function<bool()> predicate() {
        return [this] { return busy; };
    }
};

}  // namespace

class ShutdownCoordinatorTest : public QObject {
    Q_OBJECT

  private slots:

    // --- item 1. Quit interception: the application must not quit until
    //     approved, and approval is granted exactly once, deferred through a
    //     zero-millisecond hop rather than called directly from inside the
    //     event filter.

    void quitIsHeldUntilApprovedAndApprovalIsDeferredByOneEventLoopTurn() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        Holdable gate;
        ShutdownCoordinator coordinator(fake, proxy, drain);
        QuitGuard guard;
        coordinator.setQuitGuard(&guard);
        connect(&guard, &QuitGuard::quitRequested, &coordinator, &ShutdownCoordinator::requestQuit);
        QCoreApplication::instance()->installEventFilter(&guard);

        // Held so that the last gate can be released synchronously, with no
        // event processing in between: that is the only way to observe that the
        // approval is deferred rather than immediate.
        gate.busy = true;
        coordinator.addBusyGate(QStringLiteral("gate"), gate.predicate());
        QSignalSpy approvals(&coordinator, &ShutdownCoordinator::quitApproved);

        // A Quit event while the guard is unapproved is swallowed - the filter
        // returns true, so QCoreApplication::event() never sees it - and it is
        // what starts the shutdown.
        QEvent quit(QEvent::Quit);
        QCoreApplication::sendEvent(QCoreApplication::instance(), &quit);
        QVERIFY(!quit.isAccepted());
        QVERIFY(coordinator.isShuttingDown());
        QVERIFY(!guard.isApproved());

        // An event of another type is none of the guard's business.
        QEvent other(QEvent::User);
        QVERIFY(!guard.eventFilter(QCoreApplication::instance(), &other));

        coordinator.onProxyShutdownFinished(true, {});
        fake.flushEvents();
        QVERIFY(coordinator.isCoreStopped());
        QVERIFY(!guard.isApproved());
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("gate"));

        // The guard is released synchronously - a Quit arriving in the gap must
        // not be intercepted a second time - but the quit itself is deferred by
        // one event-loop turn, never called from inside the filter.
        gate.busy = false;
        coordinator.reevaluate();
        QVERIFY(guard.isApproved());
        QVERIFY(coordinator.isQuitApproved());
        QCOMPARE(approvals.count(), 0);
        QVERIFY(FakeBackend::waitFor([&approvals] { return approvals.count() == 1; }));

        // Approved exactly once, however often the gate is re-evaluated.
        coordinator.reevaluate();
        coordinator.reevaluate();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCOMPARE(approvals.count(), 1);

        // And an approved guard no longer intercepts.
        QEvent afterwards(QEvent::Quit);
        QVERIFY(!guard.eventFilter(QCoreApplication::instance(), &afterwards));

        QCoreApplication::instance()->removeEventFilter(&guard);
    }

    // --- item 2. requestQuit() is idempotent (main.cpp:234).

    void repeatedQuitRequestsRunTheShutdownActionsOnce() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);

        int cancels = 0;
        coordinator.addQuitAction(QStringLiteral("cancel"), [&cancels] { ++cancels; });
        QSignalSpy started(&coordinator, &ShutdownCoordinator::shutdownStarted);

        coordinator.requestQuit();
        coordinator.requestQuit();
        coordinator.requestQuit();

        QCOMPARE(cancels, 1);
        QCOMPARE(started.count(), 1);
        QCOMPARE(proxy.requests, 1);
    }

    // --- item 3, THE ORDERING RULE. The system proxy shutdown strictly
    //     precedes the core stop (main.cpp:225): the stop is issued from inside
    //     the proxy's completion and from nowhere else.

    void systemProxyShutdownStrictlyPrecedesTheCoreStop() {
        FakeBackend fake;
        QVERIFY(bringUpCore(fake));
        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);

        coordinator.requestQuit();
        QCOMPARE(proxy.requests, 1);

        // HELD: the proxy has not answered. The core must still be running, and
        // re-evaluating the gate any number of times must not stop it.
        QVERIFY(!coordinator.isStopRequested());
        QCOMPARE(fake.state(), CoreState::Running);
        for (int i = 0; i < 5; ++i) coordinator.reevaluate();
        fake.flushEvents();
        QCOMPARE(fake.state(), CoreState::Running);
        QVERIFY(!coordinator.isStopRequested());
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("system-proxy"));

        // RELEASED: and only now is the core stopped.
        coordinator.onProxyShutdownFinished(true, {});
        QVERIFY(coordinator.isStopRequested());
        QCOMPARE(fake.state(), CoreState::Stopping);
        QVERIFY(fake.isChildTerminating());

        // The stop is outstanding, so the quit is still blocked - on the stop
        // now, not on the proxy.
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("core-stop"));
        QVERIFY(fake.releaseChildExit());
        fake.flushEvents();
        QVERIFY(coordinator.isCoreStopped());
        QVERIFY(coordinator.wasLastStopConfirmed());
        QCOMPARE(coordinator.blockingReason(), QString());
    }

    // --- item 3, the other direction. A failed proxy restore must still stop
    //     the core - main.cpp calls stop() outside the !success branch - and
    //     the failure must raise a warning.

    void aFailedProxyRestoreStillStopsTheCoreAndWarns() {
        FakeBackend fake;
        QVERIFY(bringUpCore(fake));
        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);
        QSignalSpy warnings(&coordinator, &ShutdownCoordinator::warningRaised);

        coordinator.requestQuit();
        coordinator.onProxyShutdownFinished(false, QStringLiteral("networksetup failed"));

        QCOMPARE(warnings.count(), 1);
        QCOMPARE(warnings.at(0).at(1).toString(), QStringLiteral("System Proxy"));
        QCOMPARE(warnings.at(0).at(2).toString(), QStringLiteral("networksetup failed"));
        QVERIFY(coordinator.isStopRequested());
        QCOMPARE(fake.state(), CoreState::Stopping);
    }

    // --- item 4. coreStopped is reset when the quit begins, after it may
    //     already have been set by an earlier, unrelated stop (main.cpp:237).
    //     Without the reset the quit would approve itself before its own stop
    //     ever completed.

    void anEarlierStopDoesNotSatisfyTheQuitsOwnStop() {
        FakeBackend fake;
        QVERIFY(bringUpCore(fake));
        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);

        // An unrelated stop, long before any quit - a backup restore, say.
        fake.stop();
        QVERIFY(fake.releaseChildExit());
        fake.flushEvents();
        QVERIFY(coordinator.isCoreStopped());

        coordinator.requestQuit();
        QVERIFY(!coordinator.isCoreStopped());
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("system-proxy"));

        coordinator.onProxyShutdownFinished(true, {});
        fake.flushEvents();
        QCOMPARE(coordinator.blockingReason(), QString());
    }

    // --- item 5. The shutdown state is published as a query, and the
    //     "shuttingDown" application property ui/backup_page.cpp:137 reads is
    //     still set while that read exists.

    void shutdownStateIsAQueryAndStillSetsTheApplicationProperty() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        QCoreApplication::instance()->setProperty("shuttingDown", QVariant());
        ShutdownCoordinator coordinator(fake, proxy, drain);

        const app::lifecycle::ShutdownState &state = coordinator;
        QVERIFY(!state.isShuttingDown());
        QVERIFY(!QCoreApplication::instance()->property("shuttingDown").toBool());

        coordinator.requestQuit();

        QVERIFY(state.isShuttingDown());
        QVERIFY(QCoreApplication::instance()->property("shuttingDown").toBool());
        QCoreApplication::instance()->setProperty("shuttingDown", QVariant());
    }

    // --- item 6. Every quit action runs, in registration order, before
    //     anything is awaited; the proxy shutdown is requested last.

    void quitActionsRunInOrderBeforeTheProxyShutdownIsRequested() {
        FakeBackend fake;
        CountingDrain drain;
        QStringList order;
        RecordingProxy proxy(&order);

        ShutdownCoordinator coordinator(fake, proxy, drain);
        coordinator.addQuitAction(QStringLiteral("reload-stop"),
                                  [&order] { order.append(QStringLiteral("reload-stop")); });
        coordinator.addQuitAction(QStringLiteral("profiles"),
                                  [&order] { order.append(QStringLiteral("profiles")); });
        coordinator.addQuitAction(QStringLiteral("enhancer"),
                                  [&order] { order.append(QStringLiteral("enhancer")); });
        coordinator.addQuitAction(QStringLiteral("backups"),
                                  [&order] { order.append(QStringLiteral("backups")); });
        connect(&coordinator, &ShutdownCoordinator::shutdownStarted, this,
                [&order] { order.append(QStringLiteral("ui-disabled")); });

        coordinator.requestQuit();

        QCOMPARE(order, (QStringList{QStringLiteral("reload-stop"), QStringLiteral("profiles"),
                                     QStringLiteral("enhancer"), QStringLiteral("backups"),
                                     QStringLiteral("ui-disabled"), QStringLiteral("proxy")}));
    }

    // --- item 7. Warnings are non-blocking, the coordinator owns no widget,
    //     and each one holds the quit open until it is dismissed.

    void eachWarningHoldsTheQuitOpenUntilItIsDismissed() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);
        QSignalSpy warnings(&coordinator, &ShutdownCoordinator::warningRaised);

        coordinator.requestQuit();
        coordinator.onProxyShutdownFinished(false, QStringLiteral("first"));
        fake.flushEvents();

        QCOMPARE(warnings.count(), 1);
        QCOMPARE(coordinator.pendingWarnings(), 1);
        QVERIFY(coordinator.isCoreStopped());
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("warnings"));
        QVERIFY(!coordinator.isQuitApproved());

        const quint64 id = warnings.at(0).at(0).toULongLong();

        // Dismissing something that was never raised changes nothing.
        coordinator.dismissWarning(id + 1000);
        QCOMPARE(coordinator.pendingWarnings(), 1);
        QVERIFY(!coordinator.isQuitApproved());

        coordinator.dismissWarning(id);
        QCOMPARE(coordinator.pendingWarnings(), 0);
        QVERIFY(coordinator.isQuitApproved());

        // Dismissing the same warning twice cannot drive the counter negative,
        // which the bare "--shutdownWarnings" of main.cpp:203 would have.
        coordinator.dismissWarning(id);
        QCOMPARE(coordinator.pendingWarnings(), 0);
    }

    // --- item 8, THE GATE ORDER. warnings -> backup busy -> profile runtime
    //     busy -> profile file busy -> enhancer file busy -> final prune ->
    //     thread-pool drain. Each is held, proved to block, released, and the
    //     next one proved to be what blocks now.

    void theBusyGatesAreEvaluatedInTheDeclaredOrder() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        Holdable backup;
        Holdable runtime;
        Holdable profileFiles;
        Holdable enhancerFiles;

        ShutdownCoordinator coordinator(fake, proxy, drain);
        coordinator.addBusyGate(QStringLiteral("backup"), backup.predicate());
        coordinator.addBusyGate(QStringLiteral("profile-runtime"), runtime.predicate());
        coordinator.addBusyGate(QStringLiteral("profile-files"), profileFiles.predicate());
        coordinator.addBusyGate(QStringLiteral("enhancer-files"), enhancerFiles.predicate());

        int prunes = 0;
        coordinator.setFinalCleanup([&prunes] { ++prunes; });
        QSignalSpy warnings(&coordinator, &ShutdownCoordinator::warningRaised);

        backup.busy = runtime.busy = profileFiles.busy = enhancerFiles.busy = true;
        drain.count = 1;

        QCOMPARE(coordinator.blockingReason(), QStringLiteral("not-quitting"));
        coordinator.requestQuit();
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("system-proxy"));

        coordinator.onProxyShutdownFinished(false, QStringLiteral("boom"));
        fake.flushEvents();
        // The core stop completed, so the next thing in the way is the warning.
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("warnings"));

        QCOMPARE(warnings.count(), 1);
        coordinator.dismissWarning(warnings.at(0).at(0).toULongLong());
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("backup"));
        QCOMPARE(prunes, 0);

        backup.busy = false;
        coordinator.reevaluate();
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("profile-runtime"));
        QCOMPARE(prunes, 0);

        runtime.busy = false;
        coordinator.reevaluate();
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("profile-files"));
        QCOMPARE(prunes, 0);

        profileFiles.busy = false;
        coordinator.reevaluate();
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("enhancer-files"));
        // THE POINT OF THIS CASE: the prune has still not run, because one busy
        // gate is still held. Reordering it before the gates would delete a
        // snapshot a validation child is still reading.
        QCOMPARE(prunes, 0);

        enhancerFiles.busy = false;
        coordinator.reevaluate();
        // Every busy gate is clear, so the prune has run - and the drain is now
        // what blocks, which proves the prune is sequenced BEFORE the drain
        // check and not after it.
        QCOMPARE(prunes, 1);
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("thread-pool"));
        QVERIFY(!coordinator.isQuitApproved());

        drain.count = 0;
        coordinator.reevaluate();
        QCOMPARE(coordinator.blockingReason(), QString());
        QVERIFY(coordinator.isQuitApproved());
    }

    // --- THE ONE-SHOT PRUNE. main.cpp:187-190 guards the final prune with
    //     shutdownCleanupStarted because the gate is re-entered on every poll
    //     tick and every busy notification; running it again would re-queue
    //     deletions and the drain check would never settle.

    void theFinalPruneRunsExactlyOnceHoweverOftenTheGateIsReEvaluated() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);

        int prunes = 0;
        coordinator.setFinalCleanup([&prunes] { ++prunes; });
        drain.count = 1;  // hold the last gate, so the gate stays re-enterable

        coordinator.requestQuit();
        coordinator.onProxyShutdownFinished(true, {});
        fake.flushEvents();

        QCOMPARE(prunes, 1);
        QVERIFY(coordinator.isFinalCleanupDone());
        QVERIFY(!coordinator.isQuitApproved());

        for (int i = 0; i < 20; ++i) coordinator.reevaluate();
        QCOMPARE(prunes, 1);

        drain.count = 0;
        coordinator.reevaluate();
        QCOMPARE(prunes, 1);
        QVERIFY(coordinator.isQuitApproved());
    }

    // --- item 9. The unsignalled conditions - isRuntimeBusy, isFileBusy and
    //     activeThreadCount emit nothing - are re-polled. Signalled sources do
    //     not wait for a tick: every case above drives the gate through
    //     reevaluate() and never through the timer.

    void unsignalledConditionsAreRePolled() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        Holdable silent;

        ShutdownCoordinator coordinator(fake, proxy, drain);
        coordinator.setPollIntervalMs(1);
        silent.busy = true;
        coordinator.addBusyGate(QStringLiteral("silent"), silent.predicate());

        coordinator.requestQuit();
        coordinator.onProxyShutdownFinished(true, {});
        fake.flushEvents();
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("silent"));

        // Nothing will ever notify the coordinator that this went idle. The
        // poll is the only thing that can notice, and it must.
        silent.busy = false;
        QVERIFY(FakeBackend::waitFor([&coordinator] { return coordinator.isQuitApproved(); }));
    }

    // --- backend-r2 section 6. An unconfirmed stop blocks the quit with a
    //     warning and is NOT reported as success.

    void anUnconfirmedStopBlocksTheQuitAndIsNeverReportedAsSuccess() {
        FakeBackend fake;
        QVERIFY(fake.setExecutionMode(ExecutionMode::PrivilegedService));
        QVERIFY(bringUpCore(fake));
        QVERIFY(fake.usesPrivilegedService());

        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);
        QSignalSpy warnings(&coordinator, &ShutdownCoordinator::warningRaised);

        coordinator.requestQuit();
        coordinator.onProxyShutdownFinished(true, {});
        QVERIFY(coordinator.isStopRequested());

        // Lease cleanup was requested; the privileged service then went away
        // without confirming that the child exited.
        QVERIFY(fake.disconnectPrivilegedService(QStringLiteral(
            "The privileged service disconnected before confirming core shutdown.")));
        fake.flushEvents();

        // It counts as "the stop finished" - the quit must not hang forever on
        // it - but it is explicitly not a success.
        QVERIFY(coordinator.isCoreStopped());
        QVERIFY(!coordinator.wasLastStopConfirmed());

        QCOMPARE(warnings.count(), 1);
        QCOMPARE(warnings.at(0).at(1).toString(), QStringLiteral("Core Shutdown"));
        QVERIFY(warnings.at(0).at(2).toString().contains(QStringLiteral("privileged service")));
        QCOMPARE(coordinator.pendingWarnings(), 1);
        QCOMPARE(coordinator.blockingReason(), QStringLiteral("warnings"));
        QVERIFY(!coordinator.isQuitApproved());

        coordinator.dismissWarning(warnings.at(0).at(0).toULongLong());
        QVERIFY(coordinator.isQuitApproved());
    }

    // --- The counterpart: the warning is tied to `confirmed`, not to "a stop
    //     happened". A confirmed stop raises nothing.

    void aConfirmedStopRaisesNoWarning() {
        FakeBackend fake;
        QVERIFY(bringUpCore(fake));
        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);
        QSignalSpy warnings(&coordinator, &ShutdownCoordinator::warningRaised);

        coordinator.requestQuit();
        coordinator.onProxyShutdownFinished(true, {});
        QVERIFY(fake.releaseChildExit());
        fake.flushEvents();

        QCOMPARE(warnings.count(), 0);
        QVERIFY(coordinator.wasLastStopConfirmed());
        QVERIFY(coordinator.isQuitApproved());
    }

    // --- An unconfirmed stop that arrives outside a quit must not raise a
    //     shutdown warning (main.cpp:198 guards on `quitting`), but it must
    //     still be recorded as unconfirmed rather than as success.

    void anUnconfirmedStopOutsideAQuitRecordsItselfWithoutAWarning() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);
        QSignalSpy warnings(&coordinator, &ShutdownCoordinator::warningRaised);

        StopCompleted result;
        result.generation = fake.generation();
        result.confirmed = false;
        result.reason = {ErrorCode::ServiceDisconnected, QStringLiteral("gone")};
        coordinator.backendObserver().stopCompleted(result);

        QCOMPARE(warnings.count(), 0);
        QVERIFY(coordinator.isCoreStopped());
        QVERIFY(!coordinator.wasLastStopConfirmed());
    }

    // --- backend-r2 section 2, the consumer obligation. A completion stamped
    //     with a superseded generation is rejected, and rejecting it must not
    //     be mistaken for the stop having completed.

    void aStopCompletionFromASupersededGenerationIsRejected() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        ShutdownCoordinator coordinator(fake, proxy, drain);

        coordinator.backendObserver().coreStateChanged(static_cast<Generation>(7),
                                                       CoreState::Stopping, Ownership::Managed);
        QCOMPARE(coordinator.lastObservedGeneration(), static_cast<Generation>(7));

        StopCompleted stale;
        stale.generation = static_cast<Generation>(3);
        stale.confirmed = true;
        coordinator.backendObserver().stopCompleted(stale);
        QVERIFY(!coordinator.isCoreStopped());

        StopCompleted current;
        current.generation = static_cast<Generation>(8);
        current.confirmed = true;
        coordinator.backendObserver().stopCompleted(current);
        QVERIFY(coordinator.isCoreStopped());
    }

    // --- item 10. The post-exec() disconnects existed only because the
    //     shutdown lambdas captured stack locals. Here the coordinator owns its
    //     state and withdraws its own observer registration.

    void teardownWithdrawsTheObserverRegistration() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        QVERIFY(bringUpCore(fake));

        auto *coordinator = new ShutdownCoordinator(fake, proxy, drain);
        // Registered by the constructor: a second registration is refused.
        QVERIFY(!fake.addObserver(coordinator));
        delete coordinator;

        // Events keep flowing to the backend afterwards. A registration left
        // behind would be a dangling observer, which this drives into.
        fake.stop();
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Stopped);
    }
};

QTEST_GUILESS_MAIN(ShutdownCoordinatorTest)
#include "shutdown_coordinator_test.moc"
