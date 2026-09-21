// Profile storage and subscriptions: rejection of non-HTTP sources, cache and
// quota preservation across a URL edit, cancellation of in-flight imports, and the
// validate-then-drain contract of the asynchronous writers.
//
// Partition of the former `runtime` suite (tests/core/runtime_test.cpp). Cases are
// carried over verbatim; see tests/README.md for the full original-to-new map.
#include <QtTest>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <memory>

#include "core/config/enhance/config_enhancer.h"
#include "core/profiles/profile_store.h"
#include "support/fixture_files.h"
#include "support/scoped_environment.h"

class ProfileStoreTest : public QObject {
    Q_OBJECT
private slots:
    // One scoped environment per test function, as the original suite had one
    // QTemporaryDir per test function. ScopedEnvironment additionally *restores*
    // any CLASH_QT_DATA_DIR the developer had exported, where the original
    // cleanup() unset it and lost it for the rest of the process.
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("prof"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
    }
    void cleanup() { environment_.reset(); }
    void invalidEditAndRuntimePreserveFiles() {
        core::ProfileStore store;
        const QString input = environment_->filePath("sample.yaml");
        testsupport::writeFile(input, "proxies: []\n");
        store.importFromFile(input);
        const QString path = store.profiles().first().filePath;
        const QByteArray original = testsupport::readFile(path);
        QVERIFY(!store.saveProfileContent(store.currentUid(), "[]"));
        QCOMPARE(testsupport::readFile(path), original);
        QVERIFY(store.saveProfileContent(store.currentUid(), "proxies: []\nmode: direct\n"));
        const QString runtime = store.generateRuntimeConfig();
        const QByteArray generated = testsupport::readFile(runtime);
        testsupport::writeFile(path, "[]");
        QVERIFY(store.generateRuntimeConfig().isEmpty());
        QCOMPARE(testsupport::readFile(runtime), generated);
        QVERIFY(!store.setRuntimeOverrides({{"mixed-port", 0}}));
        QVERIFY(!store.setRuntimeOverrides({{"mixed-port", 3.5}}));
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
        QCOMPARE(testsupport::readFile(original.filePath), cached);
        QVERIFY(!store.setSubscriptionUrl(original.uid, "file:///not-a-subscription"));
        QVERIFY(!store.setSubscriptionUrl(original.uid, "https://"));
        QCOMPARE(store.profiles().first().url, replacementUrl);
        core::ProfileStore reopened;
        reopened.load();
        QCOMPARE(reopened.profiles().size(), 1);
        QCOMPARE(reopened.profiles().first().url, replacementUrl);
        QCOMPARE(reopened.profiles().first().uid, original.uid);
        QCOMPARE(testsupport::readFile(reopened.profiles().first().filePath), cached);
        // The canceled request must also release the refresh lock for the new URL.
        store.updateProfile(original.uid);
        QTRY_VERIFY(server.hasPendingConnections());
        QTcpSocket *replacement = server.nextPendingConnection();
        QTRY_VERIFY(replacement->bytesAvailable() > 0);
        QVERIFY(replacement->readAll().contains("/replacement?token=new"));
        const QByteArray newCache = "proxies: []\nmode: direct\n";
        respond(replacement, newCache, "download=10; total=2000");
        QTRY_COMPARE(updated.size(), 1);
        QCOMPARE(testsupport::readFile(original.filePath), newCache);
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
    void asyncProfileAndEnhancementWritesValidateAndDrainOnShutdown() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        const QString input = environment_->filePath("async-profile.yaml");
        testsupport::writeFile(input, "proxies: []\nmode: rule\n");
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
        QCOMPARE(testsupport::readFile(profile.filePath), QByteArray("proxies: []\nmode: rule\n"));
        store.saveProfileContentAsync(profile.uid, "proxies: []\nmode: direct\n");
        store.beginShutdown();
        store.createLocalProfileAsync("rejected", "proxies: []\n");
        QTRY_COMPARE(saved.size(), 2);
        QVERIFY(saved.last()[1].toBool());
        QTRY_VERIFY(!store.isFileBusy());
        QCOMPARE(testsupport::readFile(profile.filePath), QByteArray("proxies: []\nmode: direct\n"));
        QCOMPARE(store.profiles().size(), 1);
        // Straddles the profiles/config boundary: from here on the case repeats
        // the identical busy / validate / drain-on-shutdown contract against
        // core::ConfigEnhancer instead of core::ProfileStore. It is two tests in
        // one body, kept whole so neither half's assertions are re-derived on a
        // duplicated fixture. Recorded in tests/README.md; the config package
        // owns the second half.
        const QString fragment = environment_->filePath("async-merge.yaml");
        testsupport::writeFile(fragment, "mode: direct\n");
        enhancer.importItemAsync(fragment, core::ChainKind::Merge);
        QVERIFY(enhancer.isFileBusy());
        QTRY_COMPARE(enhancer.chain().size(), 1);
        QSignalSpy itemSaved(&enhancer, &core::ConfigEnhancer::itemContentSaved);
        const auto item = enhancer.chain().first();
        enhancer.saveItemContentAsync(item.uid, "[]");
        QTRY_COMPARE(itemSaved.size(), 1);
        QVERIFY(!itemSaved.last()[1].toBool());
        QCOMPARE(testsupport::readFile(item.filePath), QByteArray("mode: direct\n"));
        enhancer.saveItemContentAsync(item.uid, "mode: global\n");
        enhancer.beginShutdown();
        QTRY_COMPARE(itemSaved.size(), 2);
        QVERIFY(itemSaved.last()[1].toBool());
        QTRY_VERIFY(!enhancer.isFileBusy());
        QCOMPARE(testsupport::readFile(item.filePath), QByteArray("mode: global\n"));
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(ProfileStoreTest)
#include "profile_store_test.moc"
