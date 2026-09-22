// ProfileStore::requestEffectiveConfigPreview -- the answer the preset editor
// asks for before it commits anything.
//
// Three properties, and they are the whole feature: it writes nothing, it
// blocks nothing, and a superseded answer is never delivered. Contract:
// config-r1, .refactor/P4_CONFIG_CONTRACT.md.
#include <QtTest>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTimer>
#include <memory>
#include <yaml-cpp/yaml.h>

#include "core/config/config_composer.h"
#include "core/config/enhance/config_enhancer.h"
#include "core/profiles/profile_store.h"
#include "support/fixture_files.h"
#include "support/scoped_environment.h"

using core::config::ComposeResult;

namespace {

QJsonObject op(const QString &kind, const QString &path,
               const QJsonValue &value = QJsonValue(QJsonValue::Undefined)) {
    QJsonObject encoded{{"op", kind}, {"path", path}};
    if (!value.isUndefined()) encoded.insert("value", value);
    return encoded;
}

QJsonObject preset(const QString &id, const QJsonArray &operations) {
    return QJsonObject{{"id", id}, {"name", id}, {"enabled", true}, {"operations", operations}};
}

QJsonObject document(const QJsonArray &global, const QJsonObject &profiles = {}) {
    return QJsonObject{{"version", 1}, {"global", global}, {"profiles", profiles}};
}

// Every path under `root`, with its size and modification time, so "nothing
// was written" can be asserted rather than asserted-about-one-file.
QStringList treeFingerprint(const QString &root) {
    QStringList entries;
    QDirIterator walk(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                      QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        const QFileInfo info(walk.next());
        entries.append(QStringLiteral("%1|%2|%3")
                           .arg(info.absoluteFilePath())
                           .arg(info.isDir() ? -1 : info.size())
                           .arg(info.lastModified().toMSecsSinceEpoch()));
    }
    entries.sort();
    return entries;
}

QString sourceOf(const ComposeResult &result, const QString &path) {
    QString source;
    for (const core::config::Provenance &entry : result.provenance)
        if (entry.path == path) source = entry.source;
    return source;
}

}  // namespace

class EffectiveConfigPreviewTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { core::config::registerMetaTypes(); }
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("prev"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
    }
    void cleanup() { environment_.reset(); }

    // -------------------------------------------------------- the happy path

    void thePreviewIsTheSameCompositionTheRuntimeWouldGet() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\nrules:\n  - M\n"));
        const QString uid = store.currentUid();
        QVERIFY(store.setPresetDocument(document(
            QJsonArray{preset("g", QJsonArray{op("merge", "/dns", QJsonObject{{"enable", true}})})},
            QJsonObject{{uid, QJsonArray{preset("p", QJsonArray{op("replace", "/mode",
                                                                   "global")})}}})));
        QVERIFY(store.setRuntimeOverrides(QJsonObject{{"ipv6", true}}));
        // Generate first: the controller secret is minted and saved by the
        // generation path and by nothing else, so a preview taken before the
        // first launch legitimately differs from the runtime file by that one
        // field. Comparing the two only means something once it exists.
        const QString path = store.generateRuntimeConfig();
        QVERIFY(!path.isEmpty());

        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        const auto preview = ready.first().first().value<ComposeResult>();
        QVERIFY2(preview.ok, qPrintable(preview.yaml));

        const YAML::Node previewed = YAML::Load(preview.yaml.toStdString());
        QCOMPARE(previewed["mode"].as<std::string>(), std::string("global"));
        QVERIFY(previewed["dns"]["enable"].as<bool>());
        QVERIFY(previewed["ipv6"].as<bool>());

        // Byte for byte the file the core would be launched with.
        QCOMPARE(testsupport::readFile(path), preview.yaml.toUtf8());
    }

    void thePreviewCarriesProvenanceAndTheEnhancementLog() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        store.setEnhancer(&enhancer);
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        enhancer.addScript("talkative");
        QVERIFY(enhancer.saveItemContent(enhancer.chain().first().uid,
                                         "function main(c) { console.log('hello'); return c; }"));
        QVERIFY(store.setPresetDocument(
            document(QJsonArray{preset("g", QJsonArray{op("replace", "/mode", "global")})})));

        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        const auto preview = ready.first().first().value<ComposeResult>();
        QVERIFY(preview.ok);
        QVERIFY(preview.logs.join('\n').contains("hello"));
        QCOMPARE(sourceOf(preview, "/mode"), QStringLiteral("global:g"));
        QCOMPARE(sourceOf(preview, "/external-controller"), QStringLiteral("controller"));
        QCOMPARE(sourceOf(preview, "/"), QStringLiteral("legacy-chain"));
    }

    void anExplicitUidPreviewsThatProfileNotTheSelectedOne() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("selected", "proxies: []\nmode: rule\n"));
        QVERIFY(store.createLocalProfile("other", "proxies: []\nmode: direct\n"));
        const QString other = store.profiles().last().uid;
        QVERIFY(store.currentUid() != other);

        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview(other);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        const auto preview = ready.first().first().value<ComposeResult>();
        QVERIFY(preview.ok);
        QCOMPARE(YAML::Load(preview.yaml.toStdString())["mode"].as<std::string>(),
                 std::string("direct"));
    }

    // --------------------------------------------------------- no side effects

    // The claim the contract makes in one sentence, checked against the whole
    // data directory: a preview writes nothing, seeds nothing, and does not
    // leave a runtime.yaml or a .runtime-* snapshot behind.
    void apreviewWritesNothingAnywhereInTheDataDirectory() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        store.setEnhancer(&enhancer);
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        QVERIFY(store.setPresetDocument(
            document(QJsonArray{preset("g", QJsonArray{op("replace", "/mode", "global")})})));
        const QString seedDir = environment_->filePath("seed");
        QVERIFY(QDir().mkpath(seedDir));
        testsupport::writeFile(seedDir + "/geoip.dat", "seed-geoip");
        store.setSeedDir(seedDir);

        // A whole second of clearance, so a same-millisecond rewrite of an
        // identical file cannot hide inside the timestamp resolution.
        QTest::qWait(1100);
        const QStringList before = treeFingerprint(store.dataDir());
        QVERIFY(!before.isEmpty());

        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QVERIFY(ready.first().first().value<ComposeResult>().ok);

        QCOMPARE(treeFingerprint(store.dataDir()), before);
        QVERIFY(!QFile::exists(store.dataDir() + "/runtime.yaml"));
        QVERIFY(!QFile::exists(store.dataDir() + "/geoip.dat"));  // no seeding
        QVERIFY(!QFile::exists(store.dataDir() + "/ui"));         // no dashboard directory
    }

    // A preview must not be able to cancel, start or disturb a generation.
    void apreviewDoesNotDisturbRuntimeGeneration() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        QSignalSpy ready(&store, &core::ProfileStore::runtimeConfigReady);
        QSignalSpy previews(&store, &core::ProfileStore::effectiveConfigPreviewReady);

        store.requestRuntimeConfig();
        QVERIFY(store.isRuntimeBusy());
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(previews.size(), 1, 5000);
        QVERIFY(!store.isRuntimeBusy());
        QVERIFY(previews.first().first().value<ComposeResult>().ok);
    }

    // ------------------------------------------------------- staleness

    // Six requests, one answer. The editor types; only the last keystroke may
    // reach the view, and it must be the last keystroke's composition.
    void onlyTheNewestRequestIsAnswered() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        const QString uid = store.currentUid();

        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        for (int i = 0; i < 6; ++i) {
            QVERIFY(store.setPresetDocument(document(
                {}, QJsonObject{{uid, QJsonArray{preset(QStringLiteral("p%1").arg(i),
                                                        QJsonArray{op("replace", "/step", i)})}}})));
            store.requestEffectiveConfigPreview();
        }
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        // Give every superseded worker time to finish and be dropped.
        QTest::qWait(400);
        QCOMPARE(ready.size(), 1);
        const auto preview = ready.first().first().value<ComposeResult>();
        QVERIFY(preview.ok);
        QCOMPARE(YAML::Load(preview.yaml.toStdString())["step"].as<int>(), 5);
    }

    // The input is snapshotted when the request is made, so an edit that lands
    // while a preview is in flight cannot be half-applied to it. The older
    // request is superseded, and the answer that arrives is the newer one's.
    void aneditDuringAPreviewSupersedesItRatherThanLeakingIntoIt() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        store.setEnhancer(&enhancer);
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        enhancer.addScript("slow");
        const QString script = enhancer.chain().first().uid;
        QVERIFY(enhancer.saveItemContent(
            script,
            "function main(c) { let t=Date.now(); while(Date.now()-t<700) {} c.slow=1; return c; }"));

        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview();
        QTest::qWait(50);
        QVERIFY(ready.isEmpty());  // still running, on a worker

        QVERIFY(enhancer.saveItemContent(script, "function main(c) { c.slow=2; return c; }"));
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QTest::qWait(1200);  // the first worker finishes in here and is dropped
        QCOMPARE(ready.size(), 1);
        QCOMPARE(YAML::Load(ready.first().first().value<ComposeResult>().yaml.toStdString())["slow"]
                     .as<int>(),
                 2);
    }

    void thePreviewDoesNotBlockTheCaller() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        store.setEnhancer(&enhancer);
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        enhancer.addScript("slow");
        QVERIFY(enhancer.saveItemContent(
            enhancer.chain().first().uid,
            "function main(c) { let t=Date.now(); while(Date.now()-t<900) {} return c; }"));

        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        QElapsedTimer elapsed;
        elapsed.start();
        store.requestEffectiveConfigPreview();
        QVERIFY2(elapsed.elapsed() < 200, "requestEffectiveConfigPreview blocked the caller");
        bool tick = false;
        QTimer::singleShot(30, this, [&tick] { tick = true; });
        QTRY_VERIFY_WITH_TIMEOUT(tick, 500);
        QVERIFY(ready.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
    }

    // ------------------------------------------------------------- failures

    void afailedCompositionIsReportedAsAResultNotAsSilence() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("broken", "proxies: []\n"));
        QVERIFY(store.saveProfileContent(store.currentUid(), "proxies: []\ntun: false\n"));

        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        const auto preview = ready.first().first().value<ComposeResult>();
        QVERIFY(!preview.ok);
        QVERIFY(preview.yaml.isEmpty());
        bool explained = false;
        for (const core::config::Diagnostic &diagnostic : preview.diagnostics)
            explained = explained || (diagnostic.severity == QLatin1String("error") &&
                                      diagnostic.message.contains("TUN"));
        QVERIFY(explained);
    }

    void previewingWithNoProfileAnswersInsteadOfHanging() {
        core::ProfileStore store;
        store.load();
        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        const auto preview = ready.first().first().value<ComposeResult>();
        QVERIFY(!preview.ok);
        QVERIFY(!preview.diagnostics.isEmpty());
    }

    void previewingAnUnknownUidAnswersInsteadOfFallingBackToTheCurrentProfile() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\n"));
        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview("no-such-uid");
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QVERIFY(!ready.first().first().value<ComposeResult>().ok);
    }

    // A preview before the first launch has no secret to show, and must say so
    // rather than mint one -- minting it is a write, and previews do not write.
    void abeforeFirstLaunchPreviewSaysTheSecretIsNotAssignedYet() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\n"));
        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        const auto preview = ready.first().first().value<ComposeResult>();
        QVERIFY(preview.ok);
        bool told = false;
        for (const core::config::Diagnostic &diagnostic : preview.diagnostics)
            told = told || (diagnostic.severity == QLatin1String("info") &&
                            diagnostic.path == QLatin1String("/secret"));
        QVERIFY(told);
        // Generating once assigns it; the next preview is then quiet.
        QVERIFY(!store.generateRuntimeConfig().isEmpty());
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 5000);
        for (const core::config::Diagnostic &diagnostic :
             ready.last().first().value<ComposeResult>().diagnostics)
            QVERIFY(diagnostic.path != QLatin1String("/secret"));
    }

    void maintenanceModeAnswersWithAPausedDiagnosticRatherThanReadingFiles() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\n"));
        store.setMaintenanceMode(true);
        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QVERIFY(!ready.first().first().value<ComposeResult>().ok);
    }

    void shutdownStopsAnsweringAltogether() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\n"));
        store.beginShutdown();
        QSignalSpy ready(&store, &core::ProfileStore::effectiveConfigPreviewReady);
        store.requestEffectiveConfigPreview();
        QTest::qWait(300);
        QCOMPARE(ready.size(), 0);
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(EffectiveConfigPreviewTest)
#include "effective_config_preview_test.moc"
