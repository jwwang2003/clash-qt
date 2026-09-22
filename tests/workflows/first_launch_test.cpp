// First launch - a complete journey.
//
//   Exercise  Isolated empty workspace -> import local profile -> start ->
//             inspect status -> stop -> quit -> reopen
//   Outcome   Correct profile persists, readiness is real, owned child exits,
//             application remains usable
//
// WHAT MAKES THIS A JOURNEY RATHER THAN A LONGER UNIT TEST
//
// Every object is the production one. The profile store writes real YAML to a
// real directory and generates the runtime configuration on a real worker; the
// backend validates that configuration in a real child process and launches a
// real child from it; readiness is a real HTTP GET /version over a real socket;
// the quit runs the real ShutdownCoordinator sequence. Nothing is substituted
// except the two things the isolation rules forbid a test from touching - the
// OS proxy command and the privileged helper - and neither participates in
// this journey at all.
//
// THE FOUR CLAIMS, AND WHAT DECIDES EACH
//
//   "the correct profile persists"  a SECOND ProfileStore, constructed after
//       the first graph is destroyed, over the same data directory. The
//       assertion is the bytes on disk and the selected uid, not a field of the
//       object that wrote them.
//   "readiness is real"  the controller's /version is HELD. The child process
//       is observably alive - pgrep, not the backend's own opinion - while the
//       core is still only Starting and no endpoint has been published. The
//       gate is then released and the same core becomes Running. A backend that
//       equated "the process started" with "ready" passes the second half and
//       fails the first.
//   "the owned child exits"  pgrep again, for the unique path this journey
//       copied its fake core to. The backend saying Stopped is not evidence
//       about a process.
//   "the application remains usable"  the reopened graph starts a core again
//       and quits again, and the quit gate reports no blocking reason.
//
// THE ENGINE IS LOADED, NOT LINKED. The supervisor ships as a separately built
// shared library, so this journey drives clashqt::integration::ModuleBackend
// over a session clashqt::integration::ModuleLoader created from
// CLASH_QT_MODULE_PATH - the same two classes src/main.cpp uses, against the
// same shipping module id.
// Nothing in this file names core/mihomo/** any more, and there is no
// setTimings(): the readiness deadline this journey waits out below is the 10
// seconds the module PUBLISHES through timings(), not a shrunken test value.
//
// THE FIXED CONTROLLER PORT. core::ProfileStore overwrites external-controller
// in every configuration it generates, so a journey that starts a core through
// it cannot choose where that core must answer. workflows::ControllerRelay
// bridges the generated address onto the loopback fixture's ephemeral port; if
// the address is already taken - the developer's own core is the usual reason -
// the journey SKIPS and says so, because half a journey is not evidence.

#include <QtTest>

#include <QDir>
#include <QSignalSpy>
#include <QVector>

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

constexpr auto kVersionBody = R"({"version":"workflow-core-1.19.31","premium":false})";

void scriptController(LoopbackServer &server) {
    using Reply = LoopbackServer::Reply;
    server.route("GET", "/version", Reply::json(kVersionBody));
    server.route("GET", "/configs",
                 Reply::json(R"({"mode":"rule","mixed-port":27890,"tun":{"enable":false}})"));
    server.route("GET", "/proxies",
                 Reply::json(R"({"proxies":{"GLOBAL":{"type":"Selector","now":"DIRECT","all":["DIRECT"]},"DIRECT":{"type":"Direct","now":""}}})"));
    server.route("GET", "/rules", Reply::json(R"({"rules":[{"type":"MATCH","payload":"","proxy":"DIRECT"}]})"));
}

}  // namespace

class FirstLaunchTest : public QObject {
    Q_OBJECT

  private slots:

    void init() {
        environment_ = std::make_unique<ScopedEnvironment>(QStringLiteral("first-launch"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }

    void cleanup() {
        // Order matters: the engine directory is deleted with the scoped
        // environment, so nothing may still be running out of it.
        if (!enginePath_.isEmpty()) {
            const int alive = wf::liveProcessesOf(enginePath_);
            QVERIFY2(alive <= 0, qPrintable(QStringLiteral(
                         "%1 process(es) started from %2 outlived the test")
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

    // ------------------------------------------------------------- the journey

    void firstLaunchImportsStartsInspectsStopsQuitsAndReopens() {
        LoopbackServer controller;
        // The empty secret disables the fixture's bearer check. The secret the
        // profile store generates is random per data directory and is not known
        // until the first configuration is generated - by which time the
        // readiness probe is already in flight. Controller authentication is
        // covered by tests/core/controller_test.cpp; this journey is about the
        // trip, and a half-scripted auth check here would only be decoration.
        QVERIFY(controller.listen(QString()));
        scriptController(controller);

        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) {
            QSKIP(qPrintable(QStringLiteral(
                "This journey needs the generated controller address 127.0.0.1:%1, which is in "
                "use: %2. A running clash-qt core is the usual reason. Not asserted rather "
                "than asserted weakly.")
                             .arg(wf::kGeneratedControllerPort)
                             .arg(relay.errorString())));
        }

        // Held from the start, because the readiness claim below needs the FIRST
        // probe parked. `autoRelease` disarms it once that claim is made; a gate
        // cannot be unregistered, and every later /version in this journey is an
        // ordinary request.
        auto *held = controller.hold("GET", "/version");
        bool autoRelease = false;
        QObject::connect(held, &LoopbackServer::Gate::pendingChanged, held,
                         [held, &autoRelease](int pending) {
                             if (!autoRelease || pending <= 0) return;
                             QTimer::singleShot(0, held, [held] { held->release(); });
                         });

        const QString engineDir = environment_->filePath(QStringLiteral("engine/.keep"));
        QDir().mkpath(QFileInfo(engineDir).absolutePath());
        FakeCore engine(QFileInfo(engineDir).absolutePath());
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        QVERIFY(engine.validationSucceeds()
                    .printsLine(QStringLiteral("[INFO] fake core up"))
                    .runsForever()
                    .commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();

        // ---- 1. an isolated, empty workspace -----------------------------
        {
            auto app = std::make_unique<wf::AssembledApp>(proxyLog, osProxy);
            // The boundary first: an inert graph would fail every later
            // assertion with the wrong reason.
            QVERIFY2(app->moduleLoaded(), qPrintable(app->moduleError));
            QCOMPARE(app->moduleArtifact(), wf::moduleArtifactPath());
            app->bootstrap(engine.binaryPath());
            QVERIFY2(app->startupErrors.isEmpty(),
                     qPrintable(app->startupErrors.join(QLatin1Char('\n'))));
            QVERIFY2(app->profiles->profiles().isEmpty(), "the workspace was not empty");
            QVERIFY(app->profiles->currentUid().isEmpty());
            // Nothing can start without a selection - that gate is the
            // coordinator's, and it is what makes the empty case usable rather
            // than broken.
            QVERIFY2(!app->runtimeCoordinator->requestAutostart(),
                     "an empty workspace scheduled a core start");
            QVERIFY(wf::drainPostedTimers());
            QCOMPARE(app->backend->state(), cb::CoreState::Stopped);
            QCOMPARE(engine.invocationCount(), 0);

            // ---- 2. import a LOCAL profile -------------------------------
            const QString source = environment_->filePath(QStringLiteral("import/home.yaml"));
            QVERIFY(wf::writeTextFile(source, profileBody()));
            QSignalSpy imported(app->profiles.get(), &core::ProfileStore::profilesChanged);
            app->profiles->importFromFile(source);
            QVERIFY2(app->storeErrors.isEmpty(),
                     qPrintable(app->storeErrors.join(QLatin1Char('\n'))));
            QCOMPARE(imported.size(), 1);
            QCOMPARE(app->profiles->profiles().size(), 1);
            profileUid_ = app->profiles->currentUid();
            QVERIFY2(!profileUid_.isEmpty(), "the first imported profile was not selected");
            QCOMPARE(app->profiles->profiles().first().name, QStringLiteral("home"));
            // The imported bytes, not the store's opinion of them.
            QCOMPARE(wf::readTextFile(app->profiles->profiles().first().filePath), profileBody());

            // ---- 3. start, and prove readiness is REAL -------------------
            QString launchedConfig;
            QObject::connect(app->runtimeCoordinator.get(),
                             &app::runtime::RuntimeCoordinator::coreStartRequested,
                             app->runtimeCoordinator.get(),
                             [&launchedConfig](const QString &path) { launchedConfig = path; });

            QVERIFY2(app->runtimeCoordinator->requestAutostart(),
                     "a selected profile did not schedule a start");
            QVERIFY(wf::waitFor([&launchedConfig] { return !launchedConfig.isEmpty(); }));
            QVERIFY2(QFileInfo::exists(launchedConfig),
                     "the coordinator started a core from a configuration that does not exist");
            // The protected controller field, read back out of the generated
            // file. If production stops writing 29097 this fails HERE, with the
            // reason, rather than as an unexplained readiness timeout.
            QCOMPARE(wf::controllerPortOf(launchedConfig), wf::kGeneratedControllerPort);

            // Validation runs in its own child; the launch is the second.
            QVERIFY(wf::waitFor([&engine] { return engine.invocationCount() >= 2; }));
            QVERIFY2(wf::waitFor([&held] { return held->pending() >= 1; }),
                     qPrintable(QStringLiteral("no readiness probe arrived. %1")
                                    .arg(controller.pendingReport())));

            // THE CLAIM. A live child, a probe in flight, and the core is still
            // only Starting: readiness is the controller's answer, not the
            // process's existence.
            QCOMPARE(wf::liveProcessesOf(engine.binaryPath()), 1);
            QCOMPARE(app->backend->state(), cb::CoreState::Starting);
            QVERIFY(app->backend->drain());
            QVERIFY2(app->events.readyEndpoints.empty(),
                     "a core reported ready while its readiness probe was still held");
            QVERIFY2(!cb::isValid(app->backend->managedEndpoint()),
                     "an endpoint was published before the controller answered");

            // The gate stays registered for the path for the life of the
            // server, so every LATER /version - the readiness retries, the
            // refreshState() that follows the attach, and the whole reopened
            // launch below - would be parked too. Arming the auto-release turns
            // the hold back into an ordinary route from here on. A release, not
            // a sleep: nothing waits on a duration.
            autoRelease = true;
            held->release();
            QVERIFY2(wf::waitFor([&app] {
                         return app->backend->state() == cb::CoreState::Running;
                     }),
                     qPrintable(QStringLiteral("the core never became ready. %1 | %2")
                                    .arg(controller.pendingReport(), app->events.transcript())));
            QVERIFY(app->backend->drain());
            QCOMPARE(static_cast<int>(app->events.readyEndpoints.size()), 1);
            QCOMPARE(app->events.readyEndpoints.front().port, wf::kGeneratedControllerPort);

            // ---- 4. inspect status ---------------------------------------
            // The managed-to-attached handoff RuntimeCoordinator performs on
            // coreReady: the controller the application is talking to is now
            // its own child's.
            QVERIFY2(wf::waitFor([&app] { return app->backend->isConnected(); }),
                     qPrintable(controller.pendingReport()));
            QCOMPARE(app->backend->attachmentOwnership(), cb::Ownership::Managed);
            QVERIFY2(!app->backend->isExternalControllerConnected(),
                     "the managed child was reported as an external controller");
            QCOMPARE(app->backend->ownership(), cb::Ownership::Managed);

            app->backend->refreshVersion();
            QVERIFY(wf::waitFor([&app] { return !app->events.versions.empty(); }));
            QCOMPARE(app->events.versions.back(), QStringLiteral("workflow-core-1.19.31"));
            QVERIFY2(!controller.sawUnexpectedRequest(),
                     qPrintable(controller.redactedTranscript()));
            // Everything above crossed the module boundary. A host that did not
            // understand an event the module sent would have dropped it
            // silently, so the count is asserted rather than assumed: with a
            // matching handshake it can only be zero.
            QCOMPARE(app->unknownModuleEvents(), std::uint64_t(0));

            // The snapshot the coordinator is retaining is the running core's,
            // and the backend agrees it must not be deleted yet.
            QCOMPARE(app->runtimeCoordinator->trackedSnapshots(), QStringList{launchedConfig});
            QVERIFY(app->backend->activeConfigPaths().contains(launchedConfig));

            // ---- 5. stop --------------------------------------------------
            app->backend->stop();
            QVERIFY2(wf::waitFor([&app] { return !app->events.stops.empty(); }),
                     qPrintable(app->events.transcript()));
            QVERIFY2(app->events.stops.back().confirmed,
                     "the stop was not confirmed: the child's exit was never observed");
            QCOMPARE(app->events.stops.back().status, cb::CompletionStatus::Ok);
            QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()),
                     "the owned child outlived a confirmed stop");

            // ---- 6. quit --------------------------------------------------
            QVERIFY2(app->quit(), qPrintable(QStringLiteral("the quit never completed: %1")
                                                 .arg(app->blockingReason())));
            QVERIFY(app->shutdownStarted);
            QVERIFY2(app->shutdownWarnings.isEmpty(),
                     qPrintable(app->shutdownWarnings.join(QLatin1Char('\n'))));
            QVERIFY(app->quitGuard->isApproved());
            QVERIFY(app->shutdown->isCoreStopped());
            QVERIFY(app->shutdown->wasLastStopConfirmed());
            QVERIFY(app->shutdown->isSystemProxyStopped());
            QCOMPARE(app->blockingReason(), QString());
            // The final prune ran and nothing it queued is still running.
            QVERIFY(app->shutdown->isFinalCleanupDone());
            QVERIFY(wf::waitFor([&app, &launchedConfig] {
                return !QFileInfo::exists(launchedConfig);
            }));
            QVERIFY2(snapshotsIn(app->profiles->dataDir()).isEmpty(),
                     "a runtime snapshot survived the quit");
            // This journey never enabled the system proxy, so the shutdown must
            // not have changed anything the application does not own.
            QCOMPARE(proxyLog->count(wf::ProxyAction::Enable), 0);
            QCOMPARE(proxyLog->count(wf::ProxyAction::Disable), 0);
        }

        // ---- 7. reopen ---------------------------------------------------
        // A new process would construct a new graph over the same directory.
        // This is that graph; the first one is gone, including its ProfileStore.
        {
            auto app = std::make_unique<wf::AssembledApp>(proxyLog, osProxy);
            // A second load of the same artifact in the same process: the first
            // graph released its session and unmapped the library, so this is
            // also the evidence that the loader's lifetime accounting lets a
            // module be taken twice.
            QVERIFY2(app->moduleLoaded(), qPrintable(app->moduleError));
            app->bootstrap(engine.binaryPath());
            QVERIFY2(app->startupErrors.isEmpty(),
                     qPrintable(app->startupErrors.join(QLatin1Char('\n'))));

            QCOMPARE(app->profiles->profiles().size(), 1);
            QCOMPARE(app->profiles->currentUid(), profileUid_);
            QCOMPARE(app->profiles->profiles().first().name, QStringLiteral("home"));
            QCOMPARE(wf::readTextFile(app->profiles->profiles().first().filePath), profileBody());

            // Still usable: the same journey's start works again, and the quit
            // gate is the one that stops the core this time.
            QString launchedConfig;
            QObject::connect(app->runtimeCoordinator.get(),
                             &app::runtime::RuntimeCoordinator::coreStartRequested,
                             app->runtimeCoordinator.get(),
                             [&launchedConfig](const QString &path) { launchedConfig = path; });
            QVERIFY(app->runtimeCoordinator->requestAutostart());
            QVERIFY2(wf::waitFor([&app] { return app->backend->state() == cb::CoreState::Running; }),
                     qPrintable(QStringLiteral("the reopened application could not start a core. "
                                               "%1 | %2")
                                    .arg(controller.pendingReport(), app->events.transcript())));
            QCOMPARE(wf::liveProcessesOf(engine.binaryPath()), 1);

            QVERIFY2(app->quit(), qPrintable(QStringLiteral("the quit never completed: %1")
                                                 .arg(app->blockingReason())));
            QVERIFY2(app->shutdownWarnings.isEmpty(),
                     qPrintable(app->shutdownWarnings.join(QLatin1Char('\n'))));
            QVERIFY2(app->events.stops.back().confirmed,
                     "quitting did not confirm the owned child's exit");
            QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()),
                     "the owned child outlived the quit");
            QVERIFY(wf::waitFor([&launchedConfig] { return !QFileInfo::exists(launchedConfig); }));
            QVERIFY(snapshotsIn(app->profiles->dataDir()).isEmpty());
        }

        QVERIFY2(!controller.sawUnexpectedRequest(), qPrintable(controller.redactedTranscript()));
    }

    // --- The negative control for the readiness claim above.
    //
    // A journey that starts a child and never gets an answer must report a
    // failure, not sit in Starting for ever, and must not leave the child
    // behind. Without this case, "readiness is real" could be satisfied by a
    // backend that simply never becomes ready.

    void aControllerThatNeverAnswersFailsTheLaunchAndLeavesNoChild() {
        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        // Answers, unusably. The readiness contract requires 200 with a string
        // `version`; 503 is neither.
        controller.route("GET", "/version", LoopbackServer::Reply::failure(503, "{}"));
        controller.route("GET", "/configs", LoopbackServer::Reply::json("{}"));
        controller.route("GET", "/proxies", LoopbackServer::Reply::json(R"({"proxies":{}})"));
        controller.route("GET", "/rules", LoopbackServer::Reply::json(R"({"rules":[]})"));

        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) {
            QSKIP(qPrintable(QStringLiteral("The control case needs 127.0.0.1:%1: %2")
                                 .arg(wf::kGeneratedControllerPort)
                                 .arg(relay.errorString())));
        }

        const QString engineDir = environment_->filePath(QStringLiteral("engine/.keep"));
        QDir().mkpath(QFileInfo(engineDir).absolutePath());
        FakeCore engine(QFileInfo(engineDir).absolutePath());
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        QVERIFY(engine.validationSucceeds().runsForever().commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        auto app = std::make_unique<wf::AssembledApp>(proxyLog, osProxy);
        QVERIFY2(app->moduleLoaded(), qPrintable(app->moduleError));
        app->bootstrap(engine.binaryPath());

        // THE BUDGET IS THE MODULE'S OWN. This case used to shrink the idle
        // readiness deadline to 400 ms through MihomoBackendImpl::setTimings().
        // There is no such call across the module boundary, and it is not coming
        // back: the shipped deadline is what a user waits, so it is what this
        // case waits. Read from timings() rather than written down, so a
        // production change to the budget moves this wait with it instead of
        // turning into a mystery timeout.
        const cb::BackendTimings published = app->backend->timings();
        QVERIFY2(published.idleDeadlineMs > 0,
                 "the module published a zero readiness deadline, so nothing bounds a launch");
        QCOMPARE(published.idleDeadlineMs, cb::kContractTimings.idleDeadlineMs);
        // The fake core prints NOTHING after validation, and the idle deadline
        // is silence-based (capabilities.h: refreshed by every log line), so the
        // failure arrives one idle deadline after the launch rather than at the
        // three-minute hard cap.
        const int readinessDeadline = wf::deadlineFor(published.idleDeadlineMs);

        const QString source = environment_->filePath(QStringLiteral("import/home.yaml"));
        QVERIFY(wf::writeTextFile(source, profileBody()));
        app->profiles->importFromFile(source);
        QVERIFY(!app->profiles->currentUid().isEmpty());

        QVERIFY(app->runtimeCoordinator->requestAutostart());
        // Stopped is the state this backend STARTS in, so waiting for
        // "Failed or Stopped" would be satisfied before anything happened. The
        // launch has to be observed first.
        QVERIFY2(wf::waitFor([&app] { return app->events.sawState(cb::CoreState::Starting); }),
                 qPrintable(QStringLiteral("no core was ever launched. %1 | %2")
                                .arg(app->storeErrors.join(QLatin1Char(' ')),
                                     app->events.transcript())));
        QVERIFY2(wf::waitFor([&engine] { return engine.invocationCount() >= 2; }),
                 "the child was never launched, so nothing was probed");
        QVERIFY2(wf::waitFor(
                     [&app] {
                         return app->backend->state() == cb::CoreState::Failed ||
                                app->backend->state() == cb::CoreState::Stopped;
                     },
                     readinessDeadline),
                 qPrintable(QStringLiteral("a core that never became ready stayed in state %1 for "
                                           "%2 ms, past the %3 ms readiness deadline the module "
                                           "publishes")
                                .arg(static_cast<int>(app->backend->state()))
                                .arg(readinessDeadline)
                                .arg(published.idleDeadlineMs)));
        QVERIFY(app->backend->drain());
        QVERIFY2(!app->events.coreFailures.empty(), "the failure was never reported");
        QCOMPARE(app->events.coreFailures.back().error.code, cb::ErrorCode::ReadyTimeout);
        QVERIFY2(app->events.readyEndpoints.empty(),
                 "an unusable /version answer was accepted as readiness");
        QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()),
                 "the child of a failed launch was left running");

        QVERIFY2(app->quit(), qPrintable(app->blockingReason()));
        QVERIFY(snapshotsIn(app->profiles->dataDir()).isEmpty());
    }

  private:
    static QByteArray profileBody() {
        return wf::directOnlyProfile(0, QStringLiteral("first-launch-home"));
    }

    static QStringList snapshotsIn(const QString &dataDir) {
        return QDir(dataDir).entryList({QStringLiteral(".runtime-*")},
                                       QDir::Files | QDir::Hidden);
    }

    std::unique_ptr<ScopedEnvironment> environment_;
    QString enginePath_;
    QString profileUid_;
};

QTEST_GUILESS_MAIN(FirstLaunchTest)
#include "first_launch_test.moc"
