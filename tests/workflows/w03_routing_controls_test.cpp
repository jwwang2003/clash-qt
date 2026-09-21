// W03 - Routing controls.
//
// docs/TEST_STRATEGY.md, "Complete workflows":
//
//   Exercise  Start managed backend -> toggle from settings/toolbar/tray ->
//             reject one change -> quit
//   Outcome   All surfaces agree with confirmed state; failure is visible; only
//             owned proxy state is restored
//
// THE ONLY SUITE HERE THAT BUILDS THE SHELL. "All surfaces agree" is a claim
// about three widgets, so three widgets are built: the real ui::MainWindow with
// its real ui::SettingsPage and ui::RoutingControls, and the real ui::TrayIcon
// over that window's menu. They are driven the way a user drives them - the
// settings checkbox is checked, the tray action is triggered, the toolbar's
// intent slot is called - and then all three are READ BACK. A test that asked
// app::runtime::RoutingController three times would prove only that a getter is
// deterministic.
//
// NOTHING TOUCHES THE MACHINE'S PROXY, AND THAT IS ASSERTED, NOT ASSUMED.
// The controller is given a private platform::SystemProxyService whose single OS
// command is an in-process function. ui::SettingsPage, however, reaches
// platform::SystemProxyService::instance() directly for its descriptive status
// line - it takes no service parameter - so the process-global singleton exists
// in this test whether or not it is wanted. Its state is therefore asserted to
// be untouched at both ends of the journey: a default-constructed
// SystemProxyState means no OS operation ever completed on it. See the worker
// report; the page should take the service the controller was built with.
//
// TWO DIFFERENT TUN REFUSALS, AND THEY ARE NOT THE SAME THING. This header used
// to claim the managed journey drove "a TUN enable whose read-back disagrees".
// It did not, and the assertion under that claim - `tunChanges.empty()` - said
// so: nothing was ever sent, so no read-back ever happened. The two are now
// separate cases and each says which one it is.
//
//   1. THE PRE-SUBMISSION BLOCK, in the managed journey below. ui::MainWindow
//      decides an unprivileged managed core cannot have TUN and hands the reason
//      to the toolbar; app::runtime::RoutingController refuses BEFORE calling
//      the backend. `tunChanges.empty()` is the point of it: a blocked request
//      must not reach the engine at all. This refusal exists only once the shell
//      and the coordinators are assembled - neither half produces it alone.
//
//   2. THE READ-BACK DISAGREEMENT, in aTunReadBackThatDisagreesWith... below.
//      The request IS submitted, the controller accepts the PATCH, and the
//      re-read of /configs still says TUN is off. That is the contract's own
//      definition of a refusal (backend-r2: TunChangeCompleted::actual is a
//      read-back, never an echo), and it is what catches `actual` and
//      `requested` being swapped anywhere on the path. It is driven over an
//      EXTERNAL attachment, because the permission block in (1) would otherwise
//      stop the request before it could be submitted - so the case that means to
//      test the read-back has to be a case where nothing else says no first.
//
// Neither needs a test hook in production code.

#include <QtTest>

#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QMenu>
#include <QSignalSpy>

#include <memory>

#include "core/backend/backend_bridge.h"
#include "platform/proxy/system_proxy.h"
#include "platform/system/hotkeys.h"
#include "support/fake_core.h"
#include "support/loopback_server.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"
#include "ui/shell/main_window.h"
#include "ui/shell/routing_controls.h"
#include "ui/shell/tray_icon.h"
#include "ui/pages/settings/settings_page.h"
#include "workflows/composition_root_audit.h"
#include "workflows/workflow_support.h"

using testsupport::FakeCore;
using testsupport::LoopbackServer;
using testsupport::ScopedEnvironment;

namespace wf = workflows;
namespace cb = core::backend;

namespace {

// The controller fixture reports TUN as OFF for the whole journey. A PATCH that
// says otherwise is accepted at the transport level and contradicted by the
// read-back, which is exactly the shape of a refusal the contract describes.
constexpr auto kConfigsOff = R"({"mode":"rule","mixed-port":27890,"port":0,"socks-port":0,"tun":{"enable":false}})";

void scriptController(LoopbackServer &server) {
    using Reply = LoopbackServer::Reply;
    server.route("GET", "/version", Reply::json(R"({"version":"workflow-core-1.19.31"})"));
    server.route("GET", "/configs", Reply::json(kConfigsOff));
    server.route("PATCH", "/configs", Reply::json("{}"));
    server.route("GET", "/proxies",
                 Reply::json(R"({"proxies":{"GLOBAL":{"type":"Selector","now":"DIRECT","all":["DIRECT"]},"DIRECT":{"type":"Direct","now":""}}})"));
    server.route("GET", "/rules", Reply::json(R"({"rules":[{"type":"MATCH","payload":"","proxy":"DIRECT"}]})"));
    server.route("GET", "/providers/proxies", Reply::json(R"({"providers":{}})"));
    server.route("GET", "/providers/rules", Reply::json(R"({"providers":{}})"));
    // The assembled shell opens these as soon as its pages are built. An
    // unexpected handshake is refused by the fixture and recorded, so declaring
    // them is what keeps sawUnexpectedRequest() meaningful rather than noisy.
    for (const char *stream : {"/traffic", "/connections", "/logs", "/memory"})
        server.expectStream(QLatin1String(stream));
}

QAction *actionNamed(QMenu *menu, const QString &text) {
    if (!menu) return nullptr;
    for (QAction *action : menu->actions())
        if (action->text() == text) return action;
    return nullptr;
}

}  // namespace

class W03RoutingControlsTest : public QObject {
    Q_OBJECT

  private slots:

    void initTestCase() {
        // The process-global service the settings page reaches for on its own.
        // Recorded here so the journey can prove it was never driven.
        auto *real = platform::SystemProxyService::instance();
        QVERIFY2(!real->state().valid && !real->isBusy(),
                 "the real system proxy service was already active before this suite ran");
    }

    void init() {
        environment_ = std::make_unique<ScopedEnvironment>(QStringLiteral("w03"));
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
        auto *real = platform::SystemProxyService::instance();
        QVERIFY2(!real->state().valid,
                 "an OS proxy command completed on the real system proxy service: this test "
                 "changed the developer's machine");
        QVERIFY2(!real->isChanging(), "the real system proxy service has a change in flight");
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

    void everySurfaceAgreesWithConfirmedStateThroughAManagedSession() {
        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) {
            QSKIP(qPrintable(QStringLiteral(
                                 "W03 needs the generated controller address 127.0.0.1:%1, which "
                                 "is in use: %2. Not asserted rather than asserted weakly.")
                                 .arg(wf::kGeneratedControllerPort)
                                 .arg(relay.errorString())));
        }

        const QString engineDir = engineDirectory();
        FakeCore engine(engineDir);
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        QVERIFY(engine.validationSucceeds()
                    .printsLine(QStringLiteral("[INFO] up"))
                    .runsForever()
                    .commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        wf::AssembledApp app(proxyLog, osProxy);
        app.bootstrap(engine.binaryPath());

        // The shell, assembled the way src/main.cpp assembles it: the bridge
        // before the window, the window before the tray.
        cb::BackendBridge bridge(*app.backend);
        platform::Hotkeys hotkeys;
        const app::Context context{app.profiles.get(), app.enhancer.get(), &hotkeys,
                                   app.backups.get()};
        ui::MainWindow window(context, &bridge, app.routing.get());
        ui::TrayIcon tray(&window, nullptr);

        auto *toolbar = window.routingControls();
        QVERIFY2(toolbar, "the window built no routing controls");
        auto *settings = window.findChild<ui::SettingsPage *>();
        QVERIFY2(settings, "the window built no settings page");
        QMenu *trayMenu = tray.contextMenu();
        QVERIFY2(trayMenu, "the tray built no menu");
        QAction *trayProxy = actionNamed(trayMenu, QStringLiteral("System Proxy"));
        QAction *trayTun = actionNamed(trayMenu, QStringLiteral("TUN Mode"));
        QVERIFY2(trayProxy && trayTun, "the tray menu has no routing entries");
        // The settings checkbox carries no object name, so it is located by its
        // label. That is a weakness in the page, not in the journey - see the
        // worker report.
        QCheckBox *settingsProxy = checkBoxLabelled(
            settings, QStringLiteral("Use the connected core as the system proxy"));
        QVERIFY2(settingsProxy, "the settings page has no system-proxy checkbox");

        // ---- start a managed backend --------------------------------------
        const QString profile = createProfile(app, QStringLiteral("alpha"));
        QVERIFY(!profile.isEmpty());
        app.profiles->selectProfile(profile);
        QVERIFY(app.runtimeCoordinator->requestAutostart());
        QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; }),
                 qPrintable(report(app, controller)));
        QVERIFY(wf::waitFor([&app] { return app.backend->isConnected(); }));
        // THE WIRING GAP THIS JOURNEY FOUND, and the one line that works round
        // it. app::runtime::RoutingController::systemProxyAvailable() requires a
        // proxy TARGET, and the only code in the tree that ever sets one is
        // ui::SettingsPage::applySystemProxy() - which is reachable only from a
        // checkbox that renderSystemProxy() disables while availability is
        // false. src/main.cpp never calls setProxyTarget(). From a cold data
        // directory the system proxy is therefore unreachable from every
        // surface; the case below states that as a defect rather than hiding it
        // here. This line stands in for the composition-root wiring that is
        // missing, so the three surfaces can still be compared to each other.
        platform::ProxyConfig target;
        target.host = QStringLiteral("127.0.0.1");
        target.port = 27890;
        app.routing->setProxyTarget(target);
        QVERIFY2(wf::waitFor([&] { return settingsProxy->isEnabled(); }),
                 qPrintable(QStringLiteral("the system proxy control never became usable: %1")
                                .arg(report(app, controller))));

        const int restoresBefore = proxyLog->count(wf::ProxyAction::Restore);

        // ---- surface 1: settings -------------------------------------------
        // The only surface that knows the bypass list and the core's port, and
        // therefore the only one that can set the controller's proxy target.
        settingsProxy->setChecked(true);
        QVERIFY2(wf::waitFor([&app] { return app.routing->systemProxyEnabled(); }),
                 qPrintable(QStringLiteral("the settings toggle was never confirmed: %1")
                                .arg(proxyLog->transcript())));
        QCOMPARE(proxyLog->count(wf::ProxyAction::Enable), 1);
        QCOMPARE(osProxy->port, quint16(27890));
        assertAllSurfacesAgree(true, toolbar, settings, trayProxy, app);

        // ---- surface 2: the tray -------------------------------------------
        // trigger() flips the checkable action and emits triggered(checked),
        // which is the connection ui::TrayIcon makes to the toolbar's intent.
        QVERIFY(trayProxy->isChecked());
        trayProxy->trigger();
        QVERIFY2(wf::waitFor([&app] { return !app.routing->systemProxyEnabled(); }),
                 qPrintable(proxyLog->transcript()));
        QCOMPARE(proxyLog->count(wf::ProxyAction::Disable), 1);
        QCOMPARE(osProxy->port, quint16(0));
        assertAllSurfacesAgree(false, toolbar, settings, trayProxy, app);

        // ---- surface 3: the toolbar ----------------------------------------
        toolbar->requestSystemProxyChange(true);
        QVERIFY2(wf::waitFor([&app] { return app.routing->systemProxyEnabled(); }),
                 qPrintable(proxyLog->transcript()));
        QCOMPARE(proxyLog->count(wf::ProxyAction::Enable), 2);
        assertAllSurfacesAgree(true, toolbar, settings, trayProxy, app);

        // ---- refusal (1): the PRE-SUBMISSION block the shell computes --------
        // ui::MainWindow decides that an unprivileged managed core cannot have
        // TUN and hands the reason to the toolbar, which hands it to the
        // controller; the controller refuses the request BEFORE it reaches the
        // backend and raises it on its one error channel. Nothing is echoed as
        // applied on any surface. This is a rejection that only exists once the
        // shell and the coordinators are assembled - neither half produces it
        // alone.
        //
        // NOT a read-back disagreement. `tunChanges.empty()` below is the
        // assertion that the request never reached the engine, which is the
        // opposite of a change that was sent and then contradicted by a re-read.
        // That one is aTunReadBackThatDisagreesWithTheRequestIsNeverAppliedToASurface().
        QVERIFY2(!toolbar->tunEnabled(), "TUN was on before the journey asked for it");
        QVERIFY2(!app.routing->tunBlockedReason().isEmpty(),
                 "the shell never told the controller why TUN is blocked");
        int errorsBefore = app.routingErrors.size();
        toolbar->requestTunChange(true);
        QVERIFY2(wf::waitFor([&] { return app.routingErrors.size() > errorsBefore; }),
                 qPrintable(QStringLiteral("a refused TUN request was not reported: %1")
                                .arg(report(app, controller))));
        QVERIFY2(app.events.tunChanges.empty(),
                 "a request the controller refused was still sent to the engine");
        QVERIFY2(!app.routing->tunPending(), "a refused TUN request stayed pending");
        QVERIFY2(!app.routing->tunEnabled(), "a refused TUN change was reported as applied");
        QVERIFY2(!toolbar->tunEnabled(), "the toolbar rendered a refused TUN change as applied");
        QVERIFY2(!trayTun->isChecked(), "the tray rendered a refused TUN change as applied");
        QVERIFY2(!app.routing->lastError().isEmpty(), "the refusal left no error to display");
        // The refusal did not disturb the change that DID take.
        assertAllSurfacesAgree(true, toolbar, settings, trayProxy, app);

        // ---- reject one change: an OS proxy enable the machine refuses -------
        // The substituted OS command fails this one change. A failure is not a
        // pending change and it is not a success: every surface must still show
        // the last CONFIRMED value, which is off.
        trayProxy->trigger();
        QVERIFY2(wf::waitFor([&app] { return !app.routing->systemProxyEnabled(); }),
                 qPrintable(proxyLog->transcript()));
        assertAllSurfacesAgree(false, toolbar, settings, trayProxy, app);

        proxyLog->enableSucceeds = false;
        errorsBefore = app.routingErrors.size();
        const int enablesBeforeRefusal = proxyLog->count(wf::ProxyAction::Enable);
        toolbar->requestSystemProxyChange(true);
        QVERIFY2(wf::waitFor([&] { return app.routingErrors.size() > errorsBefore; }),
                 qPrintable(QStringLiteral("a refused system-proxy change was not reported: %1")
                                .arg(proxyLog->transcript())));
        QVERIFY2(proxyLog->count(wf::ProxyAction::Enable) > enablesBeforeRefusal,
                 "the journey never reached the OS command it meant to have refused");
        QVERIFY2(wf::waitFor([&app] { return !app.routing->systemProxyPending(); }),
                 "the refused change never settled");
        QCOMPARE(osProxy->port, quint16(0));
        assertAllSurfacesAgree(false, toolbar, settings, trayProxy, app);
        QVERIFY2(!app.routing->lastError().isEmpty(), "the refusal left no error to display");

        // ---- and back on, so the quit has owned state to restore -------------
        proxyLog->enableSucceeds = true;
        settingsProxy->setChecked(true);
        QVERIFY2(wf::waitFor([&app] { return app.routing->systemProxyEnabled(); }),
                 qPrintable(proxyLog->transcript()));
        assertAllSurfacesAgree(true, toolbar, settings, trayProxy, app);

        // ---- quit ------------------------------------------------------------
        const int disablesBefore = proxyLog->count(wf::ProxyAction::Disable);
        const int enablesBefore = proxyLog->count(wf::ProxyAction::Enable);
        QVERIFY2(app.quit(), qPrintable(QStringLiteral("the quit never completed: %1 | %2")
                                            .arg(app.blockingReason(), proxyLog->transcript())));
        QVERIFY(app.shutdownStarted);
        QVERIFY2(app.shutdownWarnings.isEmpty(),
                 qPrintable(app.shutdownWarnings.join(QLatin1Char('\n'))));

        // ONLY owned proxy state is restored: a restore, and not a blanket
        // disable of whatever the machine happened to have.
        QCOMPARE(proxyLog->count(wf::ProxyAction::Restore), restoresBefore + 1);
        QCOMPARE(proxyLog->count(wf::ProxyAction::Disable), disablesBefore);
        QCOMPARE(proxyLog->count(wf::ProxyAction::Enable), enablesBefore);
        QCOMPARE(osProxy->port, quint16(0));
        QVERIFY(app.shutdown->isSystemProxyStopped());
        QVERIFY(app.shutdown->wasLastStopConfirmed());
        QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()), "a child outlived the quit");
        QVERIFY2(!controller.sawUnexpectedRequest(), qPrintable(controller.redactedTranscript()));
    }

    // --- refusal (2): the read-back the contract defines ---------------------
    //
    // The change IS submitted. The controller accepts the PATCH with 200 and
    // then keeps answering GET /configs with `tun.enable: false`, which is a
    // core that took the request and could not create the interface - the
    // commonest real refusal there is. core::MihomoClient re-reads /configs
    // after the PATCH and reports what it FOUND, so TunChangeCompleted arrives
    // with requested=true and actual=false.
    //
    // WHY AN EXTERNAL ATTACHMENT AND NOT THE MANAGED JOURNEY ABOVE. On macOS the
    // shell blocks a TUN enable on an unprivileged managed core before it can be
    // submitted, so the journey above can never reach a read-back at all. Here
    // the composition root's other attachment is used - the one src/main.cpp
    // makes with `bridge.attach(backend.discoverEndpoint())` before any managed
    // core exists - and no child process is started. Nothing says no first, so
    // the read-back is the only thing that can refuse.
    //
    // WHAT WOULD FAIL THIS. Publishing `requested` where `actual` belongs,
    // anywhere on the path: MihomoClient::confirmTunChange, the
    // TunChangeCompleted the backend builds, or
    // RoutingController::tunChangeCompleted. Each of those would leave TUN
    // reading as ON on all three surfaces after a change that never took.

    void aTunReadBackThatDisagreesWithTheRequestIsNeverAppliedToASurface() {
        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        wf::AssembledApp app(proxyLog, osProxy);
        // No engine: this journey never starts a core. The binary path is left
        // empty on purpose, so a managed start would fail loudly rather than
        // quietly become the subject.
        app.bootstrap(QString());

        cb::BackendBridge bridge(*app.backend);
        platform::Hotkeys hotkeys;
        const app::Context context{app.profiles.get(), app.enhancer.get(), &hotkeys,
                                   app.backups.get()};
        ui::MainWindow window(context, &bridge, app.routing.get());
        ui::TrayIcon tray(&window, nullptr);

        auto *toolbar = window.routingControls();
        QVERIFY2(toolbar, "the window built no routing controls");
        QMenu *trayMenu = tray.contextMenu();
        QVERIFY2(trayMenu, "the tray built no menu");
        QAction *trayTun = actionNamed(trayMenu, QStringLiteral("TUN Mode"));
        QVERIFY2(trayTun, "the tray menu has no TUN entry");

        cb::Endpoint endpoint;
        endpoint.host = QStringLiteral("127.0.0.1");
        endpoint.port = controller.port();
        bridge.attach(endpoint);

        QVERIFY2(wf::waitFor([&app] { return app.backend->isConnected(); }),
                 qPrintable(report(app, controller)));
        // tunAvailable() means connected AND a configuration has been read, so
        // this is also the proof that the read-back channel is live before
        // anything is asked of it.
        QVERIFY2(wf::waitFor([&app] { return app.routing->tunAvailable(); }),
                 qPrintable(report(app, controller)));
        QVERIFY2(app.routing->tunBlockedReason().isEmpty(),
                 qPrintable(QStringLiteral(
                                "the shell blocked TUN on an attached controller (%1), so this "
                                "case would measure the pre-submission block instead of the "
                                "read-back it exists for")
                                .arg(app.routing->tunBlockedReason())));
        QVERIFY2(!app.routing->tunEnabled(), "the controller reported TUN already on");
        QVERIFY2(!toolbar->tunEnabled(), "the toolbar showed TUN on before anything asked");

        QSignalSpy confirmed(app.routing.get(),
                             &app::runtime::RoutingController::tunConfirmed);
        const int errorsBefore = app.routingErrors.size();
        const int patchesBefore = controller.requestCount("PATCH", QStringLiteral("/configs"));

        toolbar->requestTunChange(true);

        QVERIFY2(wf::waitFor([&app] { return !app.events.tunChanges.empty(); }),
                 qPrintable(QStringLiteral("no TUN completion arrived: %1")
                                .arg(report(app, controller))));
        // The request really was submitted - this is what separates case (2)
        // from the pre-submission block in the journey above.
        QVERIFY2(controller.requestCount("PATCH", QStringLiteral("/configs")) > patchesBefore,
                 qPrintable(QStringLiteral("the engine was never asked to change TUN: %1")
                                .arg(controller.redactedTranscript())));

        const cb::TunChangeCompleted &result = app.events.tunChanges.back();
        QCOMPARE(result.requested, true);
        QVERIFY2(!result.actual,
                 "TunChangeCompleted::actual is a READ-BACK: the controller answered "
                 "tun.enable=false after the PATCH, so publishing true here is the request "
                 "echoed back as though it had taken");
        QCOMPARE(result.status, cb::CompletionStatus::Failed);

        QVERIFY2(wf::waitFor([&app] { return !app.routing->tunPending(); }),
                 "the refused TUN change never settled");
        QVERIFY2(!app.routing->tunEnabled(),
                 "the controller reported a TUN change as applied although the read-back said "
                 "it was still off");
        QVERIFY2(!toolbar->tunEnabled(), "the toolbar rendered the disagreeing read-back as on");
        auto *rendered = toolbar->findChild<QCheckBox *>(QStringLiteral("tunSwitch"));
        QVERIFY2(rendered, "the toolbar has no tunSwitch");
        QVERIFY2(!rendered->isChecked(), "the toolbar's TUN switch rendered as on");
        QVERIFY2(!trayTun->isChecked(), "the tray rendered the disagreeing read-back as on");
        QCOMPARE(confirmed.count(), 0);

        // The failure is visible, and it is the engine's own explanation.
        QVERIFY2(app.routingErrors.size() > errorsBefore,
                 "a TUN change the core did not apply was never reported to the user");
        QVERIFY2(app.routingErrors.last().contains(QStringLiteral("still disabled")),
                 qPrintable(QStringLiteral("the reported error does not say what happened: %1")
                                .arg(app.routingErrors.last())));
        QCOMPARE(app.routing->lastError(), app.routingErrors.last());

        QVERIFY2(app.quit(), qPrintable(app.blockingReason()));
        QVERIFY2(!controller.sawUnexpectedRequest(), qPrintable(controller.redactedTranscript()));
    }

    // --- the gap the journey above had to work round -----------------------
    //
    // This case was written as an EXPECTED failure: on a cold data directory the
    // system proxy was unreachable from every surface, because the controller
    // reports it unavailable until a target is set, renderSystemProxy() disables
    // the checkbox while it is unavailable, and that checkbox was the only thing
    // that set a target. Publishing the target on every refresh broke the cycle,
    // the expected failure became an unexpected pass, and the case now asserts
    // the behaviour directly. Left as a named regression case because the
    // deadlock is invisible to every other test: each of them supplies a target.

    void aColdStartCanEnableTheSystemProxyFromTheSettingsSurface() {
        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) {
            QSKIP(qPrintable(QStringLiteral("W03 needs 127.0.0.1:%1: %2")
                                 .arg(wf::kGeneratedControllerPort)
                                 .arg(relay.errorString())));
        }

        FakeCore engine(engineDirectory());
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        QVERIFY(engine.validationSucceeds().printsLine(QStringLiteral("[INFO] up")).runsForever().commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        wf::AssembledApp app(proxyLog, osProxy);
        app.bootstrap(engine.binaryPath());

        cb::BackendBridge bridge(*app.backend);
        platform::Hotkeys hotkeys;
        const app::Context context{app.profiles.get(), app.enhancer.get(), &hotkeys,
                                   app.backups.get()};
        ui::MainWindow window(context, &bridge, app.routing.get());
        ui::TrayIcon tray(&window, nullptr);

        const QString profile = createProfile(app, QStringLiteral("alpha"));
        app.profiles->selectProfile(profile);
        QVERIFY(app.runtimeCoordinator->requestAutostart());
        QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; }),
                 qPrintable(report(app, controller)));
        QVERIFY(wf::waitFor([&app] { return app.backend->isConnected(); }));
        // The controller reported a usable proxy port, and the OS proxy state
        // has been read: everything the user could be asked for is in place.
        QVERIFY(wf::waitFor([&app] { return app.proxyService->state().valid; }));
        QVERIFY(wf::drainPostedTimers());

        auto *settings = window.findChild<ui::SettingsPage *>();
        QVERIFY(settings);
        QCheckBox *settingsProxy = checkBoxLabelled(
            settings, QStringLiteral("Use the connected core as the system proxy"));
        QVERIFY(settingsProxy);

        // Gated, not asserted outright. Availability arrives through a signal
        // chain that drainPostedTimers() does not guarantee has settled, and the
        // immediate form passed alone while failing inside a full lane run --
        // load-dependent, which is the worst kind of green. The cause is checked
        // before the rendered effect, and a condition that never holds still
        // fails when the deadline expires.
        QVERIFY2(wf::waitFor([&app] { return app.routing->systemProxyAvailable(); }),
                 "the controller must have a proxy target without the user toggling anything");
        QVERIFY2(wf::waitFor([settingsProxy] { return settingsProxy->isEnabled(); }),
                 "a cold start must be able to enable the system proxy from settings");

        QVERIFY2(app.quit(), qPrintable(app.blockingReason()));
        QVERIFY(wf::awaitNoLiveProcess(engine.binaryPath()));
    }

  private:
    /// The three surfaces, read back from the widgets rather than from the
    /// coordinator they share. `expected` is the CONFIRMED value.
    void assertAllSurfacesAgree(bool expected, ui::RoutingControls *toolbar,
                                ui::SettingsPage *settings, QAction *trayProxy,
                                wf::AssembledApp &app) {
        QVERIFY2(!app.routing->systemProxyPending(),
                 "a change was still in flight when the surfaces were compared");
        QCOMPARE(app.routing->systemProxyEnabled(), expected);
        QCOMPARE(settings->systemProxyEnabled(), expected);
        QCOMPARE(toolbar->systemProxyEnabled(), expected);
        // The toolbar's rendered switch, not only its accessor: the widget is
        // what a user sees.
        auto *rendered = toolbar->findChild<QCheckBox *>(QStringLiteral("systemProxySwitch"));
        QVERIFY2(rendered, "the toolbar has no systemProxySwitch");
        QCOMPARE(rendered->isChecked(), expected);
        QCOMPARE(trayProxy->isChecked(), expected);
    }

    static QCheckBox *checkBoxLabelled(QWidget *root, const QString &text) {
        for (QCheckBox *box : root->findChildren<QCheckBox *>())
            if (box->text() == text) return box;
        return nullptr;
    }

    QString engineDirectory() {
        const QString marker = environment_->filePath(QStringLiteral("engine/.keep"));
        const QString dir = QFileInfo(marker).absolutePath();
        QDir().mkpath(dir);
        return dir;
    }

    QString createProfile(wf::AssembledApp &app, const QString &name) {
        if (!app.profiles->createLocalProfile(name,
                                              QString::fromUtf8(wf::directOnlyProfile(0, name))))
            return {};
        for (const auto &profile : app.profiles->profiles())
            if (profile.name == name) return profile.uid;
        return {};
    }

    static QString report(wf::AssembledApp &app, LoopbackServer &controller) {
        return QStringLiteral("state=%1 connected=%2 | events: %3 | routing: %4 | %5")
            .arg(static_cast<int>(app.backend->state()))
            .arg(app.backend->isConnected())
            .arg(app.events.transcript(), app.routingErrors.join(QLatin1Char(' ')),
                 controller.pendingReport());
    }

    std::unique_ptr<ScopedEnvironment> environment_;
    QString enginePath_;
};

QTEST_MAIN(W03RoutingControlsTest)
#include "w03_routing_controls_test.moc"
