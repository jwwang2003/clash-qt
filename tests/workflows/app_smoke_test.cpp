// The executable smoke harness.
//
// docs/TEST_STRATEGY.md, "Complete workflows":
//
//   "The executable smoke harness launches the actual installed app with
//    --data-dir and --no-autostart, drives its supported single-instance/quit
//    behaviour, and checks process exit and isolated artifacts. ... If
//    cross-process UI automation needs a harness, build it separately rather
//    than exposing an unrestricted test-control API in releases."
//
// WHAT THIS IS FOR. Every other suite in the tree links the application's
// libraries and assembles the objects itself. That is why a defect can ship in
// src/main.cpp with a green board: the seam is proven, the wiring that uses it
// is not. This harness therefore takes the SHIPPED BINARY as its subject and
// asks only questions that can be answered from outside it - files it creates,
// processes it starts, sockets it answers on, the code it exits with. There is
// no test-control flag, no environment hook into the composition root, and
// nothing in src/** changed to make any of it observable.
//
// THE INJECTION CASE, AND WHY IT IS SHAPED THE WAY IT IS
//
// src/main.cpp injects core::PrivilegedServiceClientAdapter into the backend.
// Without it the backend falls back to NullPrivilegedCoreService, whose
// isSupported() is false, so setExecutionMode(PrivilegedService) is refused and
// the user's saved core/useService preference is silently discarded - the
// application runs a MANAGED child instead. That difference is externally
// visible: in managed mode the engine binary is executed twice (validate, then
// run) and the run leaves a marker; in service mode it is executed once and no
// child is ever launched.
//
// Driving service mode against a host that has a privileged helper installed
// would otherwise ask a root daemon to start a core, which the isolation rules
// forbid outright. So the launch configuration is made deliberately too large
// for the service path: core_process.cpp rejects a service configuration over
// 8 MiB BEFORE it calls startCore(), and the managed path does not read the
// file at all. The service arm therefore stops one step short of the helper,
// every time, and the case refuses to run unless the oversized configuration is
// confirmed first - see the guard in that test.
//
// A control arm with core/useService=false proves the marker is observable at
// all. Without it "the marker never appeared" would pass on a harness that
// could not see markers.
//
// WHAT ELSE THIS LANE NOW DRIVES, AND WHY IT IS DRIVEN FROM HERE
//
// src/main.cpp makes twenty-two connections and four startup calls, and until
// this change not one of them was exercised by anything: wf::AssembledApp
// rebuilds the same graph in parallel, so deleting a connection from the
// composition root changed nothing any suite could see. Three of those
// statements ARE observable from outside the process, and the cases below drive
// them through the shipped binary, with no flag and nothing added to src/**:
//
//   * `instance.newConnection -> window` - the running instance READS the
//     handover and closes the socket. A client that never sees its socket close
//     is a running instance that ignored the request.
//   * `bridge.attach(backend.discoverEndpoint())` and
//     `bridge.openTrafficStream()` - CLASH_QT_CONTROLLER is a production
//     discovery input, so a loopback controller placed there sees the process
//     attach and open the stream, or does not.
//   * `poll.timeout -> refreshVersion + refreshProxies`, and `poll.start` - the
//     same controller sees the liveness probe come round again, twice, after
//     everything else has gone quiet.
//
// Everything on the quit path stays unreachable: there is no supported way to
// ask the shipped application to quit from outside its own process, and adding
// one is the test-control API the strategy forbids. That is what
// workflows/composition_root_audit.h is the net under, and
// theHarnessStillMirrorsTheCompositionRoot() below is where it is asked.

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>

#include <memory>
#include <vector>

#include "core/preferences/preferences.h"
#include "core/profiles/profile_store.h"
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

namespace {

// The size core::CoreProcess refuses to hand the privileged service.
constexpr qint64 kServiceConfigLimit = 8 * 1024 * 1024;

QString appBinary() { return qEnvironmentVariable("CLASH_QT_APP_BINARY"); }

/// The child's environment: the developer's, minus the data-directory override,
/// plus a platform it can actually start on. CLASH_QT_DATA_DIR is REMOVED on
/// purpose - the harness passes --data-dir and nothing else, so a broken flag
/// cannot be masked by an inherited variable. CLASH_QT_CONTROLLER and
/// CLASH_QT_SECRET are removed for the same reason and put back only by the
/// case that means to be attached somewhere, so a developer who happens to have
/// a controller exported cannot change what any other case observes.
QProcessEnvironment childEnvironment(const QString &controller = QString(),
                                     const QString &secret = QString()) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("CLASH_QT_DATA_DIR"));
    environment.remove(QStringLiteral("CLASH_QT_CORE_BINARY"));
    environment.remove(QStringLiteral("CLASH_QT_CONTROLLER"));
    environment.remove(QStringLiteral("CLASH_QT_SECRET"));
    if (!controller.isEmpty()) {
        environment.insert(QStringLiteral("CLASH_QT_CONTROLLER"), controller);
        environment.insert(QStringLiteral("CLASH_QT_SECRET"), secret);
    }
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    return environment;
}

/// Everything the assembled shell asks a controller for on its way up. Answered
/// so the process settles into a steady state: the point of the poll case is
/// what happens AFTER it has gone quiet.
void scriptController(LoopbackServer &controller) {
    using Reply = LoopbackServer::Reply;
    controller.route("GET", "/version", Reply::json(R"({"version":"smoke-core-1.19.31"})"));
    controller.route("GET", "/configs",
                     Reply::json(R"({"mode":"rule","mixed-port":0,"port":0,"socks-port":0,"tun":{"enable":false}})"));
    controller.route("GET", "/proxies",
                     Reply::json(R"({"proxies":{"GLOBAL":{"type":"Selector","now":"DIRECT","all":["DIRECT"]},"DIRECT":{"type":"Direct","now":""}}})"));
    controller.route("GET", "/rules",
                     Reply::json(R"({"rules":[{"type":"MATCH","payload":"","proxy":"DIRECT"}]})"));
    controller.route("GET", "/providers/proxies", Reply::json(R"({"providers":{}})"));
    controller.route("GET", "/providers/rules", Reply::json(R"({"providers":{}})"));
    for (const char *stream : {"/traffic", "/connections", "/logs", "/memory"})
        controller.expectStream(QLatin1String(stream));
}

QByteArray digestOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QByteArrayLiteral("absent");
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
}

}  // namespace

class AppSmokeTest : public QObject {
    Q_OBJECT

  private slots:

    void initTestCase() {
        if (appBinary().isEmpty() || !QFileInfo::exists(appBinary())) {
            QSKIP("CLASH_QT_APP_BINARY is not set to a built application. Register this test with "
                  "ENVIRONMENT \"CLASH_QT_APP_BINARY=$<TARGET_FILE:clash-qt>\".");
        }
    }

    void init() {
        environment_ = std::make_unique<ScopedEnvironment>(QStringLiteral("smoke"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        preferenceDigestBefore_ = digestOf(environment_->productionSettingsFilePath());
        controllerEndpoint_.clear();
        controllerSecret_.clear();
    }

    void cleanup() {
        for (const auto &process : started_) {
            if (!process) continue;
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(5000);
            }
        }
        started_.clear();
        if (!enginePath_.isEmpty()) {
            const int alive = wf::liveProcessesOf(enginePath_);
            QVERIFY2(alive <= 0,
                     qPrintable(QStringLiteral("%1 engine process(es) outlived the test")
                                    .arg(alive)));
            enginePath_.clear();
        }
        // The developer's own preference store, byte for byte.
        QCOMPARE(digestOf(environment_->productionSettingsFilePath()), preferenceDigestBefore_);
        const QString escape = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escape.isEmpty(), qPrintable(escape));
        environment_.reset();
    }

    // --- launch, artefacts, exit ---------------------------------------------

    void theApplicationLaunchesIntoItsDataDirectoryAndExitsOnRequest() {
        const QString dataDir = environment_->dataDir();
        auto *app = launch(dataDir, {QStringLiteral("--no-autostart")});
        QVERIFY2(awaitRunning(app, dataDir),
                 qPrintable(QStringLiteral("the application never came up: %1")
                                .arg(transcript(app))));

        // Isolated artefacts: the single-instance lock and the preference
        // directory core::preferences creates eagerly both landed inside the
        // directory --data-dir named. Neither exists unless the flag reached
        // both the profile store and the preference module.
        QVERIFY2(QFileInfo::exists(dataDir + QStringLiteral("/instance.lock")),
                 "the application did not take its single-instance lock in --data-dir");
        // Awaited, not asserted on the spot: the lock is taken before the
        // settings bootstrap runs, so the two artefacts appear in that order.
        QVERIFY2(wf::waitFor([&dataDir] {
                     return QFileInfo(dataDir + QStringLiteral("/settings/clash-qt")).isDir();
                 }, 30000),
                 "preferences did not resolve into --data-dir");
        // The profile directory is deliberately NOT asserted: an empty
        // workspace never writes a profile, so it does not exist yet. The lock
        // and the preference directory are what --data-dir demonstrably reached.

        // Process exit. There is no supported way to ask the running
        // application to quit from outside it - the single-instance channel
        // accepts only "show" - so the harness terminates it and requires the
        // exit to be prompt and complete. Exposing a quit command purely for
        // this harness is exactly the test-control API the strategy forbids.
        app->terminate();
        QVERIFY2(app->waitForFinished(15000),
                 qPrintable(QStringLiteral("the application did not exit: %1").arg(transcript(app))));
        QCOMPARE(app->state(), QProcess::NotRunning);
        // Nothing of ours is left behind: with --no-autostart the application
        // owns no child, and a leaked one would show up here.
        QCOMPARE(wf::liveProcessesOf(appBinary() + QStringLiteral(" --data-dir ") + dataDir), 0);
    }

    // --- single instance, per data directory ----------------------------------

    void aSecondInstanceDefersToTheFirstOnlyWhenTheyShareADataDirectory() {
        const QString first = environment_->dataDir();
        const QString second = environment_->filePath(QStringLiteral("other-data/.keep"));
        QDir().mkpath(QFileInfo(second).absolutePath());
        const QString secondDir = QFileInfo(second).absolutePath();

        auto *owner = launch(first, {QStringLiteral("--no-autostart")});
        QVERIFY2(awaitRunning(owner, first), qPrintable(transcript(owner)));

        // Same directory: the newcomer hands its request to the owner and exits
        // successfully rather than opening a second window on one data set.
        QProcess intruder;
        intruder.setProcessEnvironment(childEnvironment());
        intruder.start(appBinary(), {QStringLiteral("--data-dir"), first,
                                     QStringLiteral("--no-autostart")});
        QVERIFY(intruder.waitForStarted(10000));
        QVERIFY2(intruder.waitForFinished(20000),
                 "a second instance on an occupied data directory did not exit");
        QCOMPARE(intruder.exitStatus(), QProcess::NormalExit);
        QCOMPARE(intruder.exitCode(), 0);
        QVERIFY2(owner->state() == QProcess::Running,
                 "the original instance died when a second one arrived");

        // A different directory is a different application state, and both run.
        auto *neighbour = launch(secondDir, {QStringLiteral("--no-autostart")});
        QVERIFY2(awaitRunning(neighbour, secondDir), qPrintable(transcript(neighbour)));
        QVERIFY(QFileInfo::exists(first + QStringLiteral("/instance.lock")));
        QVERIFY(QFileInfo::exists(secondDir + QStringLiteral("/instance.lock")));
        QVERIFY2(owner->state() == QProcess::Running,
                 "an instance on another data directory disturbed the first");

        for (QProcess *process : {owner, neighbour}) {
            process->terminate();
            QVERIFY2(process->waitForFinished(15000), qPrintable(transcript(process)));
        }
    }

    // --- the single-instance handover is READ, not just accepted --------------
    //
    // The case above proves a second launch exits cleanly; it cannot tell
    // whether the running instance did anything with the request, because the
    // newcomer neither waits for nor reads a reply. src/main.cpp's
    // QLocalServer::newConnection connection is the only thing that takes the
    // pending connection, raises the window and destroys the socket. Deleting it
    // leaves the request sitting in QLocalServer's queue with the peer still
    // connected - which is exactly what this case refuses to see.

    void theRunningInstanceAnswersItsSingleInstanceChannel() {
        const QString dataDir = environment_->dataDir();
        auto *app = launch(dataDir, {QStringLiteral("--no-autostart")});
        QVERIFY2(awaitRunning(app, dataDir), qPrintable(transcript(app)));

        const QString name = instanceNameFor(dataDir);
        QVERIFY2(!name.isEmpty(),
                 "core::ProfileStore in this process did not resolve --data-dir, so the "
                 "single-instance socket name cannot be derived the way main.cpp derives it");

        // RETRIED, not attempted once. The lock file the readiness probe waits
        // for is written a few statements BEFORE QLocalServer::listen(), and
        // removeServer() unlinks the socket path immediately before it, so a
        // single connectToServer() can legitimately land in that window and fail
        // with ENOENT. A retry closes it without weakening anything: the only
        // outcome that passes is still a socket that connects.
        QLocalSocket socket;
        QElapsedTimer attempts;
        attempts.start();
        bool connected = false;
        while (!connected && attempts.elapsed() < 20000) {
            socket.abort();
            socket.connectToServer(name);
            connected = socket.waitForConnected(1000);
            if (!connected) wf::drainPast(100);
        }
        QVERIFY2(connected,
                 qPrintable(QStringLiteral(
                                "nothing is listening on the single-instance socket %1 after %2 "
                                "ms: either the composition root stopped listening, or the name "
                                "main.cpp derives from the data directory has changed and this "
                                "case must follow it. Error: %3. Application: %4")
                                .arg(name)
                                .arg(attempts.elapsed())
                                .arg(socket.errorString(), transcript(app))));
        QCOMPARE(socket.write("show"), qint64(4));
        QVERIFY(socket.waitForBytesWritten(5000));

        // The handler calls deleteLater() on the accepted socket, so the peer
        // sees the connection close. Nothing else in the application closes it.
        QVERIFY2(wf::waitFor([&socket] { return socket.state() != QLocalSocket::ConnectedState; },
                             15000),
                 qPrintable(QStringLiteral(
                                "the running instance never read the handover: the socket is "
                                "still open %1 ms after the request. src/main.cpp's "
                                "QLocalServer::newConnection connection is what reads it, shows "
                                "and raises the window and closes the socket; without it a second "
                                "launch on an occupied data directory silently does nothing. "
                                "Application: %2")
                                .arg(15000)
                                .arg(transcript(app))));
        QVERIFY2(app->state() == QProcess::Running,
                 qPrintable(QStringLiteral("the instance died answering the handover: %1")
                                .arg(transcript(app))));

        app->terminate();
        QVERIFY2(app->waitForFinished(15000), qPrintable(transcript(app)));
    }

    // --- the startup attachment and the controller poll -----------------------
    //
    // Three statements of the composition root at once, all of them invisible
    // until something answers where the process looks:
    //
    //   bridge.attach(backend.discoverEndpoint())  the first GET /version
    //   bridge.openTrafficStream()                 the /traffic handshake
    //   poll.start / poll.timeout -> refresh*      /version AND /proxies, again,
    //                                              after everything else settled
    //
    // CLASH_QT_CONTROLLER is what core::discoverEndpoint() reads first. It is a
    // shipped feature - it is how the application finds an already-running core
    // - not a hook added for this harness.

    void theCompositionRootAttachesToADiscoveredControllerAndKeepsPollingIt() {
        LoopbackServer controller;
        QVERIFY2(controller.listen(QStringLiteral("smoke-secret")), "the fixture did not bind");
        scriptController(controller);
        controllerEndpoint_ = QStringLiteral("127.0.0.1:%1").arg(controller.port());
        controllerSecret_ = QStringLiteral("smoke-secret");

        const QString dataDir = environment_->dataDir();
        auto *app = launch(dataDir, {QStringLiteral("--no-autostart")});
        QVERIFY2(awaitRunning(app, dataDir), qPrintable(transcript(app)));

        QVERIFY2(wf::waitFor(
                     [&controller] {
                         return controller.requestCount("GET", QStringLiteral("/version")) >= 1;
                     },
                     30000),
                 qPrintable(QStringLiteral(
                                "the application never attached to the controller it discovered. "
                                "src/main.cpp's bridge.attach(backend.discoverEndpoint()) is the "
                                "only attachment before a managed core exists; without it an "
                                "already-running core is invisible. %1 | %2")
                                .arg(controller.redactedTranscript(), transcript(app))));

        QVERIFY2(wf::waitFor(
                     [&controller] {
                         return controller.streamHandshakes(QStringLiteral("/traffic")) >= 1;
                     },
                     30000),
                 qPrintable(QStringLiteral(
                                "the application attached but never opened the traffic stream. "
                                "src/main.cpp's bridge.openTrafficStream() is its only opener, "
                                "and without it the tray tooltip and the overview graph stay "
                                "empty for the whole session. %1")
                                .arg(controller.redactedTranscript())));

        // Let the shell finish asking its own questions, so what follows can
        // only be the poll.
        QVERIFY(wf::drainPast(2000));
        const int versions = controller.requestCount("GET", QStringLiteral("/version"));
        const int proxies = controller.requestCount("GET", QStringLiteral("/proxies"));
        // TWO more of each: one round could be a straggler from the start-up
        // burst, two rounds five seconds apart is a periodic probe. Both calls
        // are asserted, because the connection's body performs both.
        QVERIFY2(wf::waitFor(
                     [&controller, versions, proxies] {
                         return controller.requestCount("GET", QStringLiteral("/version")) >=
                                    versions + 2 &&
                                controller.requestCount("GET", QStringLiteral("/proxies")) >=
                                    proxies + 2;
                     },
                     25000),
                 qPrintable(QStringLiteral(
                                "the controller poll never came round: after the start-up burst "
                                "the application asked for /version %1 more time(s) and /proxies "
                                "%2 more time(s), where src/main.cpp's five-second QTimer should "
                                "have asked for both at least twice. Without it a controller that "
                                "comes back is never noticed and the proxy list never refreshes. "
                                "%3")
                                .arg(controller.requestCount("GET", QStringLiteral("/version")) -
                                     versions)
                                .arg(controller.requestCount("GET", QStringLiteral("/proxies")) -
                                     proxies)
                                .arg(controller.redactedTranscript())));

        QVERIFY2(!controller.sawUnauthenticatedRequest(),
                 qPrintable(QStringLiteral("the application reached the controller without the "
                                           "secret it was given: %1")
                                .arg(controller.redactedTranscript())));
        QVERIFY2(app->state() == QProcess::Running,
                 qPrintable(QStringLiteral("the application exited during the journey: %1")
                                .arg(transcript(app))));

        app->terminate();
        QVERIFY2(app->waitForFinished(15000), qPrintable(transcript(app)));
    }

    // --- the wiring no external observer can reach ----------------------------
    //
    // Everything on the quit path, plus the tray's traffic feed. See
    // workflows/composition_root_audit.h for what this compares and why it is a
    // source-level comparison rather than a journey.

    void theHarnessStillMirrorsTheCompositionRoot() {
        const QString drift = wf::audit::compositionRootDrift();
        QVERIFY2(drift.isEmpty(), qPrintable(drift));
    }

    // --- the composition root's privileged-service injection -------------------

    void theCompositionRootSelectsServiceModeWhenTheUserSavedIt_data() {
        QTest::addColumn<bool>("useService");
        QTest::addColumn<bool>("expectManagedChild");
        // The control arm. Without it, "no child was launched" would pass on a
        // harness that cannot see a child at all.
        QTest::newRow("managed mode launches the engine") << false << true;
        // The claim. Service mode is only reachable if main.cpp injected the
        // privileged seam; without the injection this row launches a managed
        // child and fails.
        QTest::newRow("service mode launches no managed child") << true << false;
    }

    void theCompositionRootSelectsServiceModeWhenTheUserSavedIt() {
        QFETCH(bool, useService);
        QFETCH(bool, expectManagedChild);

        const QString dataDir = environment_->dataDir();
        const QString engineDir = environment_->filePath(QStringLiteral("engine/.keep"));
        QDir().mkpath(QFileInfo(engineDir).absolutePath());
        FakeCore engine(QFileInfo(engineDir).absolutePath());
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        const QString marker = QStringLiteral("launched");
        // Validation passes; a RUN touches the marker and exits at once, so the
        // arm that does launch a child leaves nothing behind to clean up.
        QVERIFY(engine.validationSucceeds().touches(marker).exitsWith(0).commit());

        // A profile whose generated configuration is too large for the service
        // path. The managed path never reads it, so only the service arm is
        // affected - which is the whole point: the service arm must fail before
        // it can reach a privileged helper.
        {
            core::ProfileStore seed;
            QCOMPARE(QDir(seed.dataDir()).absolutePath(), QDir(dataDir).absolutePath());
            seed.load();
            QVERIFY(seed.createLocalProfile(QStringLiteral("oversized"), oversizedProfile()));
            QVERIFY(!seed.currentUid().isEmpty());
            const QString generated = seed.generateRuntimeConfig();
            QVERIFY2(!generated.isEmpty(), "the oversized profile did not generate");
            const qint64 size = QFileInfo(generated).size();
            QFile::remove(generated);
            if (size <= kServiceConfigLimit) {
                QSKIP(qPrintable(QStringLiteral(
                    "The generated configuration is %1 bytes, at or below the %2-byte service "
                    "limit. Running the service arm would let the application reach a real "
                    "privileged helper, so this case refuses to run rather than risk it.")
                                     .arg(size)
                                     .arg(kServiceConfigLimit)));
            }
        }

        {
            QSettings settings = core::preferences::open();
            settings.setValue(QStringLiteral("core/binary"), engine.binaryPath());
            settings.setValue(QStringLiteral("core/useService"), useService);
            settings.setValue(QStringLiteral("startup/startCore"), true);
            settings.sync();
        }

        // No --no-autostart here, deliberately: the autostart is the observable.
        auto *app = launch(dataDir, {});
        QVERIFY2(awaitRunning(app, dataDir), qPrintable(transcript(app)));

        // The engine is invoked for validation in BOTH modes, which is what
        // makes the second invocation - the launch - the discriminator.
        QVERIFY2(wf::waitFor([&engine] { return engine.invocationCount() >= 1; }, 30000),
                 qPrintable(QStringLiteral("the application never validated a configuration: %1")
                                .arg(transcript(app))));

        const bool launched = wf::waitFor(
            [&engine, &marker] { return engine.reached(marker); }, expectManagedChild ? 30000 : 8000);
        QCOMPARE(launched, expectManagedChild);
        if (expectManagedChild) {
            QVERIFY2(engine.invocationCount() >= 2,
                     qPrintable(QStringLiteral("managed mode invoked the engine %1 time(s)")
                                    .arg(engine.invocationCount())));
        } else {
            QVERIFY2(engine.invocationCount() == 1,
                     qPrintable(QStringLiteral(
                                    "service mode invoked the engine %1 time(s): the composition "
                                    "root did not inject the privileged seam, so the saved "
                                    "core/useService preference was discarded and a managed child "
                                    "was launched instead. Invocations: %2")
                                    .arg(engine.invocationCount())
                                    .arg(engine.invocations().join(QStringLiteral(" | ")))));
        }
        QVERIFY2(app->state() == QProcess::Running,
                 qPrintable(QStringLiteral("the application exited during the journey: %1")
                                .arg(transcript(app))));

        app->terminate();
        QVERIFY2(app->waitForFinished(15000), qPrintable(transcript(app)));
        QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()),
                 "an engine process outlived the application");
    }

  private:
    QProcess *launch(const QString &dataDir, const QStringList &extra) {
        auto process = std::make_unique<QProcess>();
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setProcessEnvironment(childEnvironment(controllerEndpoint_, controllerSecret_));
        QStringList arguments{QStringLiteral("--data-dir"), dataDir};
        arguments += extra;
        process->start(appBinary(), arguments);
        QProcess *raw = process.get();
        started_.push_back(std::move(process));
        return raw;
    }

    /// Up means: the process is still running and it has taken the
    /// single-instance lock inside the directory it was pointed at. Both, so a
    /// process that started and immediately died cannot read as up.
    static bool awaitRunning(QProcess *process, const QString &dataDir) {
        if (!process->waitForStarted(15000)) return false;
        return wf::waitFor(
            [process, &dataDir] {
                return process->state() == QProcess::Running &&
                       QFileInfo::exists(dataDir + QStringLiteral("/instance.lock"));
            },
            30000);
    }

    static QString transcript(QProcess *process) {
        return QStringLiteral("state=%1 exit=%2 output=%3")
            .arg(static_cast<int>(process->state()))
            .arg(process->exitCode())
            .arg(QString::fromUtf8(process->readAll()).left(2000));
    }

    /// A DIRECT-only profile with one very large scalar. Everything that makes
    /// it a Clash configuration is unchanged; the padding is an unknown key,
    /// which the generator copies through untouched.
    static QString oversizedProfile() {
        QByteArray yaml = wf::directOnlyProfile(0, QStringLiteral("smoke-oversized"));
        yaml += "clash-qt-workflow-padding: \"";
        yaml += QByteArray(9 * 1024 * 1024, 'x');
        yaml += "\"\n";
        return QString::fromUtf8(yaml);
    }

    /// The socket name src/main.cpp derives from the data directory the profile
    /// store resolved. Computed the same way here, from a store this process
    /// builds against the same directory, rather than from the literal path -
    /// the hash is over what the STORE returned. A connect that fails while the
    /// second-instance case still passes means this derivation has drifted from
    /// the composition root's, not that the server is missing; the failure
    /// message below says so.
    static QString instanceNameFor(const QString &dataDir) {
        core::ProfileStore store;
        if (QDir(store.dataDir()).absolutePath() != QDir(dataDir).absolutePath()) return QString();
        return QStringLiteral("clash-qt-") +
               QString::fromLatin1(QCryptographicHash::hash(store.dataDir().toUtf8(),
                                                            QCryptographicHash::Sha256)
                                       .toHex()
                                       .left(20));
    }

    std::unique_ptr<ScopedEnvironment> environment_;
    std::vector<std::unique_ptr<QProcess>> started_;
    QByteArray preferenceDigestBefore_;
    QString enginePath_;
    /// Handed to the child as CLASH_QT_CONTROLLER/CLASH_QT_SECRET, which is how
    /// core::discoverEndpoint() is told where to look. A production input, not
    /// a test hook.
    QString controllerEndpoint_;
    QString controllerSecret_;
};

QTEST_GUILESS_MAIN(AppSmokeTest)
#include "app_smoke_test.moc"
