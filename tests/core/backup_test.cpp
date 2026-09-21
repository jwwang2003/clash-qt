#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "core/backups/backup_store.h"

namespace {
bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
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
    QTemporaryDir preferences_;
private slots:
    void initTestCase() {
        QVERIFY(preferences_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences_.path());
    }
    void snapshotAndRestore() {
        QTemporaryDir directory;
        QVERIFY(seed(directory.path()));
        QVERIFY(write(directory.path() + "/Country.mmdb", "keep-cache"));
        QSettings settings("clash-qt", "clash-qt");
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
