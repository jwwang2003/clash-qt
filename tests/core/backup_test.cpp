#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSettings>
#include <QTemporaryDir>
#include <memory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "core/backups/backup_store.h"
#include "core/config/config_composer.h"
#include "core/preferences/preferences.h"
#include "support/scoped_environment.h"

namespace {
bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A non-empty preset document, valid by the composer's rules, distinguishable
// by `mode` so a round trip can tell two of them apart.
QByteArray presets(const QString &mode) {
    const QJsonObject global{
        {"id", "global-mode"}, {"name", "Force a mode"}, {"enabled", true},
        {"operations", QJsonArray{QJsonObject{{"op", "merge"}, {"path", "/mode"}, {"value", mode}}}}};
    const QJsonObject perProfile{
        {"id", "one-log"}, {"name", "Loud logs"}, {"enabled", false},
        {"operations", QJsonArray{QJsonObject{{"op", "replace"}, {"path", "/log-level"},
                                              {"value", "debug"}}}}};
    return QJsonDocument(QJsonObject{{"version", 1},
                                     {"global", QJsonArray{global}},
                                     {"profiles", QJsonObject{{"one", QJsonArray{perProfile}}}}})
        .toJson(QJsonDocument::Compact);
}

QByteArray emptyPresets() {
    return QJsonDocument(core::config::emptyPresetDocument()).toJson(QJsonDocument::Compact);
}

// True when `bytes` is something ProfileStore could load: the point of
// validating the pair inside the archive is that neither file can come back as
// something the next launch has to recover from.
bool loadable(const QByteArray &bytes) {
    core::config::PresetDocument parsed;
    QVector<core::config::Diagnostic> diagnostics;
    const auto document = QJsonDocument::fromJson(bytes);
    return document.isObject() &&
           core::config::parsePresetDocument(document.object(), &parsed, &diagnostics);
}

// The archive's `files` array, keyed by path, so a test can assert on one entry
// without walking the array each time.
QMap<QString, QByteArray> archiveFiles(const QByteArray &archive) {
    QMap<QString, QByteArray> result;
    for (const auto &entry : QJsonDocument::fromJson(archive).object().value("files").toArray()) {
        const auto item = entry.toObject();
        result.insert(item.value("path").toString(),
                      QByteArray::fromBase64(item.value("data").toString().toLatin1()));
    }
    return result;
}

// Replaces one file entry's contents, keeping the checksum honest, so what the
// archive fails on is the preset document itself and not the hash check the
// tampering tests already cover.
QByteArray withFile(const QByteArray &archive, const QString &path, const QByteArray &contents) {
    auto document = QJsonDocument::fromJson(archive).object();
    QJsonArray entries;
    for (const auto &entry : document.value("files").toArray()) {
        auto item = entry.toObject();
        if (item.value("path").toString() == path) {
            item["data"] = QString::fromLatin1(contents.toBase64());
            item["sha256"] = QString::fromLatin1(
                QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
        }
        entries.append(item);
    }
    document["files"] = entries;
    return QJsonDocument(document).toJson();
}

// An archive as it was written before presets existed: the pair simply absent.
QByteArray withoutFiles(const QByteArray &archive, const QStringList &paths) {
    auto document = QJsonDocument::fromJson(archive).object();
    QJsonArray entries;
    for (const auto &entry : document.value("files").toArray())
        if (!paths.contains(entry.toObject().value("path").toString())) entries.append(entry);
    document["files"] = entries;
    return QJsonDocument(document).toJson();
}
bool seed(const QString &directory) {
    return QDir().mkpath(directory + "/profiles") && QDir().mkpath(directory + "/chain") &&
        write(directory + "/profiles.json", R"({"profiles":[{"uid":"one","name":"One","file":"one.yaml"}],"currentUid":"one","secret":"ephemeral-test-secret"})") &&
        write(directory + "/profiles/one.yaml", "proxies: []\nmode: rule\n") &&
        write(directory + "/chain.json", R"({"chain":[{"uid":"script","name":"Script","file":"script.js","kind":"script"}]})") &&
        write(directory + "/chain/script.js", "function main(config) { return config; }") &&
        write(directory + "/runtime-overrides.json", R"({"mixed-port":17890})");
}

class WebDavFixture : public QTcpServer {
public:
    QByteArray stored;
    QList<QByteArray> requests;
    QByteArray status = "200 OK";

    WebDavFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    const int headerEnd = request.indexOf("\r\n\r\n");
                    if (headerEnd < 0 || socket->property("handled").toBool()) return;
                    int contentLength = 0;
                    for (const auto &line : request.left(headerEnd).split('\n'))
                        if (line.toLower().startsWith("content-length:")) contentLength = line.mid(15).trimmed().toInt();
                    if (request.size() < headerEnd + 4 + contentLength) return;
                    socket->setProperty("handled", true);
                    requests.append(request.left(headerEnd));
                    const bool upload = request.startsWith("PUT ");
                    if (upload) stored = request.mid(headerEnd + 4, contentLength);
                    const auto body = upload ? QByteArray() : stored;
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) +
                                  "\r\nLocation: /redirect-target\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};
}

class BackupTest : public QObject {
    Q_OBJECT
    // This suite is where the settings-isolation defect was proved: its
    // "do-not-export" sentinel turned up in the developer's real
    // ~/Library/Preferences/com.clash-qt.clash-qt.plist. setDefaultFormat() and
    // setPath(), which used to stand here, cannot redirect
    // QSettings(organization, application) on macOS. The scoped environment
    // exports CLASH_QT_DATA_DIR, which core::preferences does honour, and
    // watches the real store for the rest of the run.
    std::unique_ptr<testsupport::ScopedEnvironment> preferences_;
private slots:
    void initTestCase() {
        preferences_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("backup"));
        QVERIFY2(preferences_->isValid(), qPrintable(preferences_->errorString()));
        QVERIFY(core::preferences::isIsolated());
        QVERIFY2(core::preferences::fileName().startsWith(
                     QFileInfo(preferences_->dataDir()).absoluteFilePath() + QLatin1Char('/')),
                 qPrintable(core::preferences::fileName()));
    }
    void cleanupTestCase() {
        // Read-only, and through CFPreferences rather than the plist's
        // modification time, so a write cfprefsd has not yet flushed still
        // shows up here.
        const QSettings native(QString::fromLatin1(core::preferences::kOrganization),
                               QString::fromLatin1(core::preferences::kApplication));
        QVERIFY2(native.value("backup/password").toString() != QLatin1String("do-not-export"),
                 "This suite's sentinel reached the real user preference store");
        QVERIFY2(preferences_->realPreferencesUnchanged(),
                 qPrintable(QStringLiteral("The real user preference store at %1 changed during this run")
                                .arg(preferences_->productionSettingsFilePath())));
        preferences_.reset();
    }
    void snapshotAndRestore() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        QVERIFY(write(directory.path() + "/Country.mmdb", "keep-cache"));
        QSettings settings = core::preferences::open();
        settings.setValue("startup/startCore", true);
        settings.setValue("window/geometry", QByteArray("binary\0geometry", 15));
        settings.setValue("backup/password", "do-not-export");
        settings.sync();
        core::BackupStore store(directory.path());
        QSignalSpy before(&store, &core::BackupStore::aboutToRestore);
        QSignalSpy restored(&store, &core::BackupStore::restored);
        QVERIFY(store.createLocal());
        QCOMPARE(store.localBackups().size(), 1);
        const auto archive = store.localBackups().first();
        const auto document = QJsonDocument::fromJson(read(archive)).object();
        QVERIFY(!document.value("settings").toObject().contains("backup/password"));
        for (const auto &entry : document.value("files").toArray()) {
            const auto item = entry.toObject();
            QVERIFY(item.value("path") != "Country.mmdb");
            if (item.value("path") == "profiles.json") {
                const auto content = QByteArray::fromBase64(item.value("data").toString().toLatin1());
                QVERIFY(!QJsonDocument::fromJson(content).object().contains("secret"));
            }
        }
        QVERIFY(write(directory.path() + "/profiles/one.yaml", "changed: true\n"));
        QVERIFY(write(directory.path() + "/profiles/extra.yaml", "must: disappear\n"));
        settings.setValue("startup/startCore", false);
        settings.sync();
        QVERIFY(store.restoreLocal(archive));
        QCOMPARE(before.size(), 1);
        QCOMPARE(restored.size(), 1);
        QCOMPARE(read(directory.path() + "/profiles/one.yaml"), QByteArray("proxies: []\nmode: rule\n"));
        QVERIFY(!QFileInfo::exists(directory.path() + "/profiles/extra.yaml"));
        QCOMPARE(read(directory.path() + "/Country.mmdb"), QByteArray("keep-cache"));
        settings.sync();
        QCOMPARE(settings.value("startup/startCore").toBool(), true);
        QCOMPARE(settings.value("window/geometry").toByteArray(), QByteArray("binary\0geometry", 15));
        QCOMPARE(settings.value("backup/password").toString(), QString("do-not-export"));
    }
    void rejectsTraversalTamperingAndMissingFilesBeforeStoppingCore() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        core::BackupStore store(directory.path());
        QVERIFY(store.createLocal());
        const auto original = QJsonDocument::fromJson(read(store.localBackups().first())).object();
        QSignalSpy before(&store, &core::BackupStore::aboutToRestore);
        const auto malicious = directory.path() + "/invalid.cqtbackup";
        for (int variant = 0; variant < 4; ++variant) {
            auto document = original;
            auto entries = document.value("files").toArray();
            auto item = entries.first().toObject();
            if (variant == 0) item["path"] = "profiles/../../escape";
            if (variant == 1) item["sha256"] = "incorrect";
            if (variant == 2) entries.append(item);
            if (variant == 3) {
                for (int i = entries.size() - 1; i >= 0; --i)
                    if (entries[i].toObject().value("path") == "profiles/one.yaml") entries.removeAt(i);
            }
            entries[0] = item;
            document["files"] = entries;
            QVERIFY(write(malicious, QJsonDocument(document).toJson()));
            QVERIFY(!store.restoreLocal(malicious));
            QCOMPARE(before.size(), 0);
            QCOMPARE(read(directory.path() + "/profiles/one.yaml"), QByteArray("proxies: []\nmode: rule\n"));
        }
    }
    void rejectsOversizedArchiveAndSymlinkTarget() {
        QTemporaryDir directory, elsewhere;
        QVERIFY(seed(directory.path()));
        core::BackupStore store(directory.path());
        QVERIFY(store.createLocal());
        const auto archive = store.localBackups().first();
        QFile oversized(directory.path() + "/large.cqtbackup");
        QVERIFY(oversized.open(QIODevice::WriteOnly));
        QVERIFY(oversized.resize(32 * 1024 * 1024 + 1));
        oversized.close();
        QVERIFY(!store.restoreLocal(oversized.fileName()));
        QVERIFY(QFile::remove(directory.path() + "/profiles/one.yaml"));
        QVERIFY(QDir(directory.path() + "/profiles").removeRecursively());
        QVERIFY(QFile::link(elsewhere.path(), directory.path() + "/profiles"));
        QSignalSpy before(&store, &core::BackupStore::aboutToRestore);
        QVERIFY(!store.restoreLocal(archive));
        QCOMPARE(before.size(), 0);
        QVERIFY(QDir(elsewhere.path()).isEmpty());
    }

    void rejectsCaseUnicodeAndWindowsPathAliases() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        core::BackupStore store(directory.path());
        QVERIFY(store.createLocal());
        const auto original = QJsonDocument::fromJson(read(store.localBackups().first())).object();
        QSignalSpy before(&store, &core::BackupStore::aboutToRestore);
        const auto invalid = directory.path() + "/invalid.cqtbackup";
        const QStringList names{"profiles/ONE.yaml", "profiles/NUL.yaml", "profiles/one.yaml.",
                                "profiles/one.yaml ", "profiles/a:b"};
        for (const auto &name : names) {
            auto document = original;
            auto entries = document.value("files").toArray();
            auto item = entries.first().toObject();
            item["path"] = name;
            entries.append(item);
            document["files"] = entries;
            QVERIFY(write(invalid, QJsonDocument(document).toJson()));
            QVERIFY(!store.restoreLocal(invalid));
            QCOMPARE(before.size(), 0);
        }
        auto document = original;
        auto entries = document.value("files").toArray();
        auto item = entries.first().toObject();
        item["path"] = QString::fromUtf8("profiles/é.yaml");
        entries.append(item);
        item["path"] = QString::fromUtf8("profiles/é.yaml");
        entries.append(item);
        document["files"] = entries;
        QVERIFY(write(invalid, QJsonDocument(document).toJson()));
        QVERIFY(!store.restoreLocal(invalid));
        QCOMPARE(before.size(), 0);
    }

    void failedInstallRollsBackAlreadyReplacedFiles() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        core::BackupStore store(directory.path());
        QVERIFY(store.createLocal());
        const auto archive = store.localBackups().first();
        const QByteArray currentIndex = R"({"profiles":[{"uid":"one","name":"Keep this name","file":"one.yaml"}],"currentUid":"one","secret":"keep-current-secret"})";
        QVERIFY(write(directory.path() + "/profiles.json", currentIndex));
        QVERIFY(write(directory.path() + "/profiles/one.yaml", "mode: direct\nproxies: []\n"));
        QSignalSpy restored(&store, &core::BackupStore::restored);
        bool injected = false;
        connect(&store, &core::BackupStore::aboutToRestore, this, [&] {
            const auto stages = QDir(directory.path()).entryList({".restore-*"}, QDir::Dirs | QDir::Hidden);
            if (stages.size() == 1) injected = QFile::remove(directory.path() + '/' + stages.first() + "/chain.json");
        });
        QVERIFY(!store.restoreLocal(archive));
        QVERIFY(injected);
        QCOMPARE(restored.size(), 0);
        QCOMPARE(read(directory.path() + "/profiles.json"), currentIndex);
        QCOMPARE(read(directory.path() + "/profiles/one.yaml"), QByteArray("mode: direct\nproxies: []\n"));
        QVERIFY(QFileInfo::exists(directory.path() + "/chain/script.js"));
    }

    // ------------------------------------------------------------- presets
    //
    // presets.json and presets.last-good.json are newer than this archive
    // format. The four functions below fix what that costs: a backup that
    // carries them, an old archive that still restores and clears them, an
    // archive whose preset document this build cannot load being refused
    // before anything is touched, and a corrupt preset file on disk not being
    // able to stop a backup or to reach one.

    void presetsRoundTripThroughBackupAndRestore() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        QVERIFY(write(directory.path() + "/presets.json", presets("global")));
        QVERIFY(write(directory.path() + "/presets.last-good.json", presets("rule")));
        core::BackupStore store(directory.path());
        QVERIFY(store.createLocal());
        const auto archive = store.localBackups().first();
        const auto files = archiveFiles(read(archive));
        QCOMPARE(files.value("presets.json"), presets("global"));
        QCOMPARE(files.value("presets.last-good.json"), presets("rule"));

        // Both a changed document and a missing one come back.
        QVERIFY(write(directory.path() + "/presets.json", presets("direct")));
        QVERIFY(QFile::remove(directory.path() + "/presets.last-good.json"));
        QVERIFY(store.restoreLocal(archive));
        QCOMPARE(read(directory.path() + "/presets.json"), presets("global"));
        QCOMPARE(read(directory.path() + "/presets.last-good.json"), presets("rule"));
        QCOMPARE(read(directory.path() + "/profiles/one.yaml"), QByteArray("proxies: []\nmode: rule\n"));
    }

    void archiveWithoutPresetsRestoresTheEmptyDocument() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));  // no preset files: a data directory that predates them
        core::BackupStore store(directory.path());
        QVERIFY(store.createLocal());
        const auto complete = read(store.localBackups().first());
        QCOMPARE(archiveFiles(complete).value("presets.json"), emptyPresets());
        QCOMPARE(archiveFiles(complete).value("presets.last-good.json"), emptyPresets());

        const auto legacy = directory.path() + "/legacy.cqtbackup";
        QVERIFY(write(legacy, withoutFiles(complete, {"presets.json", "presets.last-good.json"})));
        // Presets written after that archive was taken. They belong to profiles
        // the archive is about to replace, so keeping them would be wrong.
        QVERIFY(write(directory.path() + "/presets.json", presets("global")));
        QVERIFY(write(directory.path() + "/presets.last-good.json", presets("global")));
        QSignalSpy restored(&store, &core::BackupStore::restored);
        QVERIFY(store.restoreLocal(legacy));
        QCOMPARE(restored.size(), 1);
        QCOMPARE(read(directory.path() + "/presets.json"), emptyPresets());
        QCOMPARE(read(directory.path() + "/presets.last-good.json"), emptyPresets());
        QVERIFY(loadable(read(directory.path() + "/presets.json")));
        QCOMPARE(read(directory.path() + "/profiles/one.yaml"), QByteArray("proxies: []\nmode: rule\n"));
    }

    void rejectsUnloadablePresetDocumentsBeforeTouchingAnyFile() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        QVERIFY(write(directory.path() + "/presets.json", presets("global")));
        QVERIFY(write(directory.path() + "/presets.last-good.json", presets("rule")));
        core::BackupStore store(directory.path());
        QVERIFY(store.createLocal());
        const auto original = read(store.localBackups().first());
        QSignalSpy before(&store, &core::BackupStore::aboutToRestore);
        QSignalSpy errors(&store, &core::BackupStore::errorOccurred);
        const QList<QByteArray> unusable{
            "{ half written",
            R"(["not","an","object"])",
            R"({"global":[],"profiles":{}})",
            R"({"version":2,"global":[],"profiles":{}})",
            R"({"version":1,"global":[{"id":"a"},{"id":"a"}],"profiles":{}})",
            R"({"version":1,"global":[{"id":"a","operations":[{"op":"merge","path":"/secret","value":"x"}]}],"profiles":{}})",
            R"({"version":1,"global":[{"id":"a","operations":[{"op":"blend","path":"/mode","value":"x"}]}],"profiles":{}})",
        };
        const auto invalid = directory.path() + "/invalid.cqtbackup";
        for (const auto &payload : unusable) {
            for (const auto &path : {QStringLiteral("presets.json"),
                                     QStringLiteral("presets.last-good.json")}) {
                QVERIFY(write(invalid, withFile(original, path, payload)));
                QVERIFY2(!store.restoreLocal(invalid),
                         qPrintable(path + ' ' + QString::fromLatin1(payload)));
                QCOMPARE(before.size(), 0);
                QCOMPARE(read(directory.path() + "/presets.json"), presets("global"));
                QCOMPARE(read(directory.path() + "/presets.last-good.json"), presets("rule"));
                QCOMPARE(read(directory.path() + "/profiles/one.yaml"),
                         QByteArray("proxies: []\nmode: rule\n"));
                QCOMPARE(store.localBackups().size(), 1);
            }
        }
        QCOMPARE(errors.size(), unusable.size() * 2);
        // The same document is refused on the way in, so it never becomes a
        // local backup that looks restorable.
        QVERIFY(!store.importArchive(invalid));
        QCOMPARE(store.localBackups().size(), 1);
    }

    void snapshotSubstitutesForAnUnusablePresetFile() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        QVERIFY(write(directory.path() + "/presets.json", "{ half written"));
        QVERIFY(write(directory.path() + "/presets.last-good.json", presets("rule")));
        core::BackupStore store(directory.path());
        QSignalSpy errors(&store, &core::BackupStore::errorOccurred);
        QVERIFY(store.createLocal());
        QCOMPARE(errors.size(), 0);
        auto files = archiveFiles(read(store.localBackups().first()));
        QCOMPARE(files.value("presets.json"), presets("rule"));
        QCOMPARE(files.value("presets.last-good.json"), presets("rule"));
        // Read, not repaired: the evidence of what went wrong stays on disk.
        QCOMPARE(read(directory.path() + "/presets.json"), QByteArray("{ half written"));

        // A missing last-good copy is filled from the document it would recover.
        QTest::qWait(5);
        QVERIFY(write(directory.path() + "/presets.json", presets("global")));
        QVERIFY(QFile::remove(directory.path() + "/presets.last-good.json"));
        QVERIFY(store.createLocal());
        files = archiveFiles(read(store.localBackups().first()));
        QCOMPARE(files.value("presets.json"), presets("global"));
        QCOMPARE(files.value("presets.last-good.json"), presets("global"));

        // Neither usable: the empty document, and still a backup.
        QTest::qWait(5);
        QVERIFY(write(directory.path() + "/presets.json", "{ half written"));
        QVERIFY(write(directory.path() + "/presets.last-good.json", "also broken"));
        QVERIFY(store.createLocal());
        files = archiveFiles(read(store.localBackups().first()));
        QCOMPARE(files.value("presets.json"), emptyPresets());
        QCOMPARE(files.value("presets.last-good.json"), emptyPresets());
        QCOMPARE(errors.size(), 0);
        QCOMPARE(store.localBackups().size(), 3);
    }

    void webDavRoundTripAndRedirectRejection() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        core::BackupStore store(directory.path());
        WebDavFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto url = QString("http://127.0.0.1:%1/backup.cqtbackup").arg(server.serverPort());
        QSignalSpy statuses(&store, &core::BackupStore::statusChanged);
        QSignalSpy errors(&store, &core::BackupStore::errorOccurred);
        QSignalSpy changed(&store, &core::BackupStore::backupsChanged);
        store.uploadWebDav(url, "fixture", "password");
        QTRY_VERIFY(!store.isBusy());
        QTRY_COMPARE(server.requests.size(), 1);
        QCOMPARE(errors.size(), 0);
        QVERIFY(QJsonDocument::fromJson(server.stored).isObject());
        QVERIFY(server.requests.first().startsWith("PUT /backup.cqtbackup HTTP/"));
        QVERIFY(server.requests.first().contains("Authorization: Basic " + QByteArray("fixture:password").toBase64()));
        store.downloadWebDav(url, "fixture", "password");
        QTRY_COMPARE(changed.size(), 1);
        QTRY_VERIFY(!store.isBusy());
        QCOMPARE(store.localBackups().size(), 1);
        QCOMPARE(errors.size(), 0);
        server.status = "302 Found";
        store.downloadWebDav(url, "fixture", "password");
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(server.requests.size(), 3);
        QCOMPARE(store.localBackups().size(), 1);
        server.status = "200 OK";
        server.stored = "not an archive";
        store.downloadWebDav(url, "fixture", "password");
        QTRY_COMPARE(errors.size(), 2);
        QTRY_VERIFY(!store.isBusy());
        QCOMPARE(store.localBackups().size(), 1);
    }

    void asyncRestoreKeepsEventLoopResponsiveAndWaitsForCore() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        core::BackupStore store(directory.path());
        QVERIFY(store.createLocal());
        const auto archive = store.localBackups().first();
        QVERIFY(write(directory.path() + "/profiles/one.yaml", "proxies: []\nmode: direct\n"));
        QSignalSpy prepared(&store, &core::BackupStore::restorePrepared);
        QSignalSpy restored(&store, &core::BackupStore::restored);
        QSignalSpy completed(&store, &core::BackupStore::localOperationFinished);
        store.restoreLocalAsync(archive);
        QVERIFY(store.isBusy());
        QTRY_COMPARE(prepared.size(), 1);
        QCOMPARE(restored.size(), 0);
        QCOMPARE(read(directory.path() + "/profiles/one.yaml"), QByteArray("proxies: []\nmode: direct\n"));
        int heartbeat = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] { ++heartbeat; });
        timer.start(1);
        QElapsedTimer heartbeatDeadline;
        heartbeatDeadline.start();
        while (heartbeat < 3 && heartbeatDeadline.elapsed() < 500) QTest::qWait(20);
        QVERIFY(store.isBusy());
        store.continueRestore(true);
        QTRY_VERIFY(!store.isBusy());
        QCOMPARE(restored.size(), 1);
        QCOMPARE(completed.size(), 1);
        QCOMPARE(completed.first().first().toBool(), true);
        QVERIFY(heartbeat >= 3);
        QCOMPARE(read(directory.path() + "/profiles/one.yaml"), QByteArray("proxies: []\nmode: rule\n"));
    }

    void cancellingPreparedRestoreLeavesCurrentFilesUntouched() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        core::BackupStore store(directory.path());
        QSignalSpy completed(&store, &core::BackupStore::localOperationFinished);
        store.createLocalAsync();
        QTRY_VERIFY(!store.isBusy());
        QCOMPARE(completed.size(), 1);
        const auto archive = store.localBackups().first();
        const QByteArray current = "proxies: []\nmode: direct\n";
        QVERIFY(write(directory.path() + "/profiles/one.yaml", current));
        QSignalSpy prepared(&store, &core::BackupStore::restorePrepared);
        QSignalSpy restored(&store, &core::BackupStore::restored);
        store.restoreLocalAsync(archive);
        QTRY_COMPARE(prepared.size(), 1);
        store.cancelAsync();
        QTRY_VERIFY(!store.isBusy());
        QCOMPARE(restored.size(), 0);
        QCOMPARE(completed.size(), 2);
        QCOMPARE(completed.last().first().toBool(), false);
        QCOMPARE(read(directory.path() + "/profiles/one.yaml"), current);
    }

    void preparationGateWaitsForOtherFileWriters() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        core::BackupStore store(directory.path());
        store.requirePreparation(true);
        QSignalSpy preparing(&store, &core::BackupStore::operationPreparing);
        QSignalSpy completed(&store, &core::BackupStore::localOperationFinished);
        store.createLocalAsync();
        QCOMPARE(preparing.size(), 1);
        QTest::qWait(30);
        QVERIFY(store.isBusy());
        QVERIFY(store.localBackups().isEmpty());
        store.continuePreparation();
        QTRY_VERIFY(!store.isBusy());
        QCOMPARE(store.localBackups().size(), 1);
        QCOMPARE(completed.size(), 1);
        store.createLocalAsync();
        QCOMPARE(preparing.size(), 2);
        store.cancelAsync();
        QTRY_VERIFY(!store.isBusy());
        QCOMPARE(store.localBackups().size(), 1);
        QCOMPARE(completed.size(), 2);
        QCOMPARE(completed.last().first().toBool(), false);
    }
};

QTEST_GUILESS_MAIN(BackupTest)
#include "backup_test.moc"
