// Runtime-configuration generation: override > script > profile precedence, the
// controller/secret values the application owns, TUN defaults and overrides, YAML
// fidelity through the JS enhancement chain, and the responsiveness of generation.
//
// Partition of the former `runtime` suite (tests/core/runtime_test.cpp). Cases are
// carried over verbatim; see tests/README.md for the full original-to-new map.
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTimer>
#include <memory>
#include <yaml-cpp/yaml.h>

#include "core/config/enhance/config_enhancer.h"
#include "core/config/yaml_util.h"
#include "core/profiles/profile_store.h"
#include "support/fixture_files.h"
#include "support/scoped_environment.h"

class ConfigGenerationTest : public QObject {
    Q_OBJECT
private slots:
    // One scoped environment per test function, as the original suite had one
    // QTemporaryDir per test function. ScopedEnvironment additionally *restores*
    // any CLASH_QT_DATA_DIR the developer had exported, where the original
    // cleanup() unset it and lost it for the rest of the process.
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("cfg"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
    }
    void cleanup() { environment_.reset(); }
    void runtimeAppliesEnhancementsAndProtectsController() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        store.setEnhancer(&enhancer);
        const QString input = environment_->filePath("sample.yaml");
        testsupport::writeFile(input, "proxies: []\nprofile: false\nsecret: malicious\nport: 1234\n");
        QSignalSpy selected(&store, &core::ProfileStore::currentProfileChanged);
        store.importFromFile(input);
        QCOMPARE(selected.size(), 1);
        enhancer.addScript("use profile name");
        testsupport::writeFile(enhancer.chain().first().filePath,
              "function main(c, name) { console.log(name); c.mode = 'global'; "
              "c.secret = 'wrong'; c['external-controller'] = '0.0.0.0:9999'; "
              "c['mixed-port'] = 1; c.profileName = name; return c; }");
        QVERIFY(store.setRuntimeOverrides({{"mixed-port", 28888}, {"mode", "direct"}}));
        QSignalSpy logs(&store, &core::ProfileStore::enhancementLog);
        const QString output = store.generateRuntimeConfig();
        QVERIFY(!output.isEmpty());
        const YAML::Node config = YAML::Load(testsupport::readFile(output).toStdString());
        QCOMPARE(config["profileName"].as<std::string>(), std::string("sample"));
        QCOMPARE(config["mode"].as<std::string>(), std::string("direct"));
        QCOMPARE(config["mixed-port"].as<int>(), 28888);
        QCOMPARE(config["external-controller"].as<std::string>(), std::string("127.0.0.1:29097"));
        QVERIFY(config["secret"].as<std::string>() != "wrong");
        QVERIFY(config["profile"]["store-selected"].as<bool>());
        QVERIFY(!config["port"]);
        QCOMPARE(logs.size(), 1);
        QVERIFY(logs.first().first().toStringList().join('\n').contains("sample"));
        // Straddles the config/profiles boundary. The four lines below are a
        // profiles-persistence assertion (F01): overrides and the current uid
        // survive a reopen. They are kept here, in the config suite, rather than
        // rebuilt on top of a duplicated fixture in profile-store -- splitting
        // them would mean re-creating this exact store, enhancer, script and
        // override set, which is a rewrite, not a partition. Recorded in
        // tests/README.md so the profiles package knows it owns this assertion.
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
        YAML::Node config = YAML::Load(testsupport::readFile(path).toStdString());
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
        config = YAML::Load(testsupport::readFile(path).toStdString());
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
        YAML::Node config = YAML::Load(testsupport::readFile(store.generateRuntimeConfig()).toStdString());
        QVERIFY(!config["tun"]["enable"].as<bool>());
        QCOMPARE(config["tun"]["stack"].as<std::string>(), std::string("gvisor"));
        QVERIFY(!config["tun"]["auto-route"].as<bool>());
        QVERIFY(!config["tun"]["auto-detect-interface"].as<bool>());
        QCOMPARE(config["tun"]["dns-hijack"].size(), size_t(0));
        QVERIFY(store.setRuntimeOverrides({{"tun", QJsonObject{{"enable", true}, {"stack", "system"},
            {"auto-route", false}, {"auto-detect-interface", false}, {"mtu", 1400},
            {"dns-hijack", QJsonArray{"tcp://any:5353"}}}}}));
        config = YAML::Load(testsupport::readFile(store.generateRuntimeConfig()).toStdString());
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
        const QByteArray good = testsupport::readFile(path);
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);
        QVERIFY(store.saveProfileContent(store.currentUid(), "proxies: []\ntun: false\n"));
        QVERIFY(store.generateRuntimeConfig().isEmpty());
        QCOMPARE(testsupport::readFile(path), good);
        QVERIFY(errors.last().first().toString().contains("TUN"));
        QVERIFY(store.saveProfileContent(store.currentUid(), "proxies: []\n"));
        QVERIFY(store.setRuntimeOverrides({{"tun", QJsonArray{}}}));
        QVERIFY(store.generateRuntimeConfig().isEmpty());
        QCOMPARE(testsupport::readFile(path), good);
    }
    void failedScriptPreservesConfigAndContinues() {
        core::ConfigEnhancer enhancer;
        enhancer.addScript("broken");
        testsupport::writeFile(enhancer.chain().first().filePath,
              "function main(c) { c.mode = 'global'; console.log('before'); throw Error('oops'); }");
        enhancer.addMerge("next");
        testsupport::writeFile(enhancer.chain().last().filePath, "IPV6: true\n");
        const auto result = enhancer.apply("proxies: []\nmode: rule\n", "name");
        QVERIFY(result.error.contains("oops"));
        QVERIFY(result.logs.join('\n').contains("before"));
        const YAML::Node config = YAML::Load(result.yaml.toStdString());
        QCOMPARE(config["mode"].as<std::string>(), std::string("rule"));
        QVERIFY(config["ipv6"].as<bool>());
    }
    void cyclicYamlIsRejectedWithoutRecursingForever() {
        const YAML::Node cyclic = YAML::Load("proxies: []\ncycle: &loop [*loop]\n");
        QVERIFY(core::yamlutil::dump(cyclic).empty());
        core::ConfigEnhancer enhancer;
        enhancer.addScript("identity");
        const auto result = enhancer.apply("proxies: []\ncycle: &loop [*loop]\n");
        QVERIFY(!result.error.isEmpty());
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
    // ---------------------------------------------------------- geo-data seeding
    //
    // The managed engine is launched with dataDir() as its home and looks there
    // for Country.mmdb, geoip.dat and geosite.dat. Generation copies them in
    // from a seed directory, which is what spares a first launch a ~29 MB
    // download. The store used to find that directory itself, by calling
    // core::vergeConfigPath() out of the component-private engine library; the
    // caller supplies it now, and these two cases pin both halves of the new
    // contract. They are worth their length because the failure is silent: the
    // configuration still generates, the engine still starts, and only rule
    // matching is wrong.
    void runtimeSeedsGeoDataFromTheDirectoryTheCallerSupplies() {
        core::ProfileStore store;
        QCOMPARE(store.seedDir(), QString());  // nothing is assumed by default
        QVERIFY(store.createLocalProfile("seeded", "proxies: []\nmode: rule\n"));

        const QString seedDir = environment_->filePath("verge");
        QVERIFY(QDir().mkpath(seedDir));
        testsupport::writeFile(seedDir + "/Country.mmdb", "seed-mmdb");
        testsupport::writeFile(seedDir + "/geoip.dat", "seed-geoip");
        testsupport::writeFile(seedDir + "/geosite.dat", "seed-geosite");
        // An engine that has already refreshed its own copy keeps it: seeding
        // fills a gap, it never overwrites.
        testsupport::writeFile(store.dataDir() + "/geosite.dat", "already-mine");

        store.setSeedDir(seedDir);
        QCOMPARE(store.seedDir(), seedDir);
        QVERIFY(!store.generateRuntimeConfig().isEmpty());

        QCOMPARE(testsupport::readFile(store.dataDir() + "/Country.mmdb"), QByteArray("seed-mmdb"));
        QCOMPARE(testsupport::readFile(store.dataDir() + "/geoip.dat"), QByteArray("seed-geoip"));
        QCOMPARE(testsupport::readFile(store.dataDir() + "/geosite.dat"), QByteArray("already-mine"));
        // The seed directory is a source, not a scratch space.
        QCOMPARE(testsupport::readFile(seedDir + "/Country.mmdb"), QByteArray("seed-mmdb"));
    }

    // The other half, and the point of the inversion: with no seed directory the
    // store seeds nothing. It must not reach for a location of its own choosing
    // - on a developer machine with Clash Verge Rev installed the old code
    // copied that user's real 29 MB of geo data into every test run - and it
    // must not probe the filesystem root for "/Country.mmdb" either.
    void runtimeSeedsNothingWhenNoSeedDirectoryWasSupplied() {
        core::ProfileStore store;
        QVERIFY(store.createLocalProfile("unseeded", "proxies: []\nmode: rule\n"));
        QVERIFY(store.seedDir().isEmpty());
        QVERIFY(!store.generateRuntimeConfig().isEmpty());
        for (const char *name : {"Country.mmdb", "geoip.dat", "geosite.dat"})
            QVERIFY2(!QFileInfo::exists(store.dataDir() + '/' + name), name);
    }

    // The asynchronous path shares prepareRuntime() with the synchronous one, so
    // it must seed identically. Asserted separately because the two entry points
    // are what an injected seed directory could most easily fall between.
    void requestedRuntimeGenerationSeedsGeoDataToo() {
        core::ProfileStore store;
        QVERIFY(store.createLocalProfile("seeded", "proxies: []\nmode: rule\n"));
        const QString seedDir = environment_->filePath("verge-async");
        QVERIFY(QDir().mkpath(seedDir));
        testsupport::writeFile(seedDir + "/geoip.dat", "seed-geoip");
        store.setSeedDir(seedDir);

        QSignalSpy ready(&store, &core::ProfileStore::runtimeConfigReady);
        store.requestRuntimeConfig();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QCOMPARE(testsupport::readFile(store.dataDir() + "/geoip.dat"), QByteArray("seed-geoip"));
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
        const YAML::Node runtime = YAML::Load(testsupport::readFile(ready.first().first().toString()).toStdString());
        QCOMPARE(runtime["mode"].as<std::string>(), std::string("direct"));
        enhancer.setEnabled(script, true);
        store.requestRuntimeConfig();
        QTest::qWait(30);
        store.cancelRuntimeGeneration();
        QTRY_VERIFY_WITH_TIMEOUT(!store.isRuntimeBusy(), 1000);
        QCOMPARE(ready.size(), 1);
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(ConfigGenerationTest)
#include "config_generation_test.moc"
