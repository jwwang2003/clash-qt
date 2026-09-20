#include <QApplication>
#include <QTimer>

#include "core/controller_discovery.h"
#include "app_context.h"
#include "core/enhance/config_enhancer.h"
#include "core/mihomo_client.h"
#include "core/process/core_process.h"
#include "core/profile/profile_store.h"
#include "platform/system/hotkeys.h"
#include "ui/main_window.h"
#include "ui/tray_icon.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("clash-qt");
    // Closing the window leaves the app alive in the tray.
    QApplication::setQuitOnLastWindowClosed(false);

    auto *client = new core::MihomoClient(&app);
    auto *profiles = new core::ProfileStore(&app);
    auto *coreProcess = new core::CoreProcess(&app);
    auto *enhancer = new core::ConfigEnhancer(&app);
    auto *hotkeys = new platform::Hotkeys(&app);

    // A core we launch ourselves wins; otherwise attach to whatever is already
    // running, which is what makes the app usable before stage 2 lands.
    QObject::connect(coreProcess, &core::CoreProcess::ready, client,
                     &core::MihomoClient::setEndpoint);
    QObject::connect(profiles, &core::ProfileStore::runtimeConfigReady, coreProcess,
                     [coreProcess, profiles](const QString &path) {
                         coreProcess->start(path, profiles->dataDir());
                     });

    client->setEndpoint(core::discoverEndpoint());
    profiles->load();

    enhancer->load();

    const app::Context context{client, profiles, coreProcess, enhancer, hotkeys};
    auto *window = new ui::MainWindow(context);
    auto *tray = new ui::TrayIcon(window, &app);
    tray->show();

    QObject::connect(client, &core::MihomoClient::trafficSample, tray, &ui::TrayIcon::setTraffic);

    client->fetchVersion();
    client->fetchProxies();
    client->openTrafficStream();

    // mihomo does not push proxy-state changes; poll for anything changed elsewhere.
    auto *poll = new QTimer(&app);
    QObject::connect(poll, &QTimer::timeout, client, &core::MihomoClient::fetchProxies);
    poll->start(5000);

    window->show();
    return app.exec();
}
