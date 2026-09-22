// ProfileStore's half of the preset contract: validate-then-persist, the
// last-good recovery, and the fact that presets reach the generated runtime
// configuration through the same composer the preview uses.
//
// Contract: config-r1.
#include <QtTest>
#include <QDir>
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

// A merge value nested `levels` maps deep, and the source it lines up with. Past
// the composer's recursion guard this pair has no complete composition, which is
// the input the runtime-preservation case below needs.
QJsonValue nestedFragment(int levels) {
    QJsonObject value{{"marker", "deep"}};
    for (int i = 0; i < levels; ++i) value = QJsonObject{{"k", value}};
    return value;
}

QByteArray nestedYaml(int levels) {
    QString yaml = QStringLiteral("proxies: []\nnest:\n");
    QString indent = QStringLiteral("  ");
    for (int i = 0; i < levels; ++i) {
        yaml += indent + QStringLiteral("k:\n");
        indent += QStringLiteral("  ");
    }
    return (yaml + indent + QStringLiteral("marker: shallow\n")).toUtf8();
}

QStringList presetIdsOf(const QJsonObject &document) {
    QStringList ids;
    for (const QJsonValue &preset : document.value("global").toArray())
        ids.append(preset.toObject().value("id").toString());
    ids.sort();
    return ids;
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

    // ------------------------------------- recovery from something other than
    //                                                  a malformed primary file
    //
    // The four cases below are one defect: loadPresets() used to answer a failed
    // open() with the empty document, so anything short of "the bytes are
    // corrupt" -- a permission change, a directory left at the path, the file
    // going missing -- lost every preset the user had, said nothing about it, and
    // then let the next save overwrite the intact recovery copy as well.

    void amissingPrimaryFileIsRecoveredFromTheLastGoodCopy() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.setPresetDocument(
            document(QJsonArray{preset("keepme", QJsonArray{op("replace", "/mode", "global")})})));
        const QJsonObject stored = store.presetDocument();
        // Only the primary goes; the recovery copy is exactly what it is for.
        QVERIFY(QFile::remove(presetsPath(store)));

        core::ProfileStore reopened;
        QSignalSpy errors(&reopened, &core::ProfileStore::errorOccurred);
        reopened.load();
        QCOMPARE(reopened.presetDocument(), stored);
        QCOMPARE(errors.size(), 1);
        QVERIFY(!reopened.lastPresetDiagnostics().isEmpty());
        // Loading recovers in memory and writes nothing, so the primary is still
        // absent until something asks for a save.
        QVERIFY(!QFile::exists(presetsPath(reopened)));
    }

    // The auditor's reproducer: a present-but-unopenable primary. The file lives
    // in this test's own temporary data directory and its mode is put back before
    // anything is asserted.
    void apresetFileThatWillNotOpenIsRecoveredNotTreatedAsAFirstRun() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.setPresetDocument(
            document(QJsonArray{preset("keepme", QJsonArray{op("replace", "/mode", "global")})})));
        const QJsonObject stored = store.presetDocument();
        const QByteArray onDisk = testsupport::readFile(presetsPath(store));
        QVERIFY(QFile::setPermissions(presetsPath(store), {}));

        core::ProfileStore reopened;
        QSignalSpy errors(&reopened, &core::ProfileStore::errorOccurred);
        reopened.load();
        QVERIFY(QFile::setPermissions(presetsPath(reopened),
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner));

        QCOMPARE(reopened.presetDocument(), stored);
        QCOMPARE(errors.size(), 1);
        // The diagnostic names the file that failed and the reason, not just
        // "something went wrong": an unreadable file is a thing the user can fix.
        bool named = false;
        for (const core::config::Diagnostic &diagnostic : reopened.lastPresetDiagnostics())
            if (diagnostic.severity == QLatin1String("error") &&
                diagnostic.message.contains(QLatin1String("presets.json")))
                named = true;
        QVERIFY2(named, "no error diagnostic named the unreadable file");
        // And the bad file is left where it is, evidence intact.
        QCOMPARE(testsupport::readFile(presetsPath(reopened)), onDisk);
    }

    // The same class of failure without touching any permission: a directory
    // where the document should be. open() fails, and that is still not a first
    // run.
    void adirectoryLeftAtThePresetPathIsAnUnusableDocument() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.setPresetDocument(
            document(QJsonArray{preset("keepme", QJsonArray{op("replace", "/mode", "global")})})));
        const QJsonObject stored = store.presetDocument();
        QVERIFY(QFile::remove(presetsPath(store)));
        QVERIFY(QDir().mkpath(presetsPath(store)));

        core::ProfileStore reopened;
        QSignalSpy errors(&reopened, &core::ProfileStore::errorOccurred);
        reopened.load();
        QCOMPARE(reopened.presetDocument(), stored);
        QCOMPARE(errors.size(), 1);
        QVERIFY(QDir(presetsPath(reopened)).exists());  // not deleted, not written through
    }

    // The amplification, and the reason the silent loss mattered: whatever was
    // recovered has to survive the next save. setPresetDocument() writes BOTH
    // copies, so a load that emptied itself would take the one intact copy with
    // it on the user's next edit.
    void therecoveredDocumentIsWhatTheNextSaveBuildsOn() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.setPresetDocument(
            document(QJsonArray{preset("keepme", QJsonArray{op("replace", "/mode", "global")})})));
        QVERIFY(QFile::setPermissions(presetsPath(store), {}));

        core::ProfileStore reopened;
        reopened.load();
        QVERIFY(QFile::setPermissions(presetsPath(reopened),
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner));

        // The user adds one unrelated preset in the editor.
        QJsonObject next = reopened.presetDocument();
        QJsonArray global = next.value("global").toArray();
        global.append(preset("new", QJsonArray{op("replace", "/ipv6", true)}));
        next["global"] = global;
        QVERIFY(reopened.setPresetDocument(next));

        const QStringList expected{QStringLiteral("keepme"), QStringLiteral("new")};
        QCOMPARE(presetIdsOf(reopened.presetDocument()), expected);
        for (const QString &path : {presetsPath(reopened), lastGoodPath(reopened)}) {
            const QJsonObject written =
                QJsonDocument::fromJson(testsupport::readFile(path)).object();
            QVERIFY2(!written.isEmpty(), qPrintable(path));
            QCOMPARE(presetIdsOf(written), expected);
        }
        // The primary is readable again, so the next session needs no recovery.
        core::ProfileStore third;
        QSignalSpy errors(&third, &core::ProfileStore::errorOccurred);
        third.load();
        QCOMPARE(presetIdsOf(third.presetDocument()), expected);
        QCOMPARE(errors.size(), 0);
        QVERIFY(third.lastPresetDiagnostics().isEmpty());
    }

    // Nothing on disk is usable, but presets are already loaded. They are the
    // user's, they are still valid, and a load that cannot read a document has
    // no business throwing away the one it has.
    void presetsAlreadyLoadedAreKeptWhenNeitherCopyCanBeRead() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.setPresetDocument(
            document(QJsonArray{preset("keepme", QJsonArray{op("replace", "/mode", "global")})})));
        const QJsonObject stored = store.presetDocument();

        testsupport::writeFile(presetsPath(store), "{ half written");
        testsupport::writeFile(lastGoodPath(store), "also not json");
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);
        store.load();

        QCOMPARE(store.presetDocument(), stored);
        QCOMPARE(errors.size(), 1);
        QVERIFY(!store.lastPresetDiagnostics().isEmpty());
        // And they are still the presets the composition uses, not just a value
        // presetDocument() returns.
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        const YAML::Node config =
            YAML::Load(testsupport::readFile(store.generateRuntimeConfig()).toStdString());
        QCOMPARE(config["mode"].as<std::string>(), std::string("global"));
    }

    void amalformedPrimaryWithNoRecoveryCopyIsReportedNotAssumedEmpty() {
        core::ProfileStore store;
        testsupport::writeFile(presetsPath(store), "{ not json");
        QVERIFY(!QFile::exists(lastGoodPath(store)));
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);
        store.load();
        QCOMPARE(store.presetDocument(), core::config::emptyPresetDocument());
        QCOMPARE(errors.size(), 1);
        QVERIFY(!store.lastPresetDiagnostics().isEmpty());
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

    // A preset whose merge cannot be applied in full has no candidate
    // configuration at all, so generation must fail with an actionable reason and
    // leave the runtime the core is using exactly where it is. The alternative --
    // what this forbids -- is a truncated document being written over a working
    // one and reported as a success.
    void apresetTooDeepToComposeFailsGenerationAndKeepsThePreviousRuntime() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", nestedYaml(70)));
        const QString first = store.generateRuntimeConfig();
        QVERIFY2(!first.isEmpty(), "the baseline generation failed");
        const QString preview = store.dataDir() + "/runtime.yaml";
        const QByteArray before = testsupport::readFile(preview);
        QVERIFY(!before.isEmpty());

        QVERIFY(store.setPresetDocument(document(
            QJsonArray{preset("deep", QJsonArray{op("merge", "/nest", nestedFragment(70))})})));
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);
        QCOMPARE(store.generateRuntimeConfig(), QString());
        QCOMPARE(errors.size(), 1);
        QVERIFY2(errors.first().first().toString().contains(QLatin1String("deep"),
                                                            Qt::CaseInsensitive),
                 qPrintable(errors.first().first().toString()));
        QCOMPARE(testsupport::readFile(preview), before);
        QCOMPARE(testsupport::readFile(first), before);
    }

    // The legacy behaviour the pure composer now owns (F3): the port the user
    // chose is the port the engine is launched on. ProfileStore supplies only the
    // default, so this is the end-to-end proof that handing the resolution to
    // compose() did not quietly drop it.
    void amixedPortRuntimeOverrideStillReachesTheGeneratedRuntime() {
        core::ProfileStore store;
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        QCOMPARE(YAML::Load(testsupport::readFile(store.generateRuntimeConfig()).toStdString())
                     ["mixed-port"].as<int>(),
                 27890);

        QVERIFY(store.setRuntimeOverrides(QJsonObject{{"mixed-port", 41234}}));
        const YAML::Node config =
            YAML::Load(testsupport::readFile(store.generateRuntimeConfig()).toStdString());
        QCOMPARE(config["mixed-port"].as<int>(), 41234);
        // Still nobody else's to choose: the secret and the controller are the
        // application's in the same document.
        QCOMPARE(config["external-controller"].as<std::string>(), std::string("127.0.0.1:29097"));
    }

    // setRuntimeOverrides() refuses a port outside 1..65535, so a stored one got
    // there some other way -- a hand-edited file, a restored archive. The store
    // hands the composer the application default and the overrides object, and the
    // composer is what decides between them, so the impossible value is dropped
    // with a warning instead of being passed on as the default.
    void anImpossiblePortInTheOverridesFileCannotReachTheEngine() {
        core::ProfileStore store;
        testsupport::writeFile(
            store.dataDir() + "/runtime-overrides.json",
            QJsonDocument(QJsonObject{{"mixed-port", 70000}}).toJson(QJsonDocument::Compact));
        store.load();
        QVERIFY(store.createLocalProfile("target", "proxies: []\nmode: rule\n"));
        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);

        const QString path = store.generateRuntimeConfig();
        QVERIFY2(!path.isEmpty(), "an impossible port stopped the generation altogether");
        QCOMPARE(YAML::Load(testsupport::readFile(path).toStdString())["mixed-port"].as<int>(),
                 27890);
        QCOMPARE(errors.size(), 1);
        QVERIFY2(errors.first().first().toString().contains(QLatin1String("mixed-port")),
                 qPrintable(errors.first().first().toString()));
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
