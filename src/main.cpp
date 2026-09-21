// The composition root.
//
// Everything here either (a) constructs a long-lived object and hands it to
// whoever needs it, or (b) belongs to inventory group A - startup, single
// instance and settings bootstrap - for which no service class exists. Nothing
// else lives in this file: the reload gate is app/runtime, the routing intent is
// app/runtime, the quit gate is app/lifecycle and the backup rules are
// app/backup.
//
// CONSTRUCTION ORDER, and therefore destruction order in reverse. Each line
// needs the one above it, and the coordinators are all destroyed BEFORE the
// backend they observe - which is what makes a pending single-shot restore, an
// observer registration and a queued backend event impossible to outlive their
// subject.
//
//   platform::PrivilegedServiceClient      the privileged seam's transport
//   core::PrivilegedServiceClientAdapter   -> core::PrivilegedCoreService (D2)
//   core::MihomoBackendImpl                owns its client and its process
//   core::backend::BackendBridge           the Qt view of the backend (G2)
//   app::runtime::ProfileStoreConfigSource
//   app::runtime::RuntimeCoordinator       reload gate, snapshot retention
//   app::runtime::RoutingController        system proxy, TUN, mode
//   app::lifecycle::QuitGuard / ports / ShutdownCoordinator
//   app::backup::BackupCoordinator         owns the one core::BackupStore
//   ui::MainWindow, ui::TrayIcon           after every coordinator

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

#include "app/app_context.h"
#include "app/backup/backup_coordinator.h"
#include "app/backup/backup_store_session.h"
#include "app/composition/privileged_service_adapter.h"
#include "app/lifecycle/quit_guard.h"
#include "app/lifecycle/shutdown_coordinator.h"
#include "app/lifecycle/shutdown_ports.h"
#include "app/runtime/profile_config_source.h"
#include "app/runtime/routing_controller.h"
#include "app/runtime/runtime_coordinator.h"
#include "core/backend/backend_bridge.h"
#include "core/config/enhance/config_enhancer.h"
#include "core/mihomo/controller_discovery.h"
#include "core/mihomo/mihomo_backend.h"
#include "core/preferences/preferences.h"
#include "core/profiles/profile_store.h"
#include "platform/proxy/system_proxy_service.h"
#include "platform/system/hotkeys.h"
#include "ui/shell/main_window.h"
#include "ui/shell/tray_icon.h"

namespace {

namespace cb = core::backend;
namespace lifecycle = app::lifecycle;
namespace runtime = app::runtime;

}  // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("clash-qt");
    QApplication::setApplicationVersion("0.1.0");
#ifdef Q_OS_LINUX
    QGuiApplication::setDesktopFileName("clash-qt");
#endif
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription("Native desktop manager for mihomo");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"data-dir", "Use a separate data and settings directory.", "directory"});
    parser.addOption({"no-autostart", "Do not start the managed core on launch."});
    parser.process(app);
    if (parser.isSet("data-dir"))
        qputenv("CLASH_QT_DATA_DIR", QDir(parser.value("data-dir")).absolutePath().toUtf8());
    // No QSettings global state is configured here. setDefaultFormat() and
    // setPath() cannot redirect QSettings(organization, application) on macOS -
    // that constructor is hard wired to NativeFormat - so this bootstrap used
    // to leave the native store live while appearing to isolate it. The
    // redirection now lives in core::preferences, which reads
    // CLASH_QT_DATA_DIR (set just above from --data-dir) on every access.

    // ------------------------------------------------- group A: single instance
    auto *profiles = new core::ProfileStore(&app);
    const QString instanceName = "clash-qt-" + QString::fromLatin1(
        QCryptographicHash::hash(profiles->dataDir().toUtf8(), QCryptographicHash::Sha256).toHex().left(20));
    QLockFile lock(profiles->dataDir() + "/instance.lock");
    if (!lock.tryLock()) {
        QLocalSocket existing;
        existing.connectToServer(instanceName);
        if (existing.waitForConnected(1000)) {
            existing.write("show");
            existing.waitForBytesWritten(1000);
            return 0;
        }
        QMessageBox::warning(nullptr, "clash-qt", "Another instance is using this data directory.");
        return 1;
    }
    QLocalServer::removeServer(instanceName);
    QLocalServer instance;
    instance.setSocketOptions(QLocalServer::UserAccessOption);
    instance.listen(instanceName);

    // ------------------------------------------------------------ the engine
    // The composition root owns the privileged client and adapts it to the seam
    // the backend publishes (D2). Without this injection the backend falls back
    // to NullPrivilegedCoreService, whose isSupported() is false, and service
    // mode is silently unavailable: setExecutionMode(PrivilegedService) returns
    // false and the user's saved core/useService preference is discarded
    // without a word.
    //
    // Declared before the backend so it is destroyed after it: the adapter must
    // outlive every object that holds the seam.
    platform::PrivilegedServiceClient privilegedClient;
    core::PrivilegedServiceClientAdapter privilegedService(&privilegedClient);
    core::MihomoBackendImpl backend(&privilegedService);
    cb::BackendBridge bridge(backend);

    auto *enhancer = new core::ConfigEnhancer(&app);
    auto *hotkeys = new platform::Hotkeys(&app);

    // ------------------------------------------- group A: settings bootstrap
    QStringList startupErrors;
    const auto profileErrors = QObject::connect(profiles, &core::ProfileStore::errorOccurred, &app,
        [&startupErrors](const QString &message) { startupErrors.append(message); });
    const auto chainErrors = QObject::connect(enhancer, &core::ConfigEnhancer::errorOccurred, &app,
        [&startupErrors](const QString &message) { startupErrors.append(message); });
    // An explicit user setting wins; CLASH_QT_CORE_BINARY is the development
    // fallback that lets "make run" point the app at the locally built engine
    // without writing to the user's settings.
    QString coreBinary = core::preferences::open().value("core/binary").toString();
    if (coreBinary.isEmpty())
        coreBinary = qEnvironmentVariable("CLASH_QT_CORE_BINARY");
    backend.setBinaryPath(coreBinary);
    backend.setExecutionMode(core::preferences::open().value("core/useService", false).toBool()
                                 ? cb::ExecutionMode::PrivilegedService
                                 : cb::ExecutionMode::Managed);
    enhancer->load();
    profiles->setEnhancer(enhancer);
    // Where a runtime config seeds its geo data from. An existing Clash Verge
    // Rev install keeps Country.mmdb, geoip.dat and geosite.dat beside the
    // config.yaml that discoverEndpoint() already reads, and copying them on a
    // first generation is what saves a new user a 29 MB download.
    //
    // The store used to work this path out itself, by calling
    // core::vergeConfigPath() from profile_store.cpp. One path string made
    // clash_profiles link the component-private engine library, and because
    // link edges propagate, every consumer of clash_profiles -- this
    // application included -- reached the private implementation and was handed
    // Qt WebSockets with it (PRE-ARCH edge 1 / E1-profiles-links-mihomo-impl).
    // The knowledge belongs here: the composition root is already the one place
    // that knows which engine this process is managing.
    //
    // Set before anything can generate: the first generation is driven either
    // by RuntimeCoordinator (constructed below) or by the shell (constructed
    // after it), and neither exists yet.
    profiles->setSeedDir(QFileInfo(core::vergeConfigPath()).absolutePath());
    profiles->load();
    QObject::disconnect(profileErrors);
    QObject::disconnect(chainErrors);

    // ----------------------------------------------------------- coordinators
    auto *proxyService = platform::SystemProxyService::instance();

    runtime::ProfileStoreConfigSource configs(profiles);
    runtime::RuntimeCoordinator runtimeCoordinator(backend, configs);
    runtime::RoutingController routing(backend, proxyService);

    lifecycle::QuitGuard quitGuard;
    lifecycle::FunctionProxyShutdown proxyShutdown([proxyService] { proxyService->shutdown(); });
    lifecycle::GlobalThreadPoolDrain drain;
    lifecycle::ShutdownCoordinator shutdown(backend, proxyShutdown, drain);

    app::backup::StoreHooks profileHooks{
        [profiles](bool enabled) { profiles->setMaintenanceMode(enabled); },
        [profiles] { return profiles->isFileBusy(); },
        [profiles] { profiles->load(); }};
    // Order matters on a restore: the enhancement chain is reloaded before the
    // profiles that run it (old ui/backup_page.cpp:184-185). The coordinator
    // keeps that order; these hooks only say how to perform each half.
    app::backup::StoreHooks enhancerHooks{
        [enhancer](bool enabled) { enhancer->setMaintenanceMode(enabled); },
        [enhancer] { return enhancer->isFileBusy(); },
        [enhancer] { enhancer->load(); }};
    app::backup::BackupCoordinator backups(
        std::make_unique<app::backup::BackupStoreSession>(profiles->dataDir()), backend,
        profileHooks, enhancerHooks, &shutdown);

    // ---------------------------------------------------- reload and retention
    QObject::connect(profiles, &core::ProfileStore::runtimeConfigReady, &runtimeCoordinator,
                     [&runtimeCoordinator](const QString &path) {
                         runtimeCoordinator.onRuntimeConfigReady(path);
                     });
    QObject::connect(profiles, &core::ProfileStore::currentProfileChanged, &runtimeCoordinator,
                     [&runtimeCoordinator] { runtimeCoordinator.scheduleReload(); });
    QObject::connect(profiles, &core::ProfileStore::profileUpdated, &runtimeCoordinator,
                     [&runtimeCoordinator](const QString &uid) {
                         runtimeCoordinator.onProfileUpdated(uid);
                     });
    QObject::connect(enhancer, &core::ConfigEnhancer::chainChanged, &runtimeCoordinator,
                     [&runtimeCoordinator] { runtimeCoordinator.scheduleReload(); });

    // ------------------------------------------------------------- the quit gate
    // Gate order is the order main.cpp used to evaluate them in, and the first
    // busy one names the block.
    shutdown.addBusyGate("backup", [&backups] { return backups.isBusy(); });
    shutdown.addBusyGate("profile-runtime", [profiles] { return profiles->isRuntimeBusy(); });
    shutdown.addBusyGate("profile-files", [profiles] { return profiles->isFileBusy(); });
    shutdown.addBusyGate("enhancer-files", [enhancer] { return enhancer->isFileBusy(); });

    // RuntimeCoordinator::beginShutdown() FIRST, and as a quit action rather
    // than a bare reload->stop(). It stops the debounce timer AND sets the
    // quitting_ short circuit; without it onRuntimeConfigReady would still
    // start a core from a configuration generated during shutdown.
    shutdown.addQuitAction("runtime", [&runtimeCoordinator] { runtimeCoordinator.beginShutdown(); });
    shutdown.addQuitAction("profiles", [profiles] { profiles->beginShutdown(); });
    shutdown.addQuitAction("enhancer", [enhancer] { enhancer->beginShutdown(); });
    shutdown.addQuitAction("backups", [&backups] { backups.cancel(); });
    // pruneAllSnapshots(), not pruneSnapshots(true): the final prune is its own
    // named operation and is idempotent in itself.
    shutdown.setFinalCleanup([&runtimeCoordinator] { runtimeCoordinator.pruneAllSnapshots(); });
    shutdown.setQuitGuard(&quitGuard);
    app.installEventFilter(&quitGuard);

    QObject::connect(&quitGuard, &lifecycle::QuitGuard::quitRequested, &shutdown,
                     &lifecycle::ShutdownCoordinator::requestQuit);
    QObject::connect(proxyService, &platform::SystemProxyService::shutdownFinished, &shutdown,
                     &lifecycle::ShutdownCoordinator::onProxyShutdownFinished);
    QObject::connect(&backups, &app::backup::BackupCoordinator::busyChanged, &shutdown,
                     &lifecycle::ShutdownCoordinator::reevaluate);
    QObject::connect(profiles, &core::ProfileStore::runtimeBusyChanged, &shutdown,
                     &lifecycle::ShutdownCoordinator::reevaluate);
    QObject::connect(profiles, &core::ProfileStore::fileBusyChanged, &shutdown,
                     &lifecycle::ShutdownCoordinator::reevaluate);
    QObject::connect(profiles, &core::ProfileStore::fileBusyChanged, &backups,
                     &app::backup::BackupCoordinator::onFileBusyChanged);
    QObject::connect(enhancer, &core::ConfigEnhancer::fileBusyChanged, &shutdown,
                     &lifecycle::ShutdownCoordinator::reevaluate);
    QObject::connect(enhancer, &core::ConfigEnhancer::fileBusyChanged, &backups,
                     &app::backup::BackupCoordinator::onFileBusyChanged);
    QObject::connect(&shutdown, &lifecycle::ShutdownCoordinator::quitApproved, &app,
                     &QApplication::quit);

    // ------------------------------------------------------------------- shell
    const app::Context context{profiles, enhancer, hotkeys, &backups};
    ui::MainWindow window(context, &bridge, &routing);
    ui::TrayIcon tray(&window, &app);
    tray.show();

    // The coordinator owns no widget: the shell is what disables itself and what
    // presents a warning.
    QObject::connect(&shutdown, &lifecycle::ShutdownCoordinator::shutdownStarted, &window,
                     [&window, &tray](const QString &message) {
        window.setEnabled(false);
        if (tray.contextMenu()) tray.contextMenu()->setEnabled(false);
        window.statusBar()->showMessage(message);
    });
    // A warning holds the quit open until it is acknowledged. Without this
    // connection an unconfirmed core stop or a failed proxy restore would leave
    // ShutdownCoordinator waiting on a dialog nobody ever showed, and the app
    // would never quit.
    QObject::connect(&shutdown, &lifecycle::ShutdownCoordinator::warningRaised, &window,
                     [&shutdown, &window](quint64 id, const QString &title, const QString &message) {
        auto *dialog = new QMessageBox(QMessageBox::Warning, title, message, QMessageBox::Ok,
                                       &window);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(dialog, &QMessageBox::finished, &shutdown,
                         [&shutdown, id] { shutdown.dismissWarning(id); });
        dialog->open();
    });

    QObject::connect(&instance, &QLocalServer::newConnection, &window, [&] {
        while (auto *socket = instance.nextPendingConnection()) {
            socket->deleteLater();
            window.show();
            window.raise();
            window.activateWindow();
        }
    });
    QObject::connect(&bridge, &cb::BackendBridge::trafficSample, &tray, &ui::TrayIcon::setTraffic);
    // RoutingController::errorOccurred is the single routing error channel, and
    // the only carrier of "Could not restore the system proxy: %1". Neither
    // RoutingControls nor SettingsPage republishes it, so this is its one
    // consumer - and reporting it exactly once is why they do not.
    QObject::connect(&routing, &runtime::RoutingController::errorOccurred, &window,
                     [&window](const QString &message) {
        window.statusBar()->showMessage(message, 8000);
    });

    // ------------------------------------------------------ startup attachment
    // The external controller this process attaches to before any managed core
    // exists, and the traffic stream the tray and the home page read. A managed
    // core that becomes ready overwrites this attachment from
    // RuntimeCoordinator::coreReady.
    bridge.attach(backend.discoverEndpoint());
    bridge.openTrafficStream();

    // The controller poll stays in the composition root. fetchVersion() is the
    // only call that reaches setConnected(true): it is the process's sole
    // liveness probe, and the contract's re-issue obligation is edge-triggered,
    // never periodic. On the stack, so it stops when main() returns rather than
    // firing at a half-destroyed backend during QApplication teardown.
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &bridge, [&bridge] {
        bridge.refreshVersion();
        bridge.refreshProxies();
    });
    poll.start(5000);

    window.show();
    if (!startupErrors.isEmpty())
        QMessageBox::warning(&window, "Could not load saved configuration", startupErrors.join('\n'));
    // The two gates the composition root owns; the selected-profile gate and the
    // deferral into the event loop belong to RuntimeCoordinator.
    if (!parser.isSet("no-autostart") &&
        core::preferences::open().value("startup/startCore", false).toBool()) {
        runtimeCoordinator.requestAutostart();
    }
    // No hand-written disconnects after exec(): nothing below captures a stack
    // local into a connection that outlives it. Every object above is destroyed
    // in reverse declaration order, which puts the shell first, then the
    // coordinators, then the bridge, then the backend.
    return app.exec();
}
