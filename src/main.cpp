#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QMenu>
#include <QStatusBar>
#include <QThreadPool>
#include <functional>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QTimer>

#include "core/mihomo/controller_discovery.h"
#include "app/app_context.h"
#include "core/config/enhance/config_enhancer.h"
#include "core/mihomo/mihomo_client.h"
#include "core/mihomo/process/core_process.h"
#include "core/preferences/preferences.h"
#include "core/profiles/profile_store.h"
#include "platform/system/hotkeys.h"
#include "platform/proxy/system_proxy_service.h"
#include "core/backups/backup_store.h"
#include "ui/shell/main_window.h"
#include "ui/shell/tray_icon.h"

// Keep the event loop alive while shutdown restores OS state and stops the
// child. QEvent::Quit also covers the native application menu and tray action.
class QuitGuard final : public QObject {
public:
    bool approved = false;
    std::function<void()> requested;
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == qApp && event->type() == QEvent::Quit && !approved) {
            event->ignore();
            if (requested) requested();
            return true;
        }
        return QObject::eventFilter(object, event);
    }
};

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

    bool quitting = false;
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

    auto *client = new core::MihomoClient(&app);
    auto *coreProcess = new core::CoreProcess(&app);
    auto *enhancer = new core::ConfigEnhancer(&app);
    auto *hotkeys = new platform::Hotkeys(&app);
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
    coreProcess->setBinaryPath(coreBinary);
    coreProcess->setUseService(core::preferences::open().value("core/useService", false).toBool());
    enhancer->load();
    profiles->setEnhancer(enhancer);
    profiles->load();
    QObject::disconnect(profileErrors);
    QObject::disconnect(chainErrors);

    QObject::connect(coreProcess, &core::CoreProcess::ready, client,
                     &core::MihomoClient::setEndpoint);
    QSet<QString> generatedSnapshots;
    const auto pruneSnapshots = [&](bool final) {
        const QStringList active = coreProcess->activeConfigPaths();
        QStringList obsolete;
        for (auto it = generatedSnapshots.begin(); it != generatedSnapshots.end();) {
            if (final || !active.contains(*it)) {
                obsolete.append(*it);
                it = generatedSnapshots.erase(it);
            } else ++it;
        }
        if (!obsolete.isEmpty()) QThreadPool::globalInstance()->start([obsolete] {
            for (const auto &path : obsolete) QFile::remove(path);
        });
    };
    QObject::connect(coreProcess, &core::CoreProcess::ready, &app,
                     [&pruneSnapshots] { pruneSnapshots(false); });
    QObject::connect(profiles, &core::ProfileStore::runtimeConfigReady, coreProcess,
                     [coreProcess, profiles, &quitting, &generatedSnapshots](const QString &path) {
                         if (QFileInfo(path).fileName().startsWith(".runtime-")) generatedSnapshots.insert(path);
                         if (quitting) return;
                         coreProcess->start(path, profiles->dataDir());
                     });
    auto *reload = new QTimer(&app);
    reload->setSingleShot(true);
    reload->setInterval(100);
    QObject::connect(reload, &QTimer::timeout, profiles, [profiles, coreProcess, &quitting] {
        if (quitting) return;
        if (coreProcess->state() != core::CoreState::Running &&
            coreProcess->state() != core::CoreState::Starting &&
            !(coreProcess->state() == core::CoreState::Stopping && coreProcess->isRestartPending())) return;
        if (profiles->currentUid().isEmpty()) {
            coreProcess->stop();
        } else {
            profiles->requestRuntimeConfig();
        }
    });
    const auto reloadManaged = [reload, coreProcess, &quitting] {
        if (quitting) return;
        if (coreProcess->state() == core::CoreState::Running ||
            coreProcess->state() == core::CoreState::Starting ||
            (coreProcess->state() == core::CoreState::Stopping && coreProcess->isRestartPending())) reload->start();
    };
    QObject::connect(profiles, &core::ProfileStore::currentProfileChanged, &app, reloadManaged);
    QObject::connect(profiles, &core::ProfileStore::profileUpdated, &app,
                     [profiles, reloadManaged](const QString &uid) {
                         if (uid == profiles->currentUid()) reloadManaged();
                     });
    QObject::connect(enhancer, &core::ConfigEnhancer::chainChanged, &app, reloadManaged);

    const app::Context context{client, profiles, coreProcess, enhancer, hotkeys};
    ui::MainWindow window(context);
    ui::TrayIcon tray(&window, &app);
    tray.show();
    QObject::connect(&instance, &QLocalServer::newConnection, &window, [&] {
        while (auto *socket = instance.nextPendingConnection()) {
            socket->deleteLater();
            window.show();
            window.raise();
            window.activateWindow();
        }
    });
    QObject::connect(client, &core::MihomoClient::trafficSample, &tray, &ui::TrayIcon::setTraffic);
    auto *proxyService = platform::SystemProxyService::instance();
    QuitGuard quitGuard;
    app.installEventFilter(&quitGuard);
    bool proxyStopped = false;
    bool coreStopped = false;
    int shutdownWarnings = 0;
    bool shutdownCleanupStarted = false;
    const auto backups = window.findChildren<core::BackupStore *>();
    const auto finishQuit = [&] {
        if (!quitting || !proxyStopped || !coreStopped || shutdownWarnings != 0) return;
        for (auto *backup : backups) if (backup->isBusy()) return;
        if (profiles->isRuntimeBusy() || profiles->isFileBusy() || enhancer->isFileBusy()) return;
        if (!shutdownCleanupStarted) {
            shutdownCleanupStarted = true;
            pruneSnapshots(true);
        }
        if (QThreadPool::globalInstance()->activeThreadCount() > 0) return;
        quitGuard.approved = true;
        QTimer::singleShot(0, &app, &QApplication::quit);
    };
    QObject::connect(coreProcess, &core::CoreProcess::stopFinished, &app,
                     [&](bool confirmed, const QString &error) {
        coreStopped = true;
        if (quitting && !confirmed) {
            ++shutdownWarnings;
            auto *message = new QMessageBox(QMessageBox::Warning, "Core Shutdown", error,
                                            QMessageBox::Ok, &window);
            message->setAttribute(Qt::WA_DeleteOnClose);
            QObject::connect(message, &QMessageBox::finished, &app, [&] {
                --shutdownWarnings;
                finishQuit();
            });
            message->open();
        }
        finishQuit();
    });
    QObject::connect(proxyService, &platform::SystemProxyService::shutdownFinished, &app,
                     [&](bool success, const QString &error) {
        proxyStopped = true;
        if (!success) {
            ++shutdownWarnings;
            auto *message = new QMessageBox(QMessageBox::Warning, "System Proxy", error,
                                            QMessageBox::Ok, &window);
            message->setAttribute(Qt::WA_DeleteOnClose);
            QObject::connect(message, &QMessageBox::finished, &app, [&] {
                --shutdownWarnings;
                finishQuit();
            });
            message->open();
        }
        coreProcess->stop();
        finishQuit();
    });
    for (auto *backup : backups)
        QObject::connect(backup, &core::BackupStore::busyChanged, &app, finishQuit);
    QTimer shutdownPoll;
    shutdownPoll.setInterval(50);
    QObject::connect(&shutdownPoll, &QTimer::timeout, &app, finishQuit);
    quitGuard.requested = [&] {
        if (quitting) return;
        quitting = true;
        app.setProperty("shuttingDown", true);
        coreStopped = false;
        reload->stop();
        profiles->beginShutdown();
        enhancer->beginShutdown();
        for (auto *backup : backups) backup->cancelAsync();
        window.setEnabled(false);
        if (tray.contextMenu()) tray.contextMenu()->setEnabled(false);
        window.statusBar()->showMessage(QObject::tr("Closing: restoring system proxy and stopping core…"));
        shutdownPoll.start();
        proxyService->shutdown();
    };
    const auto restoreProxy = [proxyService] { proxyService->restoreOwned(); };
    QObject::connect(proxyService, &platform::SystemProxyService::restoreFinished, client,
                     [client](bool success, const QString &error) {
        if (!success) emit client->errorOccurred(QObject::tr("Could not restore the system proxy: %1").arg(error));
    });
    QObject::connect(coreProcess, &core::CoreProcess::stateChanged, &app,
                     [coreProcess, restoreProxy](core::CoreState state) {
        if (state != core::CoreState::Stopped && state != core::CoreState::Failed) return;
        QTimer::singleShot(0, coreProcess, [coreProcess, restoreProxy] {
            if (coreProcess->state() == core::CoreState::Stopped ||
                coreProcess->state() == core::CoreState::Failed)
                restoreProxy();
        });
    });
    QObject::connect(client, &core::MihomoClient::connectedChanged, &app, [client, restoreProxy](bool connected) {
        if (connected) return;
        QTimer::singleShot(5000, client, [client, restoreProxy] {
            if (!client->isConnected()) restoreProxy();
        });
    });

    client->setEndpoint(core::discoverEndpoint());
    client->openTrafficStream();
    auto *poll = new QTimer(&app);
    QObject::connect(poll, &QTimer::timeout, client, [client] {
        client->fetchVersion();
        client->fetchProxies();
    });
    poll->start(5000);
    window.show();
    if (!startupErrors.isEmpty())
        QMessageBox::warning(&window, "Could not load saved configuration", startupErrors.join('\n'));
    if (!parser.isSet("no-autostart") && !profiles->currentUid().isEmpty() &&
        core::preferences::open().value("startup/startCore", false).toBool()) {
        QTimer::singleShot(0, profiles, &core::ProfileStore::requestRuntimeConfig);
    }
    const int result = app.exec();
    // Shutdown callbacks capture locals above; disconnect before their lifetime
    // ends and before the application-owned services are destroyed.
    QObject::disconnect(coreProcess, nullptr, &app, nullptr);
    QObject::disconnect(proxyService, nullptr, &app, nullptr);
    for (auto *backup : backups) QObject::disconnect(backup, nullptr, &app, nullptr);
    return result;
}
