// ProfileStore's half of the preset contract: validate-then-persist, the
// last-good recovery, and the fact that presets reach the generated runtime
// configuration through the same composer the preview uses.
//
// Contract: config-r1, .refactor/P4_CONFIG_CONTRACT.md.
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <memory>
#include <yaml-cpp/yaml.h>

#include "core/config/config_composer.h"
#include "core/profiles/profile_store.h"
#include "support/fixture_files.h"
#include "support/scoped_environment.h"

namespace {

QJsonObject op(const QString &kind, const QString &path,
               const QJsonValue &value = QJsonValue(QJsonValue::Undefined)) {
    QJsonObject encoded{{"op", kind}, {"path", path}};
    if (!value.isUndefined()) encoded.insert("value", value);
    return encoded;
}

QJsonObject preset(const QString &id, const QJsonArray &operations, bool enabled = true) {
    return QJsonObject{
        {"id", id}, {"name", id}, {"enabled", enabled}, {"operations", operations}};
}

QJsonObject document(const QJsonArray &global, const QJsonObject &profiles = {}) {
    return QJsonObject{{"version", 1}, {"global", global}, {"profiles", profiles}};
}

}  // namespace

class PresetStoreTest : public QObject {
    Q_OBJECT

    QString presetsPath(const core::ProfileStore &store) const {
        return store.dataDir() + "/presets.json";
    }
    QString lastGoodPath(const core::ProfileStore &store) const {
        return store.dataDir() + "/presets.last-good.json";
    }

private slots:
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("preset"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
    }
    void cleanup() { environment_.reset(); }

    // --------------------------------------------------------- the round trip

    void afreshStoreHasAnEmptyVersionOneDocumentAndNoFile() {
        core::ProfileStore store;
        store.load();
        QCOMPARE(store.presetDocument(), core::config::emptyPresetDocument());
        QVERIFY(store.lastPresetDiagnostics().isEmpty());
        // Reading presets must not be a reason to create a file.
        QVERIFY(!QFile::exists(presetsPath(store)));
        QVERIFY(!QFile::exists(lastGoodPath(store)));
    }

    void savingPersistsAtomicallyAndSurvivesAReopen() {
        core::ProfileStore store;
        store.load();
        QSignalSpy changed(&store, &core::ProfileStore::presetsChanged);

        const QJsonObject wanted =
            document(QJsonArray{preset("dns-on", QJsonArray{op("merge", "/dns",
                                                               QJsonObject{{"enable", true}})})},
                     QJsonObject{{"uid-a", QJsonArray{preset("mode", QJsonArray{op("replace",
                                                                                   "/mode",
                                                                                   "global")})}}});
        QVERIFY(store.setPresetDocument(wanted));
        QCOMPARE(changed.size(), 1);
        QVERIFY(QFile::exists(presetsPath(store)));
        QVERIFY(QFile::exists(lastGoodPath(store)));
        // Both copies hold the same bytes: the recovery copy is this document
        // in a second place, not the previous one.
        QCOMPARE(testsupport::readFile(presetsPath(store)),
                 testsupport::readFile(lastGoodPath(store)));

        core::ProfileStore reopened;
        reopened.load();
        QCOMPARE(reopened.presetDocument(), store.presetDocument());
        QCOMPARE(reopened.presetDocument().value("global").toArray().size(), 1);
        QCOMPARE(reopened.presetDocument()
                     .value("profiles")
                     .toObject()
                     .value("uid-a")
                     .toArray()
                     .size(),
                 1);
    }

    void whatIsStoredIsNormalisedSoSavingItAgainChangesNothing() {
        core::ProfileStore store;
        store.load();
        // Omits name and enabled; they have defaults.
        QVERIFY(store.setPresetDocument(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{QJsonObject{{"id", "bare"},
                                              {"operations", QJsonArray{op("replace", "/x", 1)}}}}}}));
        const QJsonObject stored = store.presetDocument();
        QCOMPARE(stored.value("global").toArray()[0].toObject().value("name").toString(),
                 QStringLiteral("bare"));
        QVERIFY(stored.value("global").toArray()[0].toObject().value("enabled").toBool());

        const QByteArray before = testsupport::readFile(presetsPath(store));
        QVERIFY(store.setPresetDocument(stored));
        QCOMPARE(store.presetDocument(), stored);
        QCOMPARE(testsupport::readFile(presetsPath(store)), before);
    }

    // ------------------------------------------------------------- rejection

    // The property that makes the editor safe to use: a rejected document
    // costs nothing. Not the in-memory state, not the file, not the recovery
    // copy, and no presetsChanged() that would make the UI redraw from it.
    void arejectedDocumentChangesNothingAtAll() {
        core::ProfileStore store;
        store.load();
        const QJsonObject good =
            document(QJsonArray{preset("keep", QJsonArray{op("replace", "/mode", "global")})});
        QVERIFY(store.setPresetDocument(good));
        const QJsonObject stored = store.presetDocument();
        const QByteArray onDisk = testsupport::readFile(presetsPath(store));
        const QByteArray recovery = testsupport::readFile(lastGoodPath(store));

        QSignalSpy changed(&store, &core::ProfileStore::presetsChanged);
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);

        const QJsonObject bad[] = {
            QJsonObject{{"global", QJsonArray{}}},                            // no version
            document(QJsonArray{preset("x", QJsonArray{op("nope", "/a", 1)})}),  // bad op
            document(QJsonArray{preset("x", QJsonArray{op("replace", "/secret", "mine")})}),
            document(QJsonArray{preset("x", {}), preset("x", {})}),           // duplicate id
            document(QJsonArray{preset("x", QJsonArray{op("append", "/proxies",
                                                          QJsonArray{"a"})})}),
        };
        for (const QJsonObject &rejected : bad) {
            QVERIFY2(!store.setPresetDocument(rejected),
                     qPrintable(QJsonDocument(rejected).toJson(QJsonDocument::Compact)));
            QCOMPARE(store.presetDocument(), stored);
            QCOMPARE(testsupport::readFile(presetsPath(store)), onDisk);
            QCOMPARE(testsupport::readFile(lastGoodPath(store)), recovery);
            QVERIFY(!store.lastPresetDiagnostics().isEmpty());
        }
        QCOMPARE(changed.size(), 0);
        QCOMPARE(errors.size(), std::size(bad));
    }

    // --------------------------------------------------------- the recovery

    void amalformedFileIsRecoveredFromTheLastGoodCopyWithADiagnostic() {
        core::ProfileStore store;
        store.load();
        const QJsonObject good =
            document(QJsonArray{preset("survivor", QJsonArray{op("replace", "/mode", "global")})});
        QVERIFY(store.setPresetDocument(good));
        const QJsonObject stored = store.presetDocument();

        // Something else corrupted the primary file between sessions.
        testsupport::writeFile(presetsPath(store), "{ not json at all");

        core::ProfileStore reopened;
        QSignalSpy errors(&reopened, &core::ProfileStore::errorOccurred);
        reopened.load();
        QCOMPARE(reopened.presetDocument(), stored);
        QCOMPARE(errors.size(), 1);
        QVERIFY(!reopened.lastPresetDiagnostics().isEmpty());
        // The evidence is left where it is: recovery is in memory.
        QCOMPARE(testsupport::readFile(presetsPath(reopened)), QByteArray("{ not json at all"));
    }

    void astructurallyInvalidFileIsRecoveredToo() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.setPresetDocument(
            document(QJsonArray{preset("survivor", QJsonArray{op("replace", "/mode", "global")})})));
        const QJsonObject stored = store.presetDocument();

        // Valid JSON, invalid document: a version this build does not write.
        testsupport::writeFile(presetsPath(store),
                               QJsonDocument(QJsonObject{{"version", 99}}).toJson());
        core::ProfileStore reopened;
        reopened.load();
        QCOMPARE(reopened.presetDocument(), stored);
    }

    void withNoUsableCopyAnywhereTheStoreStartsEmptyAndSaysSo() {
        core::ProfileStore store;
        testsupport::writeFile(store.dataDir() + "/presets.json", "{ broken");
        testsupport::writeFile(store.dataDir() + "/presets.last-good.json", "also broken");
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);
        store.load();
        QCOMPARE(store.presetDocument(), core::config::emptyPresetDocument());
        QCOMPARE(errors.size(), 1);
        QVERIFY(!store.lastPresetDiagnostics().isEmpty());
    }

    // --------------------------------------------- presets reach the runtime

    void presetsAreAppliedToTheGeneratedRuntimeConfiguration() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\nrules:\n  - M\n"));
        const QString uid = store.currentUid();

        QVERIFY(store.setPresetDocument(document(
            QJsonArray{preset("g", QJsonArray{op("merge", "/dns", QJsonObject{{"enable", true}}),
                                              op("prepend", "/rules", QJsonArray{"FIRST"})})},
            QJsonObject{{uid, QJsonArray{preset("p", QJsonArray{op("replace", "/mode",
                                                                   "global")})}}})));

        const QString path = store.generateRuntimeConfig();
        QVERIFY(!path.isEmpty());
        const YAML::Node config = YAML::Load(testsupport::readFile(path).toStdString());
        QVERIFY(config["dns"]["enable"].as<bool>());
        QCOMPARE(config["mode"].as<std::string>(), std::string("global"));
        QCOMPARE(config["rules"][0].as<std::string>(), std::string("FIRST"));
        QCOMPARE(config["rules"][1].as<std::string>(), std::string("M"));
        // The hijack default sees the preset's decision, not the profile's.
        QCOMPARE(config["tun"]["dns-hijack"].size(), size_t(2));
    }

    // Generation is the one place a preset could actually take the controller
    // away from the application, so the refusal is asserted against a real
    // generated file rather than only against the composer.
    void ahostilePresetCannotMoveTheControllerSecretOrDashboardOfARealGeneration() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("victim", "proxies: []\n"));
        // setPresetDocument refuses it, so the file is planted directly -- the
        // shape a document that reached the disk some other way would have.
        testsupport::writeFile(
            store.dataDir() + "/presets.json",
            QJsonDocument(document(QJsonArray{preset(
                              "evil", QJsonArray{op("replace", "/external-controller",
                                                    "0.0.0.0:9999"),
                                                 op("replace", "/secret", "attacker"),
                                                 op("replace", "/external-ui", "/tmp/evil")})}))
                .toJson());
        core::ProfileStore reopened;
        reopened.load();
        // The planted document is itself rejected, and recovery finds nothing.
        QCOMPARE(reopened.presetDocument(), core::config::emptyPresetDocument());

        const QString path = reopened.generateRuntimeConfig();
        QVERIFY(!path.isEmpty());
        const YAML::Node config = YAML::Load(testsupport::readFile(path).toStdString());
        QCOMPARE(config["external-controller"].as<std::string>(), std::string("127.0.0.1:29097"));
        QVERIFY(config["secret"].as<std::string>() != "attacker");
        QCOMPARE(config["external-ui"].as<std::string>(),
                 (reopened.dataDir() + "/ui").toStdString());
    }

    void apresetForAnotherProfileDoesNotAffectThisOne() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("mine", "proxies: []\nmode: rule\n"));
        QVERIFY(store.setPresetDocument(document(
            {}, QJsonObject{{"some-other-uid",
                             QJsonArray{preset("x", QJsonArray{op("replace", "/mode", "global")})}}})));
        const YAML::Node config =
            YAML::Load(testsupport::readFile(store.generateRuntimeConfig()).toStdString());
        QCOMPARE(config["mode"].as<std::string>(), std::string("rule"));
    }

    // A preset change invalidates any generation already in flight, the same
    // way an override or a chain change does. Without this the store can emit
    // a configuration composed from presets the user has already replaced.
    void changingThePresetsCancelsAGenerationAlreadyRunning() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        QSignalSpy ready(&store, &core::ProfileStore::runtimeConfigReady);
        store.requestRuntimeConfig();
        QVERIFY(store.isRuntimeBusy());
        QVERIFY(store.setPresetDocument(
            document(QJsonArray{preset("g", QJsonArray{op("replace", "/mode", "global")})})));
        QTRY_VERIFY_WITH_TIMEOUT(!store.isRuntimeBusy(), 5000);
        QCOMPARE(ready.size(), 0);

        store.requestRuntimeConfig();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        const YAML::Node config =
            YAML::Load(testsupport::readFile(ready.first().first().toString()).toStdString());
        QCOMPARE(config["mode"].as<std::string>(), std::string("global"));
    }

    void maintenanceModeRefusesPresetWritesLikeEveryOtherMutation() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.setPresetDocument(document(QJsonArray{preset("a", {})})));
        const QJsonObject stored = store.presetDocument();
        store.setMaintenanceMode(true);
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);
        QVERIFY(!store.setPresetDocument(document(QJsonArray{preset("b", {})})));
        QCOMPARE(store.presetDocument(), stored);
        QCOMPARE(errors.size(), 1);
        store.setMaintenanceMode(false);
        QVERIFY(store.setPresetDocument(document(QJsonArray{preset("b", {})})));
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(PresetStoreTest)
#include "preset_store_test.moc"
