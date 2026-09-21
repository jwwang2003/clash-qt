// Acceptance suite for app::backup::BackupCoordinator.
//
// Specification: PRE-ARCH section 4c, the table "Behaviour that must move, not
// be reinvented", plus the shutdown consumption in section 4b group E
// (main.cpp:185, 228-229, 241).
//
// The point of the package is that the backup store exists because this
// coordinator exists, not because a page happened to be built - so the first
// case builds the real store with no widget anywhere, and the last one drives a
// real ShutdownCoordinator gate off it.
//
// Nothing here sleeps. Every step of a restore is held and released by hand.

#include <QtTest>

#include <memory>
#include <utility>

#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include "app/backup/backup_coordinator.h"
#include "app/backup/backup_store_session.h"
#include "app/lifecycle/shutdown_coordinator.h"
#include "app/lifecycle/shutdown_ports.h"
#include "core/backups/backup_store.h"
#include "support/backend/fake_backend.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"
#include "fake_backup_session.h"

using app::backup::BackupCoordinator;
using app::backup::BackupStoreSession;
using app::backup::StoreHooks;
using app::lifecycle::ShutdownCoordinator;
using app::lifecycle::ShutdownState;
using app::lifecycle::SystemProxyShutdown;
using app::lifecycle::TaskDrain;

using core::backend::BackendTimings;
using core::backend::CoreState;
using core::backend::Endpoint;

using testsupport::ScopedEnvironment;
using testsupport::backup::FakeBackupSession;
using testsupport::backend::FakeBackend;

namespace {

constexpr BackendTimings kTimings{};

Endpoint endpointAt(const char *host, quint16 port) {
    Endpoint endpoint;
    endpoint.host = QString::fromLatin1(host);
    endpoint.port = port;
    return endpoint;
}

bool bringUpCore(FakeBackend &fake,
                 const QString &configPath = QStringLiteral("/tmp/.runtime-1.yaml")) {
    fake.mapConfigFile(configPath, endpointAt("127.0.0.1", 9090));
    fake.start(configPath, QStringLiteral("/work"));
    if (!fake.completeValidation(true)) return false;
    fake.advanceTime(kTimings.probeIntervalMs);
    fake.flushEvents();
    return fake.state() == CoreState::Running;
}

class FakeShutdownState final : public ShutdownState {
  public:
    bool isShuttingDown() const noexcept override { return shuttingDown; }
    bool shuttingDown = false;
};

class HeldProxy final : public SystemProxyShutdown {
  public:
    void requestShutdown() override { ++requests; }
    int requests = 0;
};

class CountingDrain final : public TaskDrain {
  public:
    int activeTaskCount() const override { return count; }
    int count = 0;
};

// ProfileStore / ConfigEnhancer, as the coordinator consumes them.
struct FakeStore {
    bool fileBusy = false;
    bool maintenance = false;
    int reloads = 0;
    QStringList *log = nullptr;
    QString name;

    StoreHooks hooks() {
        StoreHooks hooks;
        hooks.setMaintenanceMode = [this](bool enabled) { maintenance = enabled; };
        hooks.isFileBusy = [this] { return fileBusy; };
        hooks.reload = [this] {
            ++reloads;
            if (log) log->append(name);
        };
        return hooks;
    }
};

}  // namespace

class BackupCoordinatorTest : public QObject {
    Q_OBJECT

  private slots:

    void initTestCase() {
        environment_ = std::make_unique<ScopedEnvironment>(QStringLiteral("bkco"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }

    void cleanupTestCase() {
        // The developer's own preference store must be byte-identical to what
        // it was when this suite started.
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
    }

    // --- PRE-ARCH 4c. The store is CONSTRUCTED AND OWNED here, not discovered
    //     with window.findChildren<core::BackupStore *>(). There is no widget
    //     in this test, and there is still a store.

    void theStoreIsOwnedRatherThanDiscoveredInTheWidgetTree() {
        FakeBackend fake;
        auto session = std::make_unique<BackupStoreSession>(environment_->dataDir());
        auto *sessionPtr = session.get();
        FakeStore profiles;
        FakeStore enhancer;
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks());

        core::BackupStore *store = coordinator.store();
        QVERIFY(store != nullptr);
        QCOMPARE(store, sessionPtr->store());
        // Its parent is the session, not a page: a lazily built BackupPage can
        // no longer make the store disappear.
        QCOMPARE(store->parent(), static_cast<QObject *>(sessionPtr));
        QVERIFY(!coordinator.isBusy());
    }

    // --- backup_page.cpp:30. Without requirePreparation(true) the worker runs
    //     straight through and the pending-write gate never gets to hold it.

    void preparationIsRequiredFromTheStart() {
        FakeBackend fake;
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        FakeStore profiles;
        FakeStore enhancer;
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks());
        QVERIFY(fakeSession->preparationRequired);
    }

    // --- THE PENDING-SAVE GATE, backup_page.cpp:155-166. Preparation continues
    //     only once BOTH stores have finished writing. Held, proved to block,
    //     released one at a time.

    void preparationWaitsUntilBothStoresHaveFinishedWriting() {
        FakeBackend fake;
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        FakeStore profiles;
        FakeStore enhancer;
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks());
        QSignalSpy status(&coordinator, &BackupCoordinator::statusMessage);

        profiles.fileBusy = true;
        enhancer.fileBusy = true;

        fakeSession->beginOperation();
        QVERIFY(coordinator.isWaitingForFiles());
        QCOMPARE(fakeSession->preparationsContinued, 0);
        QCOMPARE(status.count(), 1);
        QVERIFY(status.at(0).at(0).toString().contains(QStringLiteral("pending profile changes")));

        // One store idle is not enough.
        profiles.fileBusy = false;
        coordinator.onFileBusyChanged();
        QCOMPARE(fakeSession->preparationsContinued, 0);
        QVERIFY(coordinator.isWaitingForFiles());

        // The other store, held alone, blocks just as hard.
        profiles.fileBusy = true;
        enhancer.fileBusy = false;
        coordinator.onFileBusyChanged();
        QCOMPARE(fakeSession->preparationsContinued, 0);

        profiles.fileBusy = false;
        coordinator.onFileBusyChanged();
        QCOMPARE(fakeSession->preparationsContinued, 1);
        QVERIFY(!coordinator.isWaitingForFiles());

        // And a further notification does not release it a second time.
        coordinator.onFileBusyChanged();
        QCOMPARE(fakeSession->preparationsContinued, 1);
    }

    void preparationContinuesImmediatelyWhenNothingIsPending() {
        FakeBackend fake;
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        FakeStore profiles;
        FakeStore enhancer;
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks());

        fakeSession->beginOperation();
        QCOMPARE(fakeSession->preparationsContinued, 1);
        QVERIFY(!coordinator.isWaitingForFiles());
    }

    // --- THE RESTORE PREPARATION, backup_page.cpp:170-185. The core is stopped
    //     first, and the restore continues only when the stop is observed.

    void restorePreparationStopsTheCoreBeforeContinuing() {
        FakeBackend fake;
        QVERIFY(bringUpCore(fake));
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        FakeStore profiles;
        FakeStore enhancer;
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks());
        QSignalSpy status(&coordinator, &BackupCoordinator::statusMessage);
        QSignalSpy progress(&coordinator, &BackupCoordinator::restoreProgressMessage);

        fakeSession->beginOperation(true);
        fakeSession->prepareRestore();

        // HELD: the core is stopping and has not exited. Nothing may be written
        // over the files it is still reading.
        QVERIFY(coordinator.isWaitingForCore());
        QCOMPARE(fake.state(), CoreState::Stopping);
        QVERIFY(fakeSession->restoreApprovals.empty());
        QVERIFY(status.count() >= 1);
        QVERIFY(status.last().at(0).toString().contains(QStringLiteral("Stopping the core")));

        // RELEASED.
        QVERIFY(fake.releaseChildExit());
        fake.flushEvents();
        QVERIFY(!coordinator.isWaitingForCore());
        QCOMPARE(fakeSession->restoreApprovals.size(), std::size_t{1});
        QVERIFY(fakeSession->restoreApprovals.front());
        QCOMPARE(progress.count(), 1);
        QVERIFY(progress.at(0).at(0).toString().contains(QStringLiteral("Restoring files")));
    }

    // --- backup_page.cpp:175-178. An already-stopped core will never deliver a
    //     stop event, so without the synchronous fast path the restore waits
    //     forever. This is the case that would silently hang.

    void anAlreadyStoppedCoreTakesTheSynchronousFastPath() {
        FakeBackend fake;
        QCOMPARE(fake.state(), CoreState::Stopped);
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        FakeStore profiles;
        FakeStore enhancer;
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks());

        fakeSession->beginOperation(true);
        fakeSession->prepareRestore();

        QCOMPARE(fakeSession->restoreApprovals.size(), std::size_t{1});
        QVERIFY(fakeSession->restoreApprovals.front());
        QVERIFY(!coordinator.isWaitingForCore());

        // The queued stop events still arrive; they must not approve a second
        // restore.
        fake.flushEvents();
        QCOMPARE(fakeSession->restoreApprovals.size(), std::size_t{1});
    }

    // --- backup_page.cpp:181. A core stop that nobody asked for must not
    //     resume a restore that was never prepared.

    void anUnrelatedCoreStopDoesNotResumeARestore() {
        FakeBackend fake;
        QVERIFY(bringUpCore(fake));
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        FakeStore profiles;
        FakeStore enhancer;
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks());

        fake.stop();
        QVERIFY(fake.releaseChildExit());
        fake.flushEvents();

        QVERIFY(fakeSession->restoreApprovals.empty());
        QVERIFY(!coordinator.isWaitingForCore());
    }

    // --- THE MAINTENANCE GATE, backup_page.cpp:137-140. Maintenance mode is on
    //     while an operation runs OR while the application is shutting down -
    //     the second half is the "shuttingDown" property read, now an explicit
    //     ShutdownState query.

    void maintenanceModeFollowsTheOperationAndTheShutdown() {
        FakeBackend fake;
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        FakeStore profiles;
        FakeStore enhancer;
        FakeShutdownState shutdown;
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks(), &shutdown);

        fakeSession->beginOperation();
        QVERIFY(profiles.maintenance);
        QVERIFY(enhancer.maintenance);
        QVERIFY(coordinator.isMaintenanceActive());

        fakeSession->finishOperation();
        QVERIFY(!profiles.maintenance);
        QVERIFY(!enhancer.maintenance);

        // Shutting down: an operation that ends must NOT re-enable writing.
        shutdown.shuttingDown = true;
        fakeSession->beginOperation();
        fakeSession->finishOperation();
        QVERIFY(profiles.maintenance);
        QVERIFY(enhancer.maintenance);
        QVERIFY(coordinator.isMaintenanceActive());
    }

    // --- backup_page.cpp:149-153. An operation that ends clears both waiting
    //     flags, so a cancelled restore does not leave the coordinator waiting
    //     for a core stop that will never be relevant.

    void endingAnOperationClearsTheWaitingFlags() {
        FakeBackend fake;
        QVERIFY(bringUpCore(fake));
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        FakeStore profiles;
        FakeStore enhancer;
        profiles.fileBusy = true;
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks());

        fakeSession->beginOperation(true);
        QVERIFY(coordinator.isWaitingForFiles());
        fakeSession->prepareRestore();
        QVERIFY(coordinator.isWaitingForCore());

        fakeSession->finishOperation(true);
        QVERIFY(!coordinator.isWaitingForFiles());
        QVERIFY(!coordinator.isWaitingForCore());
    }

    // --- backup_page.cpp:186-189. The enhancement chain is reloaded BEFORE the
    //     profiles, because ProfileStore::load() composes against the chain.

    void aRestoreReloadsTheEnhancerBeforeTheProfiles() {
        FakeBackend fake;
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        QStringList order;
        FakeStore profiles;
        FakeStore enhancer;
        profiles.log = &order;
        profiles.name = QStringLiteral("profiles");
        enhancer.log = &order;
        enhancer.name = QStringLiteral("enhancer");
        BackupCoordinator coordinator(std::move(session), fake, profiles.hooks(),
                                      enhancer.hooks());

        fakeSession->reportRestored();

        QCOMPARE(order, (QStringList{QStringLiteral("enhancer"), QStringLiteral("profiles")}));
    }

    // --- main.cpp:185, 228-229, 241 - what app/lifecycle consumes. The quit
    //     gate reads the owned store, is notified by it, and cancels it,
    //     without a window, a page or a findChildren call anywhere.

    void theQuitGateUsesTheOwnedStoreWithoutAWidgetTree() {
        FakeBackend fake;
        HeldProxy proxy;
        CountingDrain drain;
        auto session = std::make_unique<FakeBackupSession>();
        auto *fakeSession = session.get();
        FakeStore profiles;
        FakeStore enhancer;

        ShutdownCoordinator shutdown(fake, proxy, drain);
        BackupCoordinator backups(std::move(session), fake, profiles.hooks(), enhancer.hooks(),
                                  &shutdown);

        shutdown.addBusyGate(QStringLiteral("backup"), [&backups] { return backups.isBusy(); });
        shutdown.addQuitAction(QStringLiteral("backups"), [&backups] { backups.cancel(); });
        connect(&backups, &BackupCoordinator::busyChanged, &shutdown,
                &ShutdownCoordinator::reevaluate);

        fakeSession->beginOperation();
        QVERIFY(backups.isBusy());

        shutdown.requestQuit();
        QCOMPARE(fakeSession->cancels, 1);
        shutdown.onProxyShutdownFinished(true, {});
        fake.flushEvents();

        // HELD by the backup operation, which is exactly the gate main.cpp:185
        // would have skipped had the store not been found.
        QCOMPARE(shutdown.blockingReason(), QStringLiteral("backup"));
        QVERIFY(!shutdown.isQuitApproved());

        // The store's own notification drives the re-evaluation; no poll tick
        // is involved.
        fakeSession->finishOperation();
        QCOMPARE(shutdown.blockingReason(), QString());
        QVERIFY(shutdown.isQuitApproved());
    }

  private:
    std::unique_ptr<ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(BackupCoordinatorTest)
#include "backup_coordinator_test.moc"
