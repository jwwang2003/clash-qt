#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QtEndian>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QTimer>
#include <yaml-cpp/yaml.h>

#include "core/config/enhance/config_enhancer.h"
#include "core/mihomo/process/core_process.h"
#include "core/profiles/profile_store.h"
#include "core/config/yaml_util.h"
#include "platform/service/privileged_service_client.h"

namespace {
void write(const QString &path, const QByteArray &body) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(body), body.size());
}
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
}

class RuntimeTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        directory_ = std::make_unique<QTemporaryDir>();
        QVERIFY(directory_->isValid());
        qputenv("CLASH_QT_DATA_DIR", directory_->path().toUtf8());
    }
    void cleanup() {
        qunsetenv("CLASH_QT_DATA_DIR");
        directory_.reset();
    }
    void runtimeAppliesEnhancementsAndProtectsController() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        store.setEnhancer(&enhancer);
        const QString input = directory_->filePath("sample.yaml");
        write(input, "proxies: []\nprofile: false\nsecret: malicious\nport: 1234\n");
        QSignalSpy selected(&store, &core::ProfileStore::currentProfileChanged);
        store.importFromFile(input);
        QCOMPARE(selected.size(), 1);
        enhancer.addScript("use profile name");
        write(enhancer.chain().first().filePath,
              "function main(c, name) { console.log(name); c.mode = 'global'; "
              "c.secret = 'wrong'; c['external-controller'] = '0.0.0.0:9999'; "
              "c['mixed-port'] = 1; c.profileName = name; return c; }");
        QVERIFY(store.setRuntimeOverrides({{"mixed-port", 28888}, {"mode", "direct"}}));
        QSignalSpy logs(&store, &core::ProfileStore::enhancementLog);
        const QString output = store.generateRuntimeConfig();
        QVERIFY(!output.isEmpty());
        const YAML::Node config = YAML::Load(read(output).toStdString());
        QCOMPARE(config["profileName"].as<std::string>(), std::string("sample"));
        QCOMPARE(config["mode"].as<std::string>(), std::string("direct"));
        QCOMPARE(config["mixed-port"].as<int>(), 28888);
        QCOMPARE(config["external-controller"].as<std::string>(), std::string("127.0.0.1:29097"));
        QVERIFY(config["secret"].as<std::string>() != "wrong");
        QVERIFY(config["profile"]["store-selected"].as<bool>());
        QVERIFY(!config["port"]);
        QCOMPARE(logs.size(), 1);
        QVERIFY(logs.first().first().toStringList().join('\n').contains("sample"));
        core::ProfileStore reopened;
        reopened.load();
        QCOMPARE(reopened.runtimeOverrides().value("mixed-port").toInt(), 28888);
        QCOMPARE(reopened.currentUid(), store.currentUid());
    }
    void runtimeTunDefaultsAreDisabledAndRoutable() {
        core::ProfileStore store;
        QVERIFY(store.createLocalProfile("defaults", "proxies: []\n"));
        QString path = store.generateRuntimeConfig();
        QVERIFY(!path.isEmpty());
        YAML::Node config = YAML::Load(read(path).toStdString());
        const YAML::Node tun = config["tun"];
        QVERIFY(tun.IsMap());
        QVERIFY(!tun["enable"].as<bool>());
        QCOMPARE(tun["stack"].as<std::string>(), std::string("mixed"));
        QVERIFY(tun["auto-route"].as<bool>());
        QVERIFY(tun["auto-detect-interface"].as<bool>());
        QVERIFY(!tun["dns-hijack"]);
        QVERIFY(!config["dns"]);
        QVERIFY(store.saveProfileContent(store.currentUid(), "proxies: []\ndns:\n  enable: true\n"));
        path = store.generateRuntimeConfig();
        config = YAML::Load(read(path).toStdString());
        QCOMPARE(config["tun"]["dns-hijack"].size(), size_t(2));
        QCOMPARE(config["tun"]["dns-hijack"][0].as<std::string>(), std::string("any:53"));
        QCOMPARE(config["tun"]["dns-hijack"][1].as<std::string>(), std::string("tcp://any:53"));
        QVERIFY(config["dns"]["enable"].as<bool>());
    }
    void runtimeTunPreservesProfileAndOverrideChoices() {
        core::ProfileStore store;
        QVERIFY(store.createLocalProfile("custom", "proxies: []\ndns:\n  enable: true\ntun:\n"
            "  enable: false\n  stack: gvisor\n  auto-route: false\n"
            "  auto-detect-interface: false\n  dns-hijack: []\n  mtu: 1280\n"));
        YAML::Node config = YAML::Load(read(store.generateRuntimeConfig()).toStdString());
        QVERIFY(!config["tun"]["enable"].as<bool>());
        QCOMPARE(config["tun"]["stack"].as<std::string>(), std::string("gvisor"));
        QVERIFY(!config["tun"]["auto-route"].as<bool>());
        QVERIFY(!config["tun"]["auto-detect-interface"].as<bool>());
        QCOMPARE(config["tun"]["dns-hijack"].size(), size_t(0));
        QVERIFY(store.setRuntimeOverrides({{"tun", QJsonObject{{"enable", true}, {"stack", "system"},
            {"auto-route", false}, {"auto-detect-interface", false}, {"mtu", 1400},
            {"dns-hijack", QJsonArray{"tcp://any:5353"}}}}}));
        config = YAML::Load(read(store.generateRuntimeConfig()).toStdString());
        QVERIFY(config["tun"]["enable"].as<bool>());
        QCOMPARE(config["tun"]["stack"].as<std::string>(), std::string("system"));
        QVERIFY(!config["tun"]["auto-route"].as<bool>());
        QVERIFY(!config["tun"]["auto-detect-interface"].as<bool>());
        QCOMPARE(config["tun"]["mtu"].as<int>(), 1400);
        QCOMPARE(config["tun"]["dns-hijack"].size(), size_t(1));
        QCOMPARE(config["tun"]["dns-hijack"][0].as<std::string>(), std::string("tcp://any:5353"));
    }
    void malformedTunDoesNotReplaceRuntime() {
        core::ProfileStore store;
        QVERIFY(store.createLocalProfile("invalid tun", "proxies: []\n"));
        const QString path = store.generateRuntimeConfig();
        const QByteArray good = read(path);
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);
        QVERIFY(store.saveProfileContent(store.currentUid(), "proxies: []\ntun: false\n"));
        QVERIFY(store.generateRuntimeConfig().isEmpty());
        QCOMPARE(read(path), good);
        QVERIFY(errors.last().first().toString().contains("TUN"));
        QVERIFY(store.saveProfileContent(store.currentUid(), "proxies: []\n"));
        QVERIFY(store.setRuntimeOverrides({{"tun", QJsonArray{}}}));
        QVERIFY(store.generateRuntimeConfig().isEmpty());
        QCOMPARE(read(path), good);
    }
    void failedScriptPreservesConfigAndContinues() {
        core::ConfigEnhancer enhancer;
        enhancer.addScript("broken");
        write(enhancer.chain().first().filePath,
              "function main(c) { c.mode = 'global'; console.log('before'); throw Error('oops'); }");
        enhancer.addMerge("next");
        write(enhancer.chain().last().filePath, "IPV6: true\n");
        const auto result = enhancer.apply("proxies: []\nmode: rule\n", "name");
        QVERIFY(result.error.contains("oops"));
        QVERIFY(result.logs.join('\n').contains("before"));
        const YAML::Node config = YAML::Load(result.yaml.toStdString());
        QCOMPARE(config["mode"].as<std::string>(), std::string("rule"));
        QVERIFY(config["ipv6"].as<bool>());
    }
    void invalidEditAndRuntimePreserveFiles() {
        core::ProfileStore store;
        const QString input = directory_->filePath("sample.yaml");
        write(input, "proxies: []\n");
        store.importFromFile(input);
        const QString path = store.profiles().first().filePath;
        const QByteArray original = read(path);
        QVERIFY(!store.saveProfileContent(store.currentUid(), "[]"));
        QCOMPARE(read(path), original);
        QVERIFY(store.saveProfileContent(store.currentUid(), "proxies: []\nmode: direct\n"));
        const QString runtime = store.generateRuntimeConfig();
        const QByteArray generated = read(runtime);
        write(path, "[]");
        QVERIFY(store.generateRuntimeConfig().isEmpty());
        QCOMPARE(read(runtime), generated);
        QVERIFY(!store.setRuntimeOverrides({{"mixed-port", 0}}));
        QVERIFY(!store.setRuntimeOverrides({{"mixed-port", 3.5}}));
    }
    void cyclicYamlIsRejectedWithoutRecursingForever() {
        const YAML::Node cyclic = YAML::Load("proxies: []\ncycle: &loop [*loop]\n");
        QVERIFY(core::yamlutil::dump(cyclic).empty());
        core::ConfigEnhancer enhancer;
        enhancer.addScript("identity");
        const auto result = enhancer.apply("proxies: []\ncycle: &loop [*loop]\n");
        QVERIFY(!result.error.isEmpty());
    }
    void rejectsNonHttpSubscriptions() {
        core::ProfileStore store;
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);
        store.importFromUrl("file:///etc/passwd");
        store.importFromUrl("https://");
        QCOMPARE(errors.size(), 2);
        QVERIFY(store.profiles().isEmpty());
    }
    void subscriptionUrlEditPreservesCacheAndRejectsOldRefresh() {
        core::ProfileStore store;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString originalUrl = QString("http://127.0.0.1:%1/original?token=old").arg(server.serverPort());
        const QString replacementUrl = QString("http://127.0.0.1:%1/replacement?token=new").arg(server.serverPort());
        const QByteArray cached = "proxies: []\nmode: rule\n";
        auto respond = [](QTcpSocket *socket, const QByteArray &body, const QByteArray &usage) {
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                          "\r\nSubscription-Userinfo: " + usage +
                          "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        };
        store.importFromUrl(originalUrl, "Subscription");
        QTRY_VERIFY(server.hasPendingConnections());
        QTcpSocket *initial = server.nextPendingConnection();
        QTRY_VERIFY(initial->bytesAvailable() > 0);
        initial->readAll();
        respond(initial, cached, "download=50; total=1000");
        QTRY_COMPARE(store.profiles().size(), 1);
        const core::Profile original = store.profiles().first();
        store.updateProfile(original.uid);
        QTRY_VERIFY(server.hasPendingConnections());
        QTcpSocket *stale = server.nextPendingConnection();
        QTRY_VERIFY(stale->bytesAvailable() > 0);
        QVERIFY(stale->readAll().contains("/original?token=old"));
        QSignalSpy updated(&store, &core::ProfileStore::profileUpdated);
        QSignalSpy changed(&store, &core::ProfileStore::profilesChanged);
        QVERIFY(store.setSubscriptionUrl(original.uid, "  " + replacementUrl + "  "));
        // Deliver the old response after the edit; it must never replace the cache.
        respond(stale, "proxies: []\nmode: global\n", "download=999; total=9999");
        QTRY_COMPARE(stale->state(), QAbstractSocket::UnconnectedState);
        QCoreApplication::processEvents();
        QCOMPARE(changed.size(), 1);
        QVERIFY(updated.isEmpty());
        QCOMPARE(store.currentUid(), original.uid);
        QCOMPARE(store.profiles().first().uid, original.uid);
        QCOMPARE(store.profiles().first().url, replacementUrl);
        QCOMPARE(store.profiles().first().name, original.name);
        QCOMPARE(store.profiles().first().updated, original.updated);
        QCOMPARE(store.profiles().first().subscription.download, quint64(50));
        QCOMPARE(read(original.filePath), cached);
        QVERIFY(!store.setSubscriptionUrl(original.uid, "file:///not-a-subscription"));
        QVERIFY(!store.setSubscriptionUrl(original.uid, "https://"));
        QCOMPARE(store.profiles().first().url, replacementUrl);
        core::ProfileStore reopened;
        reopened.load();
        QCOMPARE(reopened.profiles().size(), 1);
        QCOMPARE(reopened.profiles().first().url, replacementUrl);
        QCOMPARE(reopened.profiles().first().uid, original.uid);
        QCOMPARE(read(reopened.profiles().first().filePath), cached);
        // The canceled request must also release the refresh lock for the new URL.
        store.updateProfile(original.uid);
        QTRY_VERIFY(server.hasPendingConnections());
        QTcpSocket *replacement = server.nextPendingConnection();
        QTRY_VERIFY(replacement->bytesAvailable() > 0);
        QVERIFY(replacement->readAll().contains("/replacement?token=new"));
        const QByteArray newCache = "proxies: []\nmode: direct\n";
        respond(replacement, newCache, "download=10; total=2000");
        QTRY_COMPARE(updated.size(), 1);
        QCOMPARE(read(original.filePath), newCache);
        QCOMPARE(store.profiles().first().subscription.total, quint64(2000));
        QCOMPARE(store.profiles().first().url, replacementUrl);
    }
    void reloadCancelsInFlightImport() {
        core::ProfileStore store;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        store.importFromUrl(QString("http://127.0.0.1:%1/config").arg(server.serverPort()));
        QTRY_VERIFY(server.hasPendingConnections());
        QTcpSocket *socket = server.nextPendingConnection();
        QTRY_VERIFY(socket->bytesAvailable() > 0);
        socket->readAll();
        store.load();
        const QByteArray body = "proxies: []\n";
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                      "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
        QTRY_COMPARE(socket->state(), QAbstractSocket::UnconnectedState);
        QCoreApplication::processEvents();
        QVERIFY(store.profiles().isEmpty());
    }
    void explicitStringTagsSurviveScripts() {
        core::ConfigEnhancer enhancer;
        enhancer.addScript("identity");
        const auto result = enhancer.apply("proxies: []\npassword: !!str 123\nquoted: 'true'\nhex: '0xFF'\n");
        QVERIFY(result.error.isEmpty());
        const YAML::Node config = YAML::Load(result.yaml.toStdString());
        QCOMPARE(config["password"].Tag(), std::string("!"));
        QCOMPARE(config["quoted"].Tag(), std::string("!"));
        QCOMPARE(config["hex"].Tag(), std::string("!"));
    }
    void asyncProfileAndEnhancementWritesValidateAndDrainOnShutdown() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        const QString input = directory_->filePath("async-profile.yaml");
        write(input, "proxies: []\nmode: rule\n");
        store.importFromFileAsync(input);
        QVERIFY(store.isFileBusy());
        QVERIFY(store.profiles().isEmpty());
        QTRY_COMPARE(store.profiles().size(), 1);
        QTRY_VERIFY(!store.isFileBusy());
        const core::Profile profile = store.profiles().first();
        QSignalSpy saved(&store, &core::ProfileStore::profileContentSaved);
        store.saveProfileContentAsync(profile.uid, "[]");
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(!saved.last()[1].toBool());
        QCOMPARE(read(profile.filePath), QByteArray("proxies: []\nmode: rule\n"));
        store.saveProfileContentAsync(profile.uid, "proxies: []\nmode: direct\n");
        store.beginShutdown();
        store.createLocalProfileAsync("rejected", "proxies: []\n");
        QTRY_COMPARE(saved.size(), 2);
        QVERIFY(saved.last()[1].toBool());
        QTRY_VERIFY(!store.isFileBusy());
        QCOMPARE(read(profile.filePath), QByteArray("proxies: []\nmode: direct\n"));
        QCOMPARE(store.profiles().size(), 1);
        const QString fragment = directory_->filePath("async-merge.yaml");
        write(fragment, "mode: direct\n");
        enhancer.importItemAsync(fragment, core::ChainKind::Merge);
        QVERIFY(enhancer.isFileBusy());
        QTRY_COMPARE(enhancer.chain().size(), 1);
        QSignalSpy itemSaved(&enhancer, &core::ConfigEnhancer::itemContentSaved);
        const auto item = enhancer.chain().first();
        enhancer.saveItemContentAsync(item.uid, "[]");
        QTRY_COMPARE(itemSaved.size(), 1);
        QVERIFY(!itemSaved.last()[1].toBool());
        QCOMPARE(read(item.filePath), QByteArray("mode: direct\n"));
        enhancer.saveItemContentAsync(item.uid, "mode: global\n");
        enhancer.beginShutdown();
        QTRY_COMPARE(itemSaved.size(), 2);
        QVERIFY(itemSaved.last()[1].toBool());
        QTRY_VERIFY(!enhancer.isFileBusy());
        QCOMPARE(read(item.filePath), QByteArray("mode: global\n"));
    }
    void maintenanceCancelsFileWritesBeforeRestoreCanProceed() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        const QString input = directory_->filePath("pending.yaml");
        write(input, "proxies: []\n");
        store.importFromFileAsync(input);
        enhancer.importItemAsync(input, core::ChainKind::Merge);
        store.setMaintenanceMode(true);
        enhancer.setMaintenanceMode(true);
        QTRY_VERIFY(!store.isFileBusy());
        QTRY_VERIFY(!enhancer.isFileBusy());
        QVERIFY(store.profiles().isEmpty());
        QVERIFY(enhancer.chain().isEmpty());
        store.load();
        enhancer.load();
        QVERIFY(store.profiles().isEmpty());
        QVERIFY(enhancer.chain().isEmpty());
    }
    void runtimeGenerationKeepsEventLoopResponsiveAndDropsStaleResults() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        store.setEnhancer(&enhancer);
        QVERIFY(store.createLocalProfile("first", "proxies: []\nmode: rule\n"));
        QVERIFY(store.createLocalProfile("second", "proxies: []\nmode: direct\n"));
        const QString second = store.profiles().last().uid;
        enhancer.addScript("slow");
        const QString script = enhancer.chain().first().uid;
        QVERIFY(enhancer.saveItemContent(script,
            "function main(c) { let t=Date.now(); while(Date.now()-t<1500) {} c.mode='global'; return c; }"));
        QSignalSpy ready(&store, &core::ProfileStore::runtimeConfigReady);
        QElapsedTimer elapsed;
        elapsed.start();
        store.requestRuntimeConfig();
        QVERIFY2(elapsed.elapsed() < 200, "Runtime generation blocked the caller");
        QVERIFY(store.isRuntimeBusy());
        bool tick = false;
        QTimer::singleShot(30, this, [&] { tick = true; });
        QTRY_VERIFY_WITH_TIMEOUT(tick, 500);
        QVERIFY(ready.isEmpty());
        enhancer.setEnabled(script, false);
        store.selectProfile(second);
        store.requestRuntimeConfig();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 2000);
        QTRY_VERIFY(!store.isRuntimeBusy());
        const YAML::Node runtime = YAML::Load(read(ready.first().first().toString()).toStdString());
        QCOMPARE(runtime["mode"].as<std::string>(), std::string("direct"));
        enhancer.setEnabled(script, true);
        store.requestRuntimeConfig();
        QTest::qWait(30);
        store.cancelRuntimeGeneration();
        QTRY_VERIFY_WITH_TIMEOUT(!store.isRuntimeBusy(), 1000);
        QCOMPARE(ready.size(), 1);
    }
    void maintenanceBlocksMutationsAndCancelsRuntimeWork() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        store.setEnhancer(&enhancer);
        QVERIFY(store.createLocalProfile("protected", "proxies: []\n"));
        enhancer.addScript("slow");
        const auto item = enhancer.chain().first();
        QVERIFY(enhancer.saveItemContent(item.uid, "function main(c) { while(true) {} return c; }"));
        const QString uid = store.currentUid();
        QSignalSpy ready(&store, &core::ProfileStore::runtimeConfigReady);
        store.requestRuntimeConfig();
        QTest::qWait(30);
        store.setMaintenanceMode(true);
        enhancer.setMaintenanceMode(true);
        QVERIFY(!store.saveProfileContent(uid, "proxies: []\nmode: direct\n"));
        QVERIFY(!store.createLocalProfile("blocked", "proxies: []\n"));
        QVERIFY(!enhancer.saveItemContent(item.uid, "function main(c) { return c; }"));
        enhancer.removeItem(item.uid);
        store.removeProfile(uid);
        QCOMPARE(store.profiles().size(), 1);
        QCOMPARE(enhancer.chain().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!store.isRuntimeBusy(), 1000);
        QVERIFY(ready.isEmpty());
        store.load();
        enhancer.load();
        QCOMPARE(store.currentUid(), uid);
        store.setMaintenanceMode(false);
        enhancer.setMaintenanceMode(false);
        QVERIFY(store.saveProfileContent(uid, "proxies: []\nmode: direct\n"));
        QVERIFY(enhancer.saveItemContent(item.uid, "function main(c) { return c; }"));
    }
    void restartWaitsForOldExitWithoutBlockingGui() {
#ifdef Q_OS_WIN
        QSKIP("Uses a POSIX fake core process");
#else
        const QString binary = directory_->filePath("stubborn-core");
        write(binary, "#!/usr/bin/python3\nimport os,sys,signal,time\n"
            "if '-t' in sys.argv: sys.exit(0)\n"
            "marker=os.path.join(sys.argv[sys.argv.index('-d')+1], 'started-once')\n"
            "if not os.path.exists(marker):\n signal.signal(signal.SIGTERM,signal.SIG_IGN)\n open(marker,'w').close()\n"
            "print('started-child',flush=True)\ntime.sleep(30)\n");
        QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString config = directory_->filePath("config.yaml");
        write(config, "external-controller: 127.0.0.1:" + QByteArray::number(server.serverPort()) + "\n");
        core::CoreProcess process;
        process.setBinaryPath(binary);
        QSignalSpy lines(&process, &core::CoreProcess::logLine);
        QSignalSpy stopped(&process, &core::CoreProcess::stopped);
        process.start(config, directory_->path());
        QTRY_COMPARE(lines.size(), 1);
        QElapsedTimer elapsed;
        elapsed.start();
        process.restart();
        QVERIFY2(elapsed.elapsed() < 200, "Restart blocked while stopping the child");
        QTRY_COMPARE(process.state(), core::CoreState::Stopping);
        QVERIFY(process.isRestartPending());
        bool tick = false;
        QTimer::singleShot(30, this, [&] { tick = true; });
        QTRY_VERIFY_WITH_TIMEOUT(tick, 500);
        QCOMPARE(lines.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(lines.size(), 2, 4500);
        process.stop();
        QVERIFY(!process.isRestartPending());
        QTRY_COMPARE(process.state(), core::CoreState::Stopped);
        QCOMPARE(stopped.size(), 1);
        process.stop();
        QCOMPARE(stopped.size(), 2);
#endif
    }
    void privilegedServiceLifecycleUsesLeaseAndWaitsForStopAck() {
#ifdef Q_OS_WIN
        QSKIP("Uses a POSIX fake validator");
#else
        QTcpServer controller;
        QVERIFY(controller.listen(QHostAddress::LocalHost));
        connect(&controller, &QTcpServer::newConnection, &controller, [&] {
            auto *socket = controller.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                const QByteArray body = "{\"version\":\"service-test\"}";
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        const QString socketPath = directory_->filePath("mock-service.sock");
        QLocalServer helper;
        QVERIFY(helper.listen(socketPath));
        QStringList commands;
        QJsonObject receivedConfig;
        QLocalSocket *peer = nullptr;
        QJsonObject pendingStop;
        bool stallLogs = false;
        auto respond = [](QLocalSocket *socket, const QJsonObject &response) {
            const QByteArray json = QJsonDocument(response).toJson(QJsonDocument::Compact);
            QByteArray frame(4, Qt::Uninitialized);
            qToBigEndian<quint32>(json.size(), frame.data());
            socket->write(frame + json);
        };
        connect(&helper, &QLocalServer::newConnection, &helper, [&] {
            peer = helper.nextPendingConnection();
            auto input = std::make_shared<QByteArray>();
            connect(peer, &QLocalSocket::readyRead, peer, [&, input] {
                input->append(peer->readAll());
                while (input->size() >= 4) {
                    const auto length = qFromBigEndian<quint32>(input->constData());
                    if (input->size() < length + 4) return;
                    const auto request = QJsonDocument::fromJson(input->mid(4, length)).object();
                    input->remove(0, length + 4);
                    QCOMPARE(request.value("protocol").toInt(), 1);
                    const QString command = request.value("command").toString();
                    commands.append(command);
                    QJsonObject response{{"protocol", 1}, {"id", request.value("id")}, {"ok", true}};
                    if (command == "start") {
                        receivedConfig = request.value("config").toObject();
                        response.insert("endpoint", QJsonObject{{"host", "127.0.0.1"},
                            {"port", controller.serverPort()}, {"secret", "0123456789abcdef0123456789abcdef"}});
                    } else if (command == "stop") { pendingStop = response; continue; }
                    else if (command == "logs") {
                        if (stallLogs) continue;
                        response.insert("logs", "service-started\n");
                    } else if (command == "status") response.insert("state", "running");
                    respond(peer, response);
                }
            });
        });
        const QString binary = directory_->filePath("validator");
        write(binary, "#!/bin/sh\nif [ \"$1\" = '-t' ]; then exit 0; fi\nexit 99\n");
        QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString config = directory_->filePath("service-config.yaml");
        write(config, "proxies: []\nsecret: '0123456789abcdef0123456789abcdef'\nexternal-controller: 127.0.0.1:" +
            QByteArray::number(controller.serverPort()) + "\ncustom-string: '123'\n");
        // The stalled-logs case below expires this client's request on purpose;
        // every other request here is answered by hand, so none of them expire.
        // Local to this case: it replaces searching the client for its QTimer,
        // and nothing here then depends on wall-clock time.
        class ManualDeadline : public platform::RequestDeadline {
        public:
            void setExpiredHandler(std::function<void()> handler) override {
                expired_ = std::move(handler);
            }
            void start(std::chrono::milliseconds) override { pending_ = true; }
            void stop() override { pending_ = false; }
            bool expire() {
                if (!pending_ || !expired_) return false;
                pending_ = false;
                expired_();
                return true;
            }

        private:
            std::function<void()> expired_;
            bool pending_ = false;
        };
        ManualDeadline deadline;
        platform::PrivilegedServiceClient client(nullptr, socketPath, &deadline);
        core::CoreProcess process(nullptr, &client);
        process.setBinaryPath(binary);
        QVERIFY(process.setUseService(true));
        QVERIFY(process.isServiceMode());
        QVERIFY(!process.usesPrivilegedService());
        QSignalSpy ready(&process, &core::CoreProcess::ready);
        QSignalSpy stopped(&process, &core::CoreProcess::stopped);
        QSignalSpy failures(&process, &core::CoreProcess::failed);
        process.start(config, directory_->path());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        QCOMPARE(ready.size(), 1);
        QVERIFY(process.usesPrivilegedService());
        QVERIFY(!process.setUseService(false));
        QCOMPARE(receivedConfig.value("custom-string").toString(), QString("123"));
        // The last core output must survive the status-to-failure transition.
        emit client.logsReceived("fatal: YAML unknown escape in configuration\n");
        emit client.statusReceived(QJsonObject{{"state", "stopped"}});
        QTRY_VERIFY(!pendingStop.isEmpty());
        respond(peer, pendingStop);
        pendingStop = {};
        QTRY_COMPARE(process.state(), core::CoreState::Failed);
        QCOMPARE(failures.size(), 1);
        QVERIFY(failures.first().first().toString().contains("YAML unknown escape"));
        failures.clear();
        process.start(config, directory_->path());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        // A polling timeout must retain its cause, release the lease locally,
        // and allow a fresh start after the helper becomes responsive again.
        stallLogs = true;
        const auto previousCommands = commands.size();
        client.requestLogs();
        QTRY_VERIFY(commands.size() > previousCommands);
        QTRY_COMPARE(commands.last(), QString("logs"));
        QVERIFY(client.isBusy());
        QVERIFY(deadline.expire());
        QTRY_COMPARE(process.state(), core::CoreState::Failed);
        QCOMPARE(failures.size(), 1);
        QVERIFY(failures.first().first().toString().contains("timed out: logs"));
        QVERIFY(!process.usesPrivilegedService());
        QVERIFY(!client.isConnected());
        stallLogs = false;
        failures.clear();
        process.start(config, directory_->path());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        QVERIFY(process.usesPrivilegedService());
        QVERIFY(client.connectionError().isEmpty());
        QVERIFY(failures.isEmpty());
        process.stop();
        QTRY_VERIFY(!pendingStop.isEmpty());
        QCOMPARE(process.state(), core::CoreState::Stopping);
        QVERIFY(stopped.isEmpty());
        respond(peer, pendingStop);
        pendingStop = {};
        QTRY_COMPARE(process.state(), core::CoreState::Stopped);
        QCOMPARE(stopped.size(), 1);
        QVERIFY(!process.usesPrivilegedService());
        QVERIFY(failures.isEmpty());
        process.start(config, directory_->path());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        QSignalSpy terminal(&process, &core::CoreProcess::stopFinished);
        process.stop();
        QTRY_VERIFY(!pendingStop.isEmpty());
        peer->abort();
        QTRY_COMPARE(process.state(), core::CoreState::Failed);
        QTRY_COMPARE(terminal.size(), 1);
        QVERIFY(!terminal.first()[0].toBool());
        QVERIFY(!terminal.first()[1].toString().isEmpty());
        QVERIFY(!process.usesPrivilegedService());
        QVERIFY(!failures.isEmpty());
        QVERIFY(process.setUseService(false));
#endif
    }
    void validationIsResponsiveAndCancelable() {
#ifdef Q_OS_WIN
        QSKIP("Uses a POSIX fake core process");
#else
        const QString binary = directory_->filePath("slow-core");
        write(binary, "#!/bin/sh\nif [ \"$1\" = '-t' ]; then exec sleep 30; fi\nexit 0\n");
        QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        core::CoreProcess process;
        process.setBinaryPath(binary);
        QElapsedTimer elapsed;
        elapsed.start();
        process.start(directory_->filePath("unused.yaml"), directory_->path());
        QVERIFY2(elapsed.elapsed() < 500, "start() blocked while validating");
        bool tick = false;
        QTimer::singleShot(10, this, [&] { tick = true; });
        QTRY_VERIFY_WITH_TIMEOUT(tick, 500);
        QCOMPARE(process.state(), core::CoreState::Starting);
        QSignalSpy failures(&process, &core::CoreProcess::failed);
        process.stop();
        QTRY_COMPARE(process.state(), core::CoreState::Stopped);
        QTest::qWait(100);
        QCOMPARE(process.state(), core::CoreState::Stopped);
        QVERIFY(failures.isEmpty());
        // A new request cancels the older validator rather than waiting for it.
        process.start(directory_->filePath("unused.yaml"), directory_->path());
        const QString invalid = directory_->filePath("invalid-core");
        write(invalid, "#!/bin/sh\necho invalid-new-request\nexit 1\n");
        QVERIFY(QFile::setPermissions(invalid, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        process.setBinaryPath(invalid);
        process.start(directory_->filePath("unused.yaml"), directory_->path());
        QTRY_COMPARE_WITH_TIMEOUT(process.state(), core::CoreState::Failed, 1000);
        QCOMPARE(failures.size(), 1);
        QVERIFY(failures.first().first().toString().contains("invalid-new-request"));
#endif
    }
    void hangingProbeCanStopAndRestart() {
#ifdef Q_OS_WIN
        QSKIP("Uses a POSIX fake core process");
#else
        const QString binary = directory_->filePath("fake-core");
        write(binary, "#!/bin/sh\nif [ \"$1\" = '-t' ]; then exit 0; fi\nexec sleep 30\n");
        QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QTcpServer hanging;
        QVERIFY(hanging.listen(QHostAddress::LocalHost));
        QSignalSpy probes(&hanging, &QTcpServer::newConnection);
        const QString config = directory_->filePath("config.yaml");
        write(config, "external-controller: 127.0.0.1:" + QByteArray::number(hanging.serverPort()) + "\n");
        core::CoreProcess process;
        process.setBinaryPath(binary);
        QSignalSpy ready(&process, &core::CoreProcess::ready);
        process.start(config, directory_->path());
        QTRY_VERIFY(hanging.hasPendingConnections());
        QTRY_VERIFY_WITH_TIMEOUT(probes.size() >= 2, 4000);
        process.stop();
        QTRY_COMPARE(process.state(), core::CoreState::Stopped);
        QTcpServer healthy;
        QVERIFY(healthy.listen(QHostAddress::LocalHost));
        connect(&healthy, &QTcpServer::newConnection, &healthy, [&] {
            QTcpSocket *socket = healthy.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                const QByteArray body = "{\"version\":\"test\"}";
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        write(config, "external-controller: 127.0.0.1:" + QByteArray::number(healthy.serverPort()) + "\n");
        process.start(config, directory_->path());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        QCOMPARE(ready.size(), 1);
        // A bad edit must not stop the healthy child process.
        write(binary, "#!/bin/sh\nif [ \"$1\" = '-t' ]; then echo invalid-config; exit 1; fi\nexec sleep 30\n");
        QSignalSpy failures(&process, &core::CoreProcess::failed);
        process.restart();
        QCOMPARE(process.state(), core::CoreState::Running);
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(process.state(), core::CoreState::Running);
        QVERIFY(failures.first().first().toString().contains("invalid-config"));
        process.stop();
#endif
    }
private:
    std::unique_ptr<QTemporaryDir> directory_;
};

QTEST_GUILESS_MAIN(RuntimeTest)
#include "runtime_test.moc"
