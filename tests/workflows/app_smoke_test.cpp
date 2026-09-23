// The executable smoke harness.
//
// The rule this harness exists to satisfy: launch the actual installed app with
// --data-dir and --no-autostart, drive its supported single-instance and quit
// behaviour, and check process exit and isolated artifacts - and if
// cross-process UI automation ever needs a harness, build it separately rather
// than exposing an unrestricted test-control API in releases.
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
// THAT GUARD WAS NOT ENOUGH, AND WHY IT SAYS SO HERE. It covers startCore and
// nothing else. The settings page queries the privileged service's STATUS from
// its constructor, on every launch, with no configuration involved at all - so
// before src/main.cpp honoured CLASH_QT_SERVICE_SOCKET, every case in this file
// opened a connection to the machine-wide root helper and no case could tell.
// childEnvironment() below now gives every child its own socket, unconditionally
// and from this harness's own scope, and
// theStartupServiceStatusGoesToTheSocketTheHostSelected() is where that is
// proven against a local QLocalServer rather than asserted in a comment. The
// oversized-configuration guard stays: the two cover different steps.
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
//   * the guarded attachment - `discoverEndpoint()`, then `attach()` only when
//     `cb::isValid()` accepts what came back - and `bridge.openTrafficStream()`.
//     CLASH_QT_CONTROLLER is a production discovery input, so a loopback
//     controller placed there sees the process attach and open the stream, or
//     does not; and a child given NO controller must reach that same fixture
//     never, which is theIsolatedLaunchAttachesToNoControllerItWasNotGiven.
//   * `poll.timeout -> refreshVersion + refreshProxies`, and `poll.start` - the
//     same controller sees the liveness probe come round again, twice, after
//     everything else has gone quiet.
//   * the privileged-helper socket the composition root selects - a local
//     QLocalServer on that socket either receives the startup status frame or
//     does not, and a second one on another socket proves it was the selection
//     and not the neighbourhood.
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
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtEndian>

#include <memory>
#include <vector>

#include "core/preferences/preferences.h"
#include "core/profiles/profile_store.h"
#include "platform/service/privileged_service_client.h"
#include "platform/service/privileged_service_installer.h"
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

/// Everything a child of this harness is allowed to see.
///
/// TWO VARIABLES ARE NON-NEGOTIABLE, and both are set from this harness's own
/// scope rather than inherited:
///
///   * CLASH_QT_DATA_DIR. Every child gets one, always, in addition to
///     --data-dir. An earlier version of this file REMOVED it so that a broken
///     flag could not be masked by an inherited value; that made isolation
///     depend on the flag alone, and a child whose flag failed to parse would
///     have opened the developer's real data directory. The flag is still
///     proven, and proven harder: `dataDirOverride` points the variable at a
///     DECOY directory inside this test's scope, and the case that cares
///     asserts the artefacts landed where --data-dir said and not in the decoy.
///     src/main.cpp qputenv()s the flag over the variable, so --data-dir wins
///     and both paths stay inside the scope either way.
///
///   * CLASH_QT_SERVICE_SOCKET. Every child gets its own, always, never
///     inherited and never the installed default. src/main.cpp hands this to
///     platform::PrivilegedServiceClient, and the Settings page asks for the
///     privileged service's status on startup (ServiceSettings::checkStatus(),
///     queued from its constructor). Without an explicit socket that status
///     request goes to the machine-wide root helper at
///     PrivilegedServiceClient::defaultSocketPath() - which the >8 MiB guard
///     below does NOT prevent, because that guard stops startCore and not a
///     status query. `serviceSocket` is asserted absolute here, because
///     main.cpp refuses a relative one and a child that exited 2 would look
///     like an unrelated failure.
///
/// CLASH_QT_CONTROLLER and CLASH_QT_SECRET are removed and put back only by the
/// case that means to be attached somewhere, so a developer who happens to have
/// a controller exported cannot change what any other case observes.
///
/// CLASH_QT_CONTROLLER_PORT is removed for the same reason, and never put back:
/// it moves the port core::ProfileStore writes into every configuration it
/// generates, so an inherited one would send the shipped binary's own core to
/// an address this harness knows nothing about.
///
/// CLASH_QT_MODULE_PATH is deliberately INHERITED, because it is what the
/// registration stages for this suite and the shipped binary cannot start
/// without it in a build tree (ModuleLoader's other candidate is the installed
/// bundle layout). `module` replaces it for the one case that means to point
/// the application at an artifact that is not there.
QProcessEnvironment childEnvironment(const QString &dataDirOverride, const QString &serviceSocket,
                                     const QString &controller = QString(),
                                     const QString &secret = QString(),
                                     const QString &module = QString()) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("CLASH_QT_CORE_BINARY"));
    environment.remove(QStringLiteral("CLASH_QT_CONTROLLER"));
    environment.remove(QStringLiteral("CLASH_QT_SECRET"));
    environment.remove(QStringLiteral("CLASH_QT_CONTROLLER_PORT"));
    environment.insert(QStringLiteral("CLASH_QT_DATA_DIR"), dataDirOverride);
    environment.insert(QStringLiteral("CLASH_QT_SERVICE_SOCKET"), serviceSocket);
    if (!controller.isEmpty()) {
        environment.insert(QStringLiteral("CLASH_QT_CONTROLLER"), controller);
        environment.insert(QStringLiteral("CLASH_QT_SECRET"), secret);
    }
    if (!module.isEmpty()) environment.insert(QStringLiteral("CLASH_QT_MODULE_PATH"), module);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    return environment;
}

/// A QLocalServer standing in for the privileged helper, on a socket this test
/// owns. It NEVER speaks to, probes or replaces the installed root helper: it
/// is a plain user-owned socket in a temporary directory, and the only thing
/// the application can learn from it is that something answered.
///
/// Requests are recorded whole, in the helper's own framing (a big-endian
/// uint32 length followed by compact JSON - see
/// platform/service/privileged_service_client.cpp), so a case can assert WHICH
/// command arrived rather than only that bytes did.
class HelperFixture : public QObject {
  public:
    bool listen(const QString &path) {
        QLocalServer::removeServer(path);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        connect(&server_, &QLocalServer::newConnection, this, [this] { accept(); });
        return server_.listen(path);
    }

    QString errorString() const { return server_.errorString(); }
    int connections() const { return connections_; }
    QStringList commands() const { return commands_; }
    /// What the peer sent, for a failure message.
    QStringList transcript() const { return transcript_; }

    /// Answer a `status` the way the helper does, so the application takes the
    /// normal success path rather than an error path. Nothing privileged
    /// happens: the reply is a literal.
    void replyToStatus(bool reply) { replyToStatus_ = reply; }

  private:
    void accept() {
        while (QLocalSocket *peer = server_.nextPendingConnection()) {
            ++connections_;
            auto buffer = std::make_shared<QByteArray>();
            connect(peer, &QLocalSocket::readyRead, this, [this, peer, buffer] {
                buffer->append(peer->readAll());
                while (buffer->size() >= 4) {
                    const quint32 length = qFromBigEndian<quint32>(buffer->constData());
                    if (length > 8u * 1024 * 1024) {  // never trust a length blindly
                        peer->abort();
                        return;
                    }
                    if (static_cast<quint32>(buffer->size()) < length + 4) return;
                    const QByteArray frame = buffer->mid(4, static_cast<int>(length));
                    buffer->remove(0, static_cast<int>(length) + 4);
                    const QJsonObject request = QJsonDocument::fromJson(frame).object();
                    commands_ << request.value(QStringLiteral("command")).toString();
                    transcript_ << QString::fromUtf8(frame);
                    if (replyToStatus_ &&
                        request.value(QStringLiteral("command")).toString() ==
                            QLatin1String("status"))
                        send(peer, QJsonObject{
                                       {QStringLiteral("protocol"), 1},
                                       {QStringLiteral("id"), request.value(QStringLiteral("id"))},
                                       {QStringLiteral("ok"), true},
                                       {QStringLiteral("running"), false},
                                   });
                }
            });
            connect(peer, &QLocalSocket::disconnected, peer, &QLocalSocket::deleteLater);
        }
    }

    static void send(QLocalSocket *peer, const QJsonObject &response) {
        const QByteArray payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
        QByteArray frame(4, Qt::Uninitialized);
        qToBigEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
        frame.append(payload);
        peer->write(frame);
    }

    QLocalServer server_;
    QStringList commands_;
    QStringList transcript_;
    int connections_ = 0;
    bool replyToStatus_ = true;
};

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
        moduleOverride_.clear();
        dataDirEnv_.clear();
        output_.clear();
        // The socket every child of this case will be pointed at. Inside the
        // scope's short socket directory, because AF_UNIX truncates a long
        // sun_path and a truncated path is a different socket. Nothing listens
        // on it unless a case starts a fixture: an application that finds
        // nobody home reports a connection error and carries on, which is the
        // behaviour every case but the helper one wants.
        serviceSocket_ = environment_->socketPath(QStringLiteral("helper"));
        QVERIFY2(!serviceSocket_.isEmpty(), qPrintable(environment_->errorString()));
        QVERIFY2(QDir::isAbsolutePath(serviceSocket_),
                 "src/main.cpp exits 2 on a relative CLASH_QT_SERVICE_SOCKET");
        QVERIFY2(serviceSocket_ !=
                     platform::PrivilegedServiceClient::defaultSocketPath(),
                 "this harness must never point a child at the installed helper");
        // Every case launches the shipped binary, and that binary loads its
        // engine from a separately built shared library. The
        // registration stages it; without the variable the application exits 2
        // before it does anything this suite could observe, so the requirement
        // is stated here once rather than diagnosed five times.
        const QString requirement = wf::moduleRequirementFailure();
        QVERIFY2(requirement.isEmpty(), qPrintable(requirement));
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
        // --data-dir is given a DECOY to beat. Every child of this harness now
        // receives CLASH_QT_DATA_DIR as well, so "the flag worked" can no
        // longer be read off a directory the variable would have chosen
        // anyway: the variable names somewhere else, inside this same scope,
        // and the flag has to win. src/main.cpp qputenv()s the flag over the
        // variable, which is the statement under test here.
        dataDirEnv_ = QFileInfo(environment_->filePath(QStringLiteral("decoy-data/.keep")))
                          .absolutePath();
        QDir().mkpath(dataDirEnv_);
        QVERIFY(dataDirEnv_ != dataDir);

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
        QVERIFY2(!QFileInfo::exists(dataDirEnv_ + QStringLiteral("/instance.lock")),
                 "the application used CLASH_QT_DATA_DIR in preference to --data-dir: the flag "
                 "no longer overrides the variable, so `clash-qt --data-dir X` run from a shell "
                 "that exports the variable silently opens the wrong data set");
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

    // --- the engine component the shipped binary actually loaded ---------------
    //
    // The supervisor lives in a separately built shared library, and
    // src/main.cpp is the only place that chooses which one: an explicit
    // CLASH_QT_MODULE_PATH, otherwise the installation-relative artifact. That
    // choice is invisible to every in-process suite - they construct the loader
    // themselves - and it is the exact shape of defect this lane exists for: a
    // module that builds, links and is never loaded by the program that ships
    // it.
    //
    // The application announces the artifact it took ("Engine component
    // loaded: <path>"), so the claim is checkable from outside the process with
    // no test-control flag: the line has to be there AND it has to name the
    // staged module, not some other copy the platform loader found.

    void theApplicationReportsTheStagedEngineComponentItLoaded() {
        const QString staged = wf::moduleArtifactPath();
        const QString dataDir = environment_->dataDir();
        auto *app = launch(dataDir, {QStringLiteral("--no-autostart")});
        QVERIFY2(awaitRunning(app, dataDir), qPrintable(transcript(app)));

        QVERIFY2(wf::waitFor([this, app] {
                     return outputOf(app).contains(QStringLiteral("Engine component loaded:"));
                 }, 30000),
                 qPrintable(QStringLiteral(
                                "the application never reported loading an engine component. "
                                "src/main.cpp loads one through "
                                "clashqt::integration::ModuleLoader before it builds anything "
                                "else; without that the shipped program has no backend at all. "
                                "%1").arg(transcript(app))));
        const QString output = outputOf(app);
        QVERIFY2(output.contains(staged),
                 qPrintable(QStringLiteral(
                                "the application loaded an engine component that is not the "
                                "staged one. CLASH_QT_MODULE_PATH named\n  %1\nand the process "
                                "reported\n  %2\nA module found anywhere else means the explicit "
                                "path was ignored, which is the search behaviour module-r1 "
                                "forbids.")
                                .arg(staged, output.right(2000))));
        QVERIFY2(!output.contains(QStringLiteral("Could not load the engine component")),
                 qPrintable(output.right(2000)));
        QVERIFY2(app->state() == QProcess::Running, qPrintable(transcript(app)));

        app->terminate();
        QVERIFY2(app->waitForFinished(15000), qPrintable(transcript(app)));
    }

    // The negative control. Without it, "the module was loaded" would pass on a
    // harness that cannot tell a loaded module from an ignored one - and a
    // composition root that carried on regardless of a missing engine would be
    // indistinguishable from one that refuses.

    void aMissingEngineComponentFailsTheLaunchLoudly() {
        moduleOverride_ = environment_->filePath(QStringLiteral("absent/not-a-module.dylib"));
        QVERIFY2(!QFileInfo::exists(moduleOverride_), "the 'missing' module exists");

        const QString dataDir = environment_->dataDir();
        auto *app = launch(dataDir, {QStringLiteral("--no-autostart")});
        QVERIFY(app->waitForStarted(15000));
        QVERIFY2(app->waitForFinished(30000),
                 qPrintable(QStringLiteral(
                                "the application kept running without an engine component. A "
                                "process with no backend cannot start a core, cannot attach to "
                                "one and cannot report why; src/main.cpp refuses to start "
                                "instead. %1").arg(transcript(app))));
        QCOMPARE(app->exitStatus(), QProcess::NormalExit);
        QVERIFY2(app->exitCode() != 0,
                 qPrintable(QStringLiteral("a missing engine component exited 0: %1")
                                .arg(transcript(app))));
        const QString output = outputOf(app);
        QVERIFY2(output.contains(QStringLiteral("Could not load the engine component")),
                 qPrintable(QStringLiteral("the refusal was silent: %1").arg(output.right(2000))));
        QVERIFY2(output.contains(QStringLiteral("not-a-module.dylib")) ||
                     output.contains(QStringLiteral("no module at")),
                 qPrintable(QStringLiteral("the refusal does not say which artifact it could not "
                                           "load: %1").arg(output.right(2000))));
        QVERIFY2(!output.contains(QStringLiteral("Engine component loaded:")),
                 qPrintable(QStringLiteral("the application announced a component it could not "
                                           "load: %1").arg(output.right(2000))));
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
        // The newcomer gets the SAME isolated socket as the owner. Two
        // processes may share one fixture path - the helper protocol is a
        // per-connection lease and nothing here even answers - but neither may
        // fall back to the installed default, which is what an inherited or
        // omitted variable would do.
        QProcess intruder;
        intruder.setProcessEnvironment(childEnvironment(first, serviceSocket_));
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
    //   attach(startupEndpoint) when valid          the first GET /version
    //   bridge.openTrafficStream()                  the /traffic handshake
    //   poll.start / poll.timeout -> refresh*       /version AND /proxies, again,
    //                                               after everything else settled
    //
    // CLASH_QT_CONTROLLER is what core::discoverEndpoint() reads first, and
    // since the isolation fix it is the WHOLE answer when it is set. It is a
    // shipped feature - it is how the application finds an already-running core
    // - not a hook added for this harness. This case supplies a local fixture
    // endpoint, so the attachment it observes is to an address this test owns
    // and to nothing else.

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

    // --- an isolated launch attaches to nothing it was not given ---------------
    //
    // THE DEFECT THIS CASE EXISTS FOR. core::discoverEndpoint() ended in
    // `return Endpoint{}`, and core::Endpoint's default is the conventional
    // 127.0.0.1:9090 with isValid() true. Before it, it read Clash Verge Rev's
    // own config.yaml. childEnvironment() above removes CLASH_QT_CONTROLLER
    // from every child that is not meant to be attached anywhere - so every one
    // of those children resolved "a controller" anyway, attached to it, opened
    // a traffic stream on it and polled it every five seconds. On a machine
    // with another client running, that is this suite driving somebody else's
    // engine, and nothing here could see it.
    //
    // WHAT IS ASSERTED, AND WHAT THIS CASE CANNOT SEE. One fixture, two
    // launches. Told nothing, the child must never reach it; told about it
    // through CLASH_QT_CONTROLLER - the production input - the same child at
    // the same address must. The second arm is what makes the first one mean
    // something: without it "nothing arrived" would pass on a harness that
    // cannot see an arrival.
    //
    // It cannot observe an attachment to an address this harness does not own,
    // and it deliberately does not try: binding 127.0.0.1:9090 to catch a stray
    // attach would put a decoy at the very address a real installation uses,
    // which is the direction the isolation rules forbid. That half of the claim
    // - no 9090 guess, no foreign configuration read - is proven in
    // tests/core/mihomo/engine_discovery_test.cpp against the discovery
    // function itself, and src/main.cpp's validity guard is held in place by
    // workflows/composition_root_audit.h.

    void theIsolatedLaunchAttachesToNoControllerItWasNotGiven() {
        LoopbackServer unnamed;
        QVERIFY2(unnamed.listen(QStringLiteral("unnamed-secret")), "the fixture did not bind");
        scriptController(unnamed);
        const QString authority = QStringLiteral("127.0.0.1:%1").arg(unnamed.port());

        // ---- arm 1: told nothing. childEnvironment() removes the variable.
        const QString dataDir = environment_->dataDir();
        QVERIFY(controllerEndpoint_.isEmpty());
        auto *silent = launch(dataDir, {QStringLiteral("--no-autostart")});
        QVERIFY2(awaitRunning(silent, dataDir), qPrintable(transcript(silent)));
        // Past the point where an attaching process would have spoken: the
        // component is loaded, so the composition root has reached the
        // attachment statement, and the poll interval has come round twice.
        QVERIFY2(wf::waitFor([this, silent] {
                     return outputOf(silent).contains(QStringLiteral("Engine component loaded:"));
                 }, 30000),
                 qPrintable(transcript(silent)));
        QVERIFY(wf::drainPast(12000));

        QVERIFY2(unnamed.requests().isEmpty(),
                 qPrintable(QStringLiteral(
                                "a launch that was given no controller reached one anyway: %1. "
                                "src/main.cpp attaches only to an endpoint cb::isValid() accepts, "
                                "and an isolated process with no CLASH_QT_CONTROLLER has none.")
                                .arg(unnamed.redactedTranscript())));
        QCOMPARE(unnamed.requestCount("GET", QStringLiteral("/version")), 0);
        QCOMPARE(unnamed.streamHandshakes(QStringLiteral("/traffic")), 0);
        QVERIFY2(silent->state() == QProcess::Running,
                 qPrintable(QStringLiteral("a launch with no controller did not survive: %1")
                                .arg(transcript(silent))));
        silent->terminate();
        QVERIFY2(silent->waitForFinished(15000), qPrintable(transcript(silent)));

        // ---- arm 2: the same fixture, the same address, named explicitly.
        controllerEndpoint_ = authority;
        controllerSecret_ = QStringLiteral("unnamed-secret");
        auto *told = launch(dataDir, {QStringLiteral("--no-autostart")});
        QVERIFY2(awaitRunning(told, dataDir), qPrintable(transcript(told)));
        QVERIFY2(wf::waitFor([&unnamed] {
                     return unnamed.requestCount("GET", QStringLiteral("/version")) >= 1;
                 }, 30000),
                 qPrintable(QStringLiteral(
                                "the fixture is not observable at all, so arm 1 proves nothing: a "
                                "child told to attach to %1 through CLASH_QT_CONTROLLER never "
                                "arrived. %2 | %3")
                                .arg(authority, unnamed.redactedTranscript(), transcript(told))));
        QVERIFY2(wf::waitFor([&unnamed] {
                     return unnamed.streamHandshakes(QStringLiteral("/traffic")) >= 1;
                 }, 30000),
                 qPrintable(QStringLiteral(
                                "the child attached but never opened the traffic stream. "
                                "src/main.cpp calls bridge.openTrafficStream() unconditionally so "
                                "the subscription outlives a startup with no address; it must "
                                "still open once there IS one. %1")
                                .arg(unnamed.redactedTranscript())));

        told->terminate();
        QVERIFY2(told->waitForFinished(15000), qPrintable(transcript(told)));
    }

    // Every child of this harness carries its own data directory and its own
    // privileged-helper socket, and carries a controller ONLY when the case
    // asked for one. Asserted on the environment childEnvironment() actually
    // builds, because that is the one place a future edit could quietly make
    // isolation conditional again - as an earlier version of this file did when
    // it removed CLASH_QT_DATA_DIR to stop a broken flag being masked.

    void everyChildEnvironmentIsExplicitlyIsolated() {
        const QProcessEnvironment bare =
            childEnvironment(environment_->dataDir(), serviceSocket_);
        QCOMPARE(bare.value(QStringLiteral("CLASH_QT_DATA_DIR")), environment_->dataDir());
        QCOMPARE(bare.value(QStringLiteral("CLASH_QT_SERVICE_SOCKET")), serviceSocket_);
        QVERIFY2(bare.value(QStringLiteral("CLASH_QT_SERVICE_SOCKET")) !=
                     platform::PrivilegedServiceClient::defaultSocketPath(),
                 "a child would have been pointed at the installed root helper");
        QVERIFY2(!bare.contains(QStringLiteral("CLASH_QT_CONTROLLER")),
                 "a child that was given no controller inherited one from the developer's shell");
        QVERIFY2(!bare.contains(QStringLiteral("CLASH_QT_SECRET")), "an inherited secret survived");
        QCOMPARE(bare.value(QStringLiteral("QT_QPA_PLATFORM")), QStringLiteral("offscreen"));

        const QProcessEnvironment attached =
            childEnvironment(environment_->dataDir(), serviceSocket_,
                             QStringLiteral("127.0.0.1:29420"), QStringLiteral("s"));
        QCOMPARE(attached.value(QStringLiteral("CLASH_QT_CONTROLLER")),
                 QStringLiteral("127.0.0.1:29420"));
        QCOMPARE(attached.value(QStringLiteral("CLASH_QT_DATA_DIR")), environment_->dataDir());
        QCOMPARE(attached.value(QStringLiteral("CLASH_QT_SERVICE_SOCKET")), serviceSocket_);
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

    // --- where the startup status query actually goes -------------------------
    //
    // THE DEFECT THIS CASE EXISTS FOR. The settings page asks the privileged
    // service for its status as soon as it is built - ServiceSettings's
    // constructor queues checkStatus(), which reaches
    // PrivilegedServiceClient::requestStatus() - and until src/main.cpp was
    // given CLASH_QT_SERVICE_SOCKET that request went to
    // PrivilegedServiceClient::defaultSocketPath(), the machine-wide root
    // helper. The oversized-configuration guard in the case below does not
    // cover it: that guard stops core_process from calling startCore, and a
    // status query never goes near startCore. So every launch this suite made
    // was a connection attempt on the installed helper, and no case could see
    // it.
    //
    // WHAT IS ASSERTED, AND HOW IT CANNOT BE FAKED. A QLocalServer this test
    // owns is placed on the socket the child is given, and a SECOND one on a
    // different socket in the same scope. The application's status frame has to
    // arrive at the first and the second has to stay untouched:
    // PrivilegedServiceClient holds exactly one socketPath_ and connects to
    // that and nothing else, so a frame arriving here is proof of where the
    // whole client is pointed - the selection is observed, not assumed.
    //
    // The decoy is a SECOND ISOLATED SOCKET on purpose. The negative direction
    // of this claim is never proven by pointing anything at the real helper.

    void theStartupServiceStatusGoesToTheSocketTheHostSelected() {
        if (!platform::PrivilegedServiceInstaller::isSupported()) {
            QSKIP("ServiceSettings::checkStatus() returns early where the privileged service is "
                  "unsupported, so this platform makes no startup status query to observe. The "
                  "socket is still selected for every child by childEnvironment().");
        }

        HelperFixture selected;
        QVERIFY2(selected.listen(serviceSocket_),
                 qPrintable(QStringLiteral("the helper fixture did not bind %1: %2")
                                .arg(serviceSocket_, selected.errorString())));
        const QString decoyPath = environment_->socketPath(QStringLiteral("decoy"));
        QVERIFY2(!decoyPath.isEmpty(), qPrintable(environment_->errorString()));
        HelperFixture decoy;
        QVERIFY2(decoy.listen(decoyPath), qPrintable(decoy.errorString()));

        const QString dataDir = environment_->dataDir();
        auto *app = launch(dataDir, {QStringLiteral("--no-autostart")});
        QVERIFY2(awaitRunning(app, dataDir), qPrintable(transcript(app)));

        QVERIFY2(wf::waitFor([&selected] {
                     return selected.commands().contains(QStringLiteral("status"));
                 }, 30000),
                 qPrintable(QStringLiteral(
                                "the application never asked THIS fixture for the privileged "
                                "service status. Either src/main.cpp stopped honouring "
                                "CLASH_QT_SERVICE_SOCKET - in which case the query went to the "
                                "machine-wide helper at %1 and this suite is touching a root "
                                "daemon again - or the settings page stopped querying on startup, "
                                "in which case this guard is inert and must be replaced rather "
                                "than deleted. Connections: %2, frames: %3. %4")
                                .arg(platform::PrivilegedServiceClient::defaultSocketPath())
                                .arg(selected.connections())
                                .arg(selected.transcript().join(QLatin1Char(' ')), transcript(app))));
        // One endpoint, not "somewhere on this machine": the decoy is an
        // equally valid local socket and nothing went to it.
        QCOMPARE(decoy.connections(), 0);
        QVERIFY2(decoy.commands().isEmpty(),
                 qPrintable(QStringLiteral("the application talked to a socket it was not given: "
                                           "%1").arg(decoy.transcript().join(QLatin1Char(' ')))));
        // The frame is the helper's protocol, so what arrived is a real client
        // request and not an accidental connect.
        QVERIFY2(selected.transcript().join(QLatin1Char(' ')).contains(QStringLiteral("\"protocol\":1")),
                 qPrintable(selected.transcript().join(QLatin1Char(' '))));
        // A fixture answer is taken as an answer: the process is still up.
        QVERIFY2(app->state() == QProcess::Running, qPrintable(transcript(app)));

        app->terminate();
        QVERIFY2(app->waitForFinished(15000), qPrintable(transcript(app)));
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
        // The socket and the data directory are decided HERE, for every launch,
        // from this test's own scope. Nothing about a child's isolation is left
        // to what the registration, the developer's shell or a previous case
        // happened to export.
        process->setProcessEnvironment(
            childEnvironment(dataDirEnv_.isEmpty() ? dataDir : dataDirEnv_, serviceSocket_,
                             controllerEndpoint_, controllerSecret_, moduleOverride_));
        QStringList arguments{QStringLiteral("--data-dir"), dataDir};
        arguments += extra;
        process->start(appBinary(), arguments);
        QProcess *raw = process.get();
        started_.push_back(std::move(process));
        return raw;
    }

    /// Everything the child has written so far, accumulated.
    ///
    /// readAll() is DESTRUCTIVE and the merged channel is the only place the
    /// application's own diagnostics appear, so a harness that read it in two
    /// places would lose whichever half the other one took first. Every read in
    /// this file goes through here.
    QString outputOf(QProcess *process) {
        output_[process] += process->readAll();
        return QString::fromUtf8(output_.value(process));
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

    QString transcript(QProcess *process) {
        const QString output = outputOf(process);
        return QStringLiteral("state=%1 exit=%2 output=%3")
            .arg(static_cast<int>(process->state()))
            .arg(process->exitCode())
            .arg(output.right(2000));
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
    QHash<QProcess *, QByteArray> output_;
    QByteArray preferenceDigestBefore_;
    QString enginePath_;
    /// Replaces CLASH_QT_MODULE_PATH for the child. Empty means "inherit what
    /// the registration staged", which is what every case but one wants.
    QString moduleOverride_;
    /// The helper socket every child of the current case is pointed at. Always
    /// set, always inside this test's scope, never the installed default.
    QString serviceSocket_;
    /// CLASH_QT_DATA_DIR for the child when it must differ from --data-dir.
    /// Empty means "the same directory the flag names", which is what every
    /// case but the precedence one wants.
    QString dataDirEnv_;
    /// Handed to the child as CLASH_QT_CONTROLLER/CLASH_QT_SECRET, which is how
    /// core::discoverEndpoint() is told where to look. A production input, not
    /// a test hook.
    QString controllerEndpoint_;
    QString controllerSecret_;
};

QTEST_GUILESS_MAIN(AppSmokeTest)
#include "app_smoke_test.moc"
