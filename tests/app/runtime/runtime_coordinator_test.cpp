// RuntimeCoordinator: reload scheduling and snapshot retention.
//
// Acceptance source: PRE-ARCH section 4b, groups B and C, which lists with
// file:line every behaviour that had to survive the move out of src/main.cpp.
// Each case below names the inventory point it protects.
//
// Nothing here sleeps. The backend is the deterministic fake: every
// asynchronous step is a gate that is held, observed, then released, and the
// only waits are QTRY deadlines that bound a failure rather than produce one.

#include <QtTest>

#include <QCoreApplication>

#include <atomic>
#include <memory>

#include <QFile>
#include <QMutex>
#include <QSignalSpy>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include "app/runtime/runtime_coordinator.h"
#include "support/backend/fake_backend.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

using app::runtime::RuntimeCoordinator;

using core::backend::CoreState;
using core::backend::Endpoint;

using testsupport::backend::FakeBackend;
using testsupport::backend::Gate;

namespace {

constexpr int kDeadlineMs = 3000;

Endpoint endpointAt(quint16 port) {
    Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = port;
    return endpoint;
}

// Everything RuntimeCoordinator asks of core::ProfileStore, and nothing else.
class StubConfigSource final : public app::runtime::ConfigSource {
  public:
    QString selection = QStringLiteral("profile-a");
    QString dir = QStringLiteral("/work");
    int requests = 0;

    QString currentSelection() const override { return selection; }
    QString workDir() const override { return dir; }
    void requestRuntimeConfig() override { ++requests; }
};

// Runs the event loop until every already-posted zero-delay timer has fired.
// A zero-delay timer posted earlier expires earlier, so a marker posted after
// the coordinator's own hop is guaranteed to run after it. This is how "the
// deferred decision ran and decided not to act" is observed without a sleep.
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

bool touch(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write("port: 7890\n");
    return true;
}

// Stopped -> Running, releasing each gate by hand.
bool bringUpCore(FakeBackend &fake, const QString &configPath, const Endpoint &endpoint) {
    fake.mapConfigFile(configPath, endpoint);
    fake.start(configPath, QStringLiteral("/work"));
    if (!fake.completeValidation(true)) return false;
    fake.advanceTime(fake.timings().probeIntervalMs);
    fake.flushEvents();
    return fake.state() == CoreState::Running;
}

}  // namespace

class RuntimeCoordinatorTest : public QObject {
    Q_OBJECT

  private slots:

    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("modrt"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }

    void cleanupTestCase() {
        const QString escape = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escape.isEmpty(), qPrintable(escape));
        environment_.reset();
    }

    // --- group B, point 2. The three-way liveness gate, duplicated at
    //     main.cpp:138-142 and :151-153, is one expression here and it is the
    //     backend's published predicate rather than a recombination of
    //     state() and isRestartPending().

    void theReloadGateIsTheBackendsOwnPredicateForEveryState() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);

        // Stopped: never reload against a core that is genuinely stopped.
        QCOMPARE(fake.state(), CoreState::Stopped);
        QCOMPARE(coordinator.canReload(), false);
        QCOMPARE(coordinator.canReload(), fake.isManagedCoreActive());

        // Starting: the validated candidate is launched and probing.
        const QString first = QStringLiteral("/tmp/.runtime-first.yaml");
        fake.mapConfigFile(first, endpointAt(9090));
        fake.start(first, QStringLiteral("/work"));
        QVERIFY(fake.completeValidation(true));
        QCOMPARE(fake.state(), CoreState::Starting);
        QCOMPARE(coordinator.canReload(), true);
        QCOMPARE(coordinator.canReload(), fake.isManagedCoreActive());

        // Running.
        fake.advanceTime(fake.timings().probeIntervalMs);
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Running);
        QCOMPARE(coordinator.canReload(), true);

        // Stopping WITH a restart pending: the retiring child is on its way out
        // and a validated launch is waiting for its exit. Reloading is correct.
        const QString second = QStringLiteral("/tmp/.runtime-second.yaml");
        fake.mapConfigFile(second, endpointAt(9091));
        fake.setValidationGate(Gate::Immediate);
        fake.start(second, QStringLiteral("/work"));
        QCOMPARE(fake.state(), CoreState::Stopping);
        QVERIFY(fake.isRestartPending());
        QCOMPARE(coordinator.canReload(), true);
        QCOMPARE(coordinator.canReload(), fake.isManagedCoreActive());

        // Stopping WITHOUT a restart pending: the half a `state != Stopped`
        // gate would get wrong.
        fake.stop();
        QCOMPARE(fake.state(), CoreState::Stopping);
        QVERIFY(!fake.isRestartPending());
        QCOMPARE(coordinator.canReload(), false);
        QCOMPARE(coordinator.canReload(), fake.isManagedCoreActive());

        // Failed.
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        FakeBackend fresh;
        StubConfigSource freshConfigs;
        RuntimeCoordinator freshCoordinator(fresh, freshConfigs);
        QVERIFY(bringUpCore(fresh, QStringLiteral("/tmp/.runtime-crash.yaml"), endpointAt(9092)));
        QVERIFY(fresh.crashChild(2));
        QVERIFY(fresh.flushEvents());
        QCOMPARE(fresh.state(), CoreState::Failed);
        QCOMPARE(freshCoordinator.canReload(), false);
        QCOMPARE(freshCoordinator.canReload(), fresh.isManagedCoreActive());
    }

    // --- group B, point 1. main.cpp:135-137.

    void theReloadDebounceIsOneHundredMillisecondsAndCoalescesTriggers() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);
        QVERIFY(bringUpCore(fake, QStringLiteral("/tmp/.runtime-a.yaml"), endpointAt(9090)));

        QCOMPARE(RuntimeCoordinator::kReloadDebounceMs, 100);
        QCOMPARE(coordinator.reloadDebounceMs(), 100);

        // currentProfileChanged, profileUpdated and chainChanged all arrive as
        // scheduleReload(); a burst must regenerate the configuration once.
        coordinator.scheduleReload();
        coordinator.scheduleReload();
        coordinator.scheduleReload();
        QVERIFY(coordinator.isReloadScheduled());
        QCOMPARE(configs.requests, 0);

        QTRY_COMPARE_WITH_TIMEOUT(configs.requests, 1, kDeadlineMs);
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.isReloadScheduled(), kDeadlineMs);
        QCOMPARE(configs.requests, 1);
    }

    // --- group B, point 2, the half a schedule-time-only gate misses. The
    //     gate is applied again when the timer FIRES, because the core can
    //     stop inside the 100 ms window: main.cpp re-tested it at :139 and
    //     that copy is load-bearing, not a duplicate of :151.

    void aCoreThatStopsInsideTheDebounceWindowIsNotReloaded() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);
        QVERIFY(bringUpCore(fake, QStringLiteral("/tmp/.runtime-a.yaml"), endpointAt(9090)));

        coordinator.scheduleReload();
        QVERIFY(coordinator.isReloadScheduled());

        // The core goes away while the debounce is still counting.
        fake.stop();
        QVERIFY(fake.releaseChildExit());
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Stopped);
        QVERIFY(!fake.isManagedCoreActive());

        QSignalSpy stops(&coordinator, &RuntimeCoordinator::coreStopRequested);
        QVERIFY(drainPast(RuntimeCoordinator::kReloadDebounceMs * 3));
        QCOMPARE(configs.requests, 0);
        QCOMPARE(stops.size(), 0);
        QVERIFY(!coordinator.isReloadScheduled());
    }

    // --- group B, point 4. main.cpp:143-147.

    void aReloadWithNothingSelectedStopsTheCoreInsteadOfRegenerating() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);
        QVERIFY(bringUpCore(fake, QStringLiteral("/tmp/.runtime-a.yaml"), endpointAt(9090)));

        QSignalSpy stops(&coordinator, &RuntimeCoordinator::coreStopRequested);
        QSignalSpy reloads(&coordinator, &RuntimeCoordinator::reloadRequested);
        configs.selection.clear();
        coordinator.scheduleReload();

        QTRY_COMPARE_WITH_TIMEOUT(stops.size(), 1, kDeadlineMs);
        QCOMPARE(reloads.size(), 0);
        QCOMPARE(configs.requests, 0);
        QCOMPARE(fake.state(), CoreState::Stopping);
    }

    // --- group B, point 5. main.cpp:156-159.

    void anUpdateToAnotherProfileDoesNotReload() {
        FakeBackend fake;
        StubConfigSource configs;
        configs.selection = QStringLiteral("profile-a");
        RuntimeCoordinator coordinator(fake, configs);
        QVERIFY(bringUpCore(fake, QStringLiteral("/tmp/.runtime-a.yaml"), endpointAt(9090)));

        coordinator.onProfileUpdated(QStringLiteral("profile-b"));
        QVERIFY(!coordinator.isReloadScheduled());

        coordinator.onProfileUpdated(QStringLiteral("profile-a"));
        QVERIFY(coordinator.isReloadScheduled());
        QTRY_COMPARE_WITH_TIMEOUT(configs.requests, 1, kDeadlineMs);
    }

    // --- group B, point 6. main.cpp:129-134. The core is started from the
    //     config-ready handler and from nowhere else.

    void generatingTheConfigurationIsTheStartAction() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);
        QSignalSpy starts(&coordinator, &RuntimeCoordinator::coreStartRequested);

        const QString path = QStringLiteral("/tmp/.runtime-a.yaml");
        fake.mapConfigFile(path, endpointAt(9090));
        coordinator.onRuntimeConfigReady(path);

        QCOMPARE(starts.size(), 1);
        QCOMPARE(starts.first().first().toString(), path);
        QVERIFY(fake.isValidating());
        QCOMPARE(fake.validatingConfigPath(), path);
    }

    // --- group B, point 7. main.cpp:280-283.

    void autostartIsDeferredIntoTheEventLoopAndNeedsASelection() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);

        configs.selection.clear();
        QCOMPARE(coordinator.requestAutostart(), false);
        QVERIFY(drainPostedTimers());
        QCOMPARE(configs.requests, 0);

        configs.selection = QStringLiteral("profile-a");
        QCOMPARE(coordinator.requestAutostart(), true);
        // Deferred: nothing has happened yet on the composition-root stack.
        QCOMPARE(configs.requests, 0);
        QTRY_COMPARE_WITH_TIMEOUT(configs.requests, 1, kDeadlineMs);
    }

    // --- group B, point 8 and group C, point 1, in one delivery.
    //     main.cpp:111-112 (the managed-to-attached handoff) then
    //     main.cpp:127-128 (the retention pass), in that order.

    void aReadyCoreIsAttachedToBeforeItsRetiredSnapshotIsDiscarded() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);

        const QString first = environment_->filePath(QStringLiteral(".runtime-first.yaml"));
        const QString second = environment_->filePath(QStringLiteral(".runtime-second.yaml"));
        QVERIFY(touch(first));
        QVERIFY(touch(second));
        fake.mapConfigFile(first, endpointAt(9090));
        fake.mapConfigFile(second, endpointAt(9191));

        coordinator.onRuntimeConfigReady(first);
        QVERIFY(fake.completeValidation(true));
        fake.advanceTime(fake.timings().probeIntervalMs);
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Running);
        QCOMPARE(fake.currentEndpoint().port, quint16(9090));

        // The endpoint at the moment the retention decision is taken. If the
        // prune ran first this reads the OLD port.
        quint16 portWhenPruned = 0;
        connect(&coordinator, &RuntimeCoordinator::snapshotsDiscarded, this,
                [&fake, &portWhenPruned] { portWhenPruned = fake.currentEndpoint().port; });

        coordinator.onRuntimeConfigReady(second);
        QVERIFY(fake.completeValidation(true));
        QCOMPARE(fake.state(), CoreState::Stopping);
        QVERIFY(fake.releaseChildExit());
        fake.advanceTime(fake.timings().probeIntervalMs);
        QVERIFY(fake.flushEvents());

        QCOMPARE(fake.state(), CoreState::Running);
        QCOMPARE(fake.currentEndpoint().port, quint16(9191));
        QCOMPARE(portWhenPruned, quint16(9191));
        QCOMPARE(coordinator.trackedSnapshots(), QStringList{second});
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(first), kDeadlineMs);
        QVERIFY(QFile::exists(second));
    }

    // --- group C, point 2. THE case. activeConfigPaths() deliberately
    //     includes the configuration of a validation child that was cancelled
    //     and has not exited yet. Deleting it is a file-deletion race, not
    //     untidiness, so the set is asked for and never re-derived.

    void aCancelledValidationKeepsItsSnapshotUntilTheChildExits() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);

        const QString running = environment_->filePath(QStringLiteral(".runtime-running.yaml"));
        const QString cancelled = environment_->filePath(QStringLiteral(".runtime-cancelled.yaml"));
        QVERIFY(touch(running));
        QVERIFY(touch(cancelled));
        fake.mapConfigFile(running, endpointAt(9090));
        fake.mapConfigFile(cancelled, endpointAt(9191));

        coordinator.onRuntimeConfigReady(running);
        QVERIFY(fake.completeValidation(true));
        fake.advanceTime(fake.timings().probeIntervalMs);
        QVERIFY(fake.flushEvents());
        QCOMPARE(fake.state(), CoreState::Running);

        // A second configuration goes into validation, and is then cancelled by
        // a stop. The child has NOT exited.
        coordinator.onRuntimeConfigReady(cancelled);
        QVERIFY(fake.isValidating());
        fake.stop();
        QVERIFY(!fake.isValidating());
        QCOMPARE(fake.cancellingValidationPaths(), QStringList{cancelled});

        QSignalSpy discarded(&coordinator, &RuntimeCoordinator::snapshotsDiscarded);
        coordinator.pruneSnapshots();
        // Nothing may be deleted: both paths are still active.
        QCOMPARE(discarded.size(), 0);
        QVERIFY(QFile::exists(cancelled));
        QVERIFY(QFile::exists(running));
        QCOMPARE(coordinator.trackedSnapshots().size(), 2);

        // The cancelled validator finally exits; only now may its config go.
        QVERIFY(fake.completeCancelledValidation(cancelled));
        QVERIFY(!fake.activeConfigPaths().contains(cancelled));
        coordinator.pruneSnapshots();
        QCOMPARE(discarded.size(), 1);
        QCOMPARE(discarded.first().first().toStringList(), QStringList{cancelled});
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(cancelled), kDeadlineMs);
        QVERIFY(QFile::exists(running));
    }

    // --- group C, point 1. main.cpp:131.

    void onlyGeneratedSnapshotsAreEverTracked() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);
        fake.setValidationGate(Gate::Immediate);

        const QString userProfile = environment_->filePath(QStringLiteral("profile.yaml"));
        QVERIFY(touch(userProfile));
        fake.mapConfigFile(userProfile, endpointAt(9090));
        coordinator.onRuntimeConfigReady(userProfile);
        QVERIFY(coordinator.trackedSnapshots().isEmpty());

        QSignalSpy discarded(&coordinator, &RuntimeCoordinator::snapshotsDiscarded);
        coordinator.pruneAllSnapshots();
        QCOMPARE(discarded.size(), 0);
        // The user's own profile is not this component's to delete.
        QVERIFY(QFile::exists(userProfile));
    }

    // --- group C, point 3. main.cpp:121-125.

    void snapshotDeletionNeverRunsOnTheGuiThread() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);

        QMutex mutex;
        QStringList removed;
        std::atomic<int> removals{0};
        std::atomic<QThread *> removerThread{nullptr};
        // Only the leaf unlink is substituted. The DISPATCH is not
        // substitutable: prune always hands the batch to the global pool, so
        // this cannot pass while the deletion happens on the GUI thread.
        coordinator.setSnapshotRemover([&](const QString &path) {
            removerThread.store(QThread::currentThread());
            {
                QMutexLocker locker(&mutex);
                removed.append(path);
            }
            ++removals;
        });

        const QString snapshot = QStringLiteral("/tmp/.runtime-threaded.yaml");
        coordinator.onRuntimeConfigReady(snapshot);
        coordinator.pruneAllSnapshots();

        QTRY_COMPARE_WITH_TIMEOUT(removals.load(), 1, kDeadlineMs);
        QMutexLocker locker(&mutex);
        QCOMPARE(removed, QStringList{snapshot});
        QVERIFY(removerThread.load() != nullptr);
        QVERIFY(removerThread.load() != QThread::currentThread());
        QVERIFY(removerThread.load() != QCoreApplication::instance()->thread());
    }

    // --- group C, point 4. main.cpp:187-190.

    void theFinalPruneDiscardsEverythingAndIsIdempotent() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);

        const QString running = environment_->filePath(QStringLiteral(".runtime-final.yaml"));
        QVERIFY(touch(running));
        fake.mapConfigFile(running, endpointAt(9090));
        coordinator.onRuntimeConfigReady(running);
        QVERIFY(fake.completeValidation(true));
        fake.advanceTime(fake.timings().probeIntervalMs);
        QVERIFY(fake.flushEvents());

        // Still active: an ordinary prune must not touch it.
        QVERIFY(fake.activeConfigPaths().contains(running));
        QSignalSpy discarded(&coordinator, &RuntimeCoordinator::snapshotsDiscarded);
        coordinator.pruneSnapshots();
        QCOMPARE(discarded.size(), 0);

        QVERIFY(!coordinator.finalPruneDone());
        coordinator.pruneAllSnapshots();
        QVERIFY(coordinator.finalPruneDone());
        QCOMPARE(discarded.size(), 1);
        QCOMPARE(discarded.first().first().toStringList(), QStringList{running});
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(running), kDeadlineMs);

        // A second call discards nothing and dispatches nothing.
        coordinator.pruneAllSnapshots();
        QCOMPARE(discarded.size(), 1);
        QVERIFY(coordinator.trackedSnapshots().isEmpty());
    }

    // --- group B, point 3. main.cpp:132, :139 and :150, plus main.cpp:241's
    //     reload->stop(). The handshake app/lifecycle calls on quit.

    void shutdownStopsTheReloadAndTheStartActionButStillTracksSnapshots() {
        FakeBackend fake;
        StubConfigSource configs;
        RuntimeCoordinator coordinator(fake, configs);
        QVERIFY(bringUpCore(fake, QStringLiteral("/tmp/.runtime-a.yaml"), endpointAt(9090)));

        coordinator.scheduleReload();
        QVERIFY(coordinator.isReloadScheduled());

        coordinator.beginShutdown();
        QVERIFY(coordinator.isShuttingDown());
        // main.cpp:241.
        QVERIFY(!coordinator.isReloadScheduled());
        // main.cpp:139 and :150, even though the core is still Running.
        QVERIFY(fake.isManagedCoreActive());
        QCOMPARE(coordinator.canReload(), false);
        coordinator.scheduleReload();
        QVERIFY(!coordinator.isReloadScheduled());

        // main.cpp:132. A snapshot generated during shutdown is still a file
        // that has to be cleaned up, but no core is started from it.
        QSignalSpy starts(&coordinator, &RuntimeCoordinator::coreStartRequested);
        const QString late = QStringLiteral("/tmp/.runtime-late.yaml");
        coordinator.onRuntimeConfigReady(late);
        QCOMPARE(starts.size(), 0);
        QCOMPARE(coordinator.trackedSnapshots(), QStringList{late});

        // And autostart cannot resurrect it.
        QCOMPARE(coordinator.requestAutostart(), false);
        QVERIFY(drainPostedTimers());
        QCOMPARE(configs.requests, 0);
    }

  private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(RuntimeCoordinatorTest)
#include "runtime_coordinator_test.moc"
