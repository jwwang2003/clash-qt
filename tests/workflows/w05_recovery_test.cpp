// W05 - Recovery.
//
// docs/TEST_STRATEGY.md, "Complete workflows":
//
//   Exercise  Run core -> controller drops or child crashes -> attempt restart
//             -> switch profile -> quit
//   Outcome   State converges to the latest request, failure is reported,
//             shutdown is bounded with accurate cleanup status
//
// THREE FAILURES, THREE JOURNEYS. The exercise names two different faults -
// a controller that goes away and a child that dies - and they are not
// interchangeable. A dead child is a dead core; a dead controller is not. The
// second journey exists precisely to show that the application does not confuse
// them, because reporting a blip as a crash is how a working core gets torn
// down for nothing.
//
// WHAT DECIDES EACH CLAIM
//
//   "failure is reported"  the backend's own failure channel, with the
//       contract's error code - CoreExited for a child that died, a transport
//       failure for a controller that stopped answering - not merely "something
//       changed".
//   "state converges to the latest request"  the CONTENT of the configuration
//       the surviving core was launched from. Each profile carries a marker key
//       that survives generation, so "which profile is live" is read out of the
//       file the child was handed rather than inferred from the store that was
//       asked last.
//   "shutdown is bounded"  a child that refuses SIGTERM. The quit must still
//       complete, the escalation must happen inside the budget the backend
//       PUBLISHES, and no process may survive. A quit that only completes
//       because the child was cooperative proves nothing about the bound.
//   "accurate cleanup status"  wasLastStopConfirmed(), and the stop completion
//       it is derived from. backend-r2 section 6 forbids reporting an
//       unconfirmed stop as a success, so the journey asserts the value rather
//       than the absence of a warning.

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>

#include <memory>

#include "support/fake_core.h"
#include "support/loopback_server.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"
#include "workflows/composition_root_audit.h"
#include "workflows/workflow_support.h"

using testsupport::FakeCore;
using testsupport::LoopbackServer;
using testsupport::ScopedEnvironment;

namespace wf = workflows;
namespace cb = core::backend;

namespace {

// The compressed restore grace window. Only the SHIPPED value is asserted as a
// value (tests/app/runtime/routing_controller_test.cpp); this is what lets the
// behaviour behind it be driven inside a journey.
constexpr int kCompressedGraceMs = 40;

void scriptController(LoopbackServer &server) {
    using Reply = LoopbackServer::Reply;
    server.route("GET", "/version", Reply::json(R"({"version":"workflow-core-1.19.31"})"));
    server.route("GET", "/configs", Reply::json(R"({"mode":"rule","tun":{"enable":false}})"));
    server.route("GET", "/proxies",
                 Reply::json(R"({"proxies":{"GLOBAL":{"type":"Selector","now":"DIRECT","all":["DIRECT"]}}})"));
    server.route("GET", "/rules", Reply::json(R"({"rules":[{"type":"MATCH","payload":"","proxy":"DIRECT"}]})"));
}

}  // namespace

class W05RecoveryTest : public QObject {
    Q_OBJECT

  private slots:

    void init() {
        environment_ = std::make_unique<ScopedEnvironment>(QStringLiteral("w05"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }

    void cleanup() {
        if (!enginePath_.isEmpty()) {
            const int alive = wf::liveProcessesOf(enginePath_);
            QVERIFY2(alive <= 0,
                     qPrintable(QStringLiteral("%1 process(es) from %2 outlived the test")
                                    .arg(alive)
                                    .arg(enginePath_)));
        }
        const QString escape = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escape.isEmpty(), qPrintable(escape));
        environment_.reset();
        enginePath_.clear();
    }

    // --- the graph this journey assembles is the one the application ships ---
    //
    // wf::AssembledApp is a REBUILD of src/main.cpp's object graph, so every
    // assertion in this suite is about a copy of the composition root. This case
    // keeps the copy honest: it compares the wiring of the two files and fails
    // the whole suite when they disagree. Until it existed, deleting the
    // warningRaised -> QMessageBox connection from src/main.cpp - the one thing
    // standing between an unacknowledged warning and a quit that never completes
    // - left all five workflow suites green.

    void theHarnessStillMirrorsTheCompositionRoot() {
        const QString drift = wf::audit::compositionRootDrift();
        QVERIFY2(drift.isEmpty(), qPrintable(drift));
    }

    // --- the journey: a crash, a restart, a switch, a quit -------------------

    void aCrashedChildIsReportedTheRestartSucceedsAndTheLatestProfileWins() {
        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) QSKIP(qPrintable(skipReason(relay)));

        const QString engineDir = engineDirectory();
        // Re-created rather than re-scripted for the restart below: FakeCore's
        // run directives are cumulative, and re-installing over the same
        // directory and name gives the SAME binary path with a clean script, so
        // the journey restarts the same engine rather than switching engines
        // half way through.
        auto engine = std::make_unique<FakeCore>(engineDir);
        QVERIFY2(engine->isValid(), qPrintable(engine->errorString()));
        enginePath_ = engine->binaryPath();
        // Runs, is probed, becomes ready, and then dies when the journey says
        // so. The crash is released, never waited for.
        QVERIFY(engine->validationSucceeds()
                    .printsLine(QStringLiteral("[INFO] up"))
                    .waitsFor(QStringLiteral("die"))
                    .crashes()
                    .commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        wf::AssembledApp app(proxyLog, osProxy,
                             app::runtime::RestoreDelays{0, kCompressedGraceMs});
        app.bootstrap(engine->binaryPath());

        const QString alpha = createProfile(app, QStringLiteral("alpha"));
        const QString bravo = createProfile(app, QStringLiteral("bravo"));
        QVERIFY(!alpha.isEmpty());
        QVERIFY(!bravo.isEmpty());
        app.profiles->selectProfile(alpha);
        QCOMPARE(app.profiles->currentUid(), alpha);

        QString launched;
        QObject::connect(app.runtimeCoordinator.get(),
                         &app::runtime::RuntimeCoordinator::coreStartRequested,
                         app.runtimeCoordinator.get(),
                         [&launched](const QString &path) { launched = path; });

        // ---- run a core ---------------------------------------------------
        QVERIFY(app.runtimeCoordinator->requestAutostart());
        QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; }),
                 qPrintable(report(app, controller)));
        QVERIFY2(wf::readTextFile(launched).contains("alpha"),
                 "the running core was not launched from the selected profile");
        QCOMPARE(wf::liveProcessesOf(engine->binaryPath()), 1);

        // ---- the child crashes --------------------------------------------
        const int crashedGeneration = static_cast<int>(cb::number(app.backend->generation()));
        QVERIFY(engine->release(QStringLiteral("die")));
        QVERIFY2(wf::waitFor([&app] {
                     return app.backend->state() == cb::CoreState::Failed ||
                            app.backend->state() == cb::CoreState::Stopped;
                 }),
                 qPrintable(report(app, controller)));
        QVERIFY(app.backend->drainPendingEvents());
        QVERIFY2(!app.events.coreFailures.empty(), "a crashed child produced no failure");
        QCOMPARE(app.events.coreFailures.back().error.code, cb::ErrorCode::CoreExited);
        QVERIFY2(!app.events.coreFailures.back().error.message.isEmpty(),
                 "the reported failure carried no message");
        QVERIFY2(wf::awaitNoLiveProcess(engine->binaryPath()), "the crashed child was not reaped");
        // A crash invalidates outstanding work, which is what makes a stale
        // reply from before it impossible to accept.
        QVERIFY(static_cast<int>(cb::number(app.backend->generation())) > crashedGeneration);
        // The reload gate refuses to restart a core that is genuinely down; a
        // restart is an explicit user action, not an automatic retry.
        app.runtimeCoordinator->scheduleReload();
        QVERIFY2(!app.runtimeCoordinator->isReloadScheduled(),
                 "a dead core scheduled a reload of its own accord");

        // ---- attempt the restart (the toolbar's Start Core) ----------------
        engine = std::make_unique<FakeCore>(engineDir);
        QVERIFY2(engine->isValid(), qPrintable(engine->errorString()));
        QCOMPARE(engine->binaryPath(), enginePath_);
        QVERIFY(engine->validationSucceeds()
                    .printsLine(QStringLiteral("[INFO] up again"))
                    .runsForever()
                    .commit());
        launched.clear();
        app.profiles->requestRuntimeConfig();
        QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; }),
                 qPrintable(report(app, controller)));
        QVERIFY(wf::readTextFile(launched).contains("alpha"));

        // ---- switch profile, twice, inside the debounce window -------------
        // The second selection arrives before the first has been acted on. One
        // reload must happen and it must use the LATEST request - that is what
        // the 100 ms coalescing window is for, and asserting it here is the
        // difference between "a reload happened" and "the right one did".
        launched.clear();
        app.profiles->selectProfile(bravo);
        QVERIFY(app.runtimeCoordinator->isReloadScheduled());
        app.profiles->selectProfile(alpha);
        QVERIFY(app.runtimeCoordinator->isReloadScheduled());
        QCOMPARE(app.profiles->currentUid(), alpha);

        QVERIFY2(wf::waitFor([&launched] { return !launched.isEmpty(); }),
                 qPrintable(report(app, controller)));
        QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; }),
                 qPrintable(report(app, controller)));
        const QByteArray live = wf::readTextFile(launched);
        QVERIFY2(live.contains("alpha"),
                 "the core converged on an earlier request, not the latest one");
        QVERIFY2(!live.contains("bravo"),
                 "the superseded profile is the one that was launched");
        QCOMPARE(wf::liveProcessesOf(engine->binaryPath()), 1);

        // ---- quit ----------------------------------------------------------
        QVERIFY2(app.quit(), qPrintable(QStringLiteral("the quit never completed: %1 | %2")
                                            .arg(app.blockingReason(), report(app, controller))));
        QVERIFY(app.shutdown->isCoreStopped());
        QVERIFY2(app.shutdown->wasLastStopConfirmed(),
                 "the quit reported an unconfirmed stop as a success");
        QVERIFY2(app.shutdownWarnings.isEmpty(),
                 qPrintable(app.shutdownWarnings.join(QLatin1Char('\n'))));
        QVERIFY2(wf::awaitNoLiveProcess(engine->binaryPath()), "a child outlived the quit");
        QVERIFY(snapshots().isEmpty());
        // Nothing in this journey enabled the system proxy, so nothing but a
        // restore of what the application owns may have been attempted.
        QCOMPARE(proxyLog->count(wf::ProxyAction::Disable), 0);
        QCOMPARE(proxyLog->count(wf::ProxyAction::Enable), 0);
    }

    // --- a controller that goes away is NOT a core that died -----------------

    void aDroppedControllerIsReportedWithoutTearingDownTheRunningCore() {
        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) QSKIP(qPrintable(skipReason(relay)));

        FakeCore engine(engineDirectory());
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        QVERIFY(engine.validationSucceeds()
                    .printsLine(QStringLiteral("[INFO] up"))
                    .runsForever()
                    .commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        wf::AssembledApp app(proxyLog, osProxy,
                             app::runtime::RestoreDelays{0, kCompressedGraceMs});
        app.bootstrap(engine.binaryPath());
        const QString alpha = createProfile(app, QStringLiteral("alpha"));
        app.profiles->selectProfile(alpha);

        QVERIFY(app.runtimeCoordinator->requestAutostart());
        QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; }),
                 qPrintable(report(app, controller)));
        QVERIFY(wf::waitFor([&app] { return app.backend->isConnected(); }));
        QCOMPARE(app.routing->restoreCount(), 0);

        // The controller stops answering. The CHILD is untouched.
        relay.close();
        controller.close();
        app.backend->refreshVersion();
        QVERIFY2(wf::waitFor([&app] { return !app.backend->isConnected(); }),
                 "a controller that stopped answering was still reported as connected");
        QVERIFY(app.backend->drainPendingEvents());
        QVERIFY2(!app.events.failures.empty(), "the transport failure was never reported");
        QVERIFY2(!app.events.connected, "the observer was told the controller was still connected");
        // Not a crash: no failure of the managed core, and the core is still up.
        QVERIFY2(app.events.coreFailures.empty(),
                 "a dropped controller was reported as a failure of the managed core");
        QCOMPARE(app.backend->state(), cb::CoreState::Running);
        QCOMPARE(wf::liveProcessesOf(engine.binaryPath()), 1);

        // The grace window expires without the controller coming back, so the
        // proxy the application owns - and nothing else - is restored.
        QVERIFY(wf::drainPast(kCompressedGraceMs * 4));
        QCOMPARE(app.routing->restoreCount(), 1);
        QCOMPARE(proxyLog->count(wf::ProxyAction::Restore), 1);
        QCOMPARE(proxyLog->count(wf::ProxyAction::Disable), 0);

        QVERIFY2(app.quit(), qPrintable(QStringLiteral("the quit never completed: %1")
                                            .arg(app.blockingReason())));
        QVERIFY(app.shutdown->wasLastStopConfirmed());
        QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()),
                 "the core outlived a quit taken with its controller already gone");
    }

    // --- the shutdown bound --------------------------------------------------

    void aCoreThatRefusesToTerminateIsKilledAndTheQuitStillCompletes() {
        if (!FakeCore::terminationContract().terminateIsCooperative) {
            QSKIP("This platform's terminate() cannot be refused by the child, so a core that "
                  "ignores it is not expressible and the escalation cannot be driven.");
        }

        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) QSKIP(qPrintable(skipReason(relay)));

        FakeCore engine(engineDirectory());
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        QVERIFY(engine.validationSucceeds()
                    .ignoresTerminate()
                    .printsLine(QStringLiteral("[INFO] stubborn"))
                    .runsForever()
                    .commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        wf::AssembledApp app(proxyLog, osProxy);
        app.bootstrap(engine.binaryPath());
        // The budget the backend will REALLY apply, shrunk through the seam it
        // publishes it from. timings() reports what is set here, so the bound
        // asserted below is the bound the application advertises.
        core::CoreTimings timings;
        timings.probeIntervalMs = 25;
        timings.probeTimeoutMs = 500;
        timings.terminateWaitMs = 400;
        app.backend->setTimings(timings);
        QCOMPARE(app.backend->timings().terminateWaitMs, 400u);

        const QString alpha = createProfile(app, QStringLiteral("alpha"));
        app.profiles->selectProfile(alpha);
        QVERIFY(app.runtimeCoordinator->requestAutostart());
        QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; }),
                 qPrintable(report(app, controller)));
        QCOMPARE(wf::liveProcessesOf(engine.binaryPath()), 1);

        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY2(app.quit(), qPrintable(QStringLiteral("a stubborn child wedged the quit: %1 | %2")
                                            .arg(app.blockingReason(), report(app, controller))));
        const qint64 took = elapsed.elapsed();

        QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()),
                 "a child that refused to terminate was never killed");
        QVERIFY2(app.shutdown->wasLastStopConfirmed(),
                 "the escalation ended the child but the stop was not reported as confirmed");
        QVERIFY2(app.shutdownWarnings.isEmpty(),
                 qPrintable(app.shutdownWarnings.join(QLatin1Char('\n'))));
        // The bound: the escalation cannot have been instant (the child refused
        // the polite request) and it must not have exceeded the published wait
        // by more than the event loop's own slack.
        QVERIFY2(took >= timings.terminateWaitMs,
                 qPrintable(QStringLiteral("the quit finished in %1 ms, inside the %2 ms "
                                           "termination wait: the child cannot have refused "
                                           "anything, so nothing was proven")
                                .arg(took)
                                .arg(timings.terminateWaitMs)));
        QVERIFY2(took < static_cast<qint64>(timings.terminateWaitMs) + 8000,
                 qPrintable(QStringLiteral("the quit took %1 ms against a %2 ms published "
                                           "termination wait: the shutdown is not bounded")
                                .arg(took)
                                .arg(timings.terminateWaitMs)));
        QVERIFY(snapshots().isEmpty());
    }

  private:
    QString engineDirectory() {
        const QString marker = environment_->filePath(QStringLiteral("engine/.keep"));
        const QString dir = QFileInfo(marker).absolutePath();
        QDir().mkpath(dir);
        return dir;
    }

    QString createProfile(wf::AssembledApp &app, const QString &marker) {
        const QStringList before = uids(app);
        if (!app.profiles->createLocalProfile(marker, QString::fromUtf8(
                                                          wf::directOnlyProfile(0, marker))))
            return {};
        for (const QString &uid : uids(app))
            if (!before.contains(uid)) return uid;
        return {};
    }

    static QStringList uids(wf::AssembledApp &app) {
        QStringList out;
        for (const auto &profile : app.profiles->profiles()) out << profile.uid;
        return out;
    }

    static QString report(wf::AssembledApp &app, LoopbackServer &controller) {
        return QStringLiteral("state=%1 connected=%2 | events: %3 | store: %4 | %5")
            .arg(static_cast<int>(app.backend->state()))
            .arg(app.backend->isConnected())
            .arg(app.events.transcript(), app.storeErrors.join(QLatin1Char(' ')),
                 controller.pendingReport());
    }

    static QString skipReason(const wf::ControllerRelay &relay) {
        return QStringLiteral(
                   "W05 needs the generated controller address 127.0.0.1:%1, which is in use: %2. "
                   "Not asserted rather than asserted weakly.")
            .arg(wf::kGeneratedControllerPort)
            .arg(relay.errorString());
    }

    QStringList snapshots() const {
        return QDir(environment_->dataDir())
            .entryList({QStringLiteral(".runtime-*")}, QDir::Files | QDir::Hidden);
    }

    std::unique_ptr<ScopedEnvironment> environment_;
    QString enginePath_;
};

QTEST_GUILESS_MAIN(W05RecoveryTest)
#include "w05_recovery_test.moc"
