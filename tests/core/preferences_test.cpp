// The regression guard for the settings-isolation defect.
//
// A CTest run once wrote into the developer's real macOS preferences, because
// every production site constructed QSettings("clash-qt", "clash-qt") inline
// and src/main.cpp tried to isolate it with QSettings::setDefaultFormat() plus
// QSettings::setPath(). Neither of those reaches that constructor on macOS: it
// is hard wired to NativeFormat, so the redirection silently did nothing and
// the CFPreferences domain com.clash-qt.clash-qt stayed live.
//
// Two halves, because one alone lets the defect return:
//   * the runtime half proves core::preferences::open() resolves and writes
//     beneath CLASH_QT_DATA_DIR and leaves the native domain untouched;
//   * the source half proves no production file has gone back to constructing
//     the store, or to the global mechanism that cannot isolate it.
//
// Nothing here ever writes to com.clash-qt.clash-qt. The native domain is only
// ever read, and only to prove a value did not reach it.

#include <QtTest>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QUuid>

#include "core/preferences/preferences.h"
#include "support/scoped_environment.h"

namespace {

const QString kOrganization = QString::fromLatin1(core::preferences::kOrganization);
const QString kApplication = QString::fromLatin1(core::preferences::kApplication);

// Every production key, so a rename of any of them is caught here rather than
// by a user losing a preference.
const QStringList kProductionKeys{
    "core/useService", "core/binary", "startup/startCore", "window/geometry",
    "proxies/sort", "backup/password", "backup/webdavUrl", "backup/webdavUser",
    "dashboard/browser", "sysproxy/bypass", "hotkeys/toggle-window",
};

QString uniqueSentinel() {
    return QStringLiteral("isolation-sentinel-") +
           QUuid::createUuid().toString(QUuid::Id128);
}

// Reads the native store. On macOS this goes through cfprefsd, which answers
// from its in-memory state, so a write the daemon has not yet flushed to disk
// is still visible here. That is deliberate: a file-modification-time or
// file-hash check alone would let an unflushed leak pass.
//
// Read-only. QSettings only writes in its destructor when it has pending
// changes, and this makes none.
bool nativeStoreHolds(const QString &value) {
    QSettings native(kOrganization, kApplication);
    for (const QString &key : native.allKeys())
        if (native.value(key).toString() == value) return true;
    return false;
}

QStringList productionSources(const QString &root) {
    QStringList files;
    QDirIterator it(root + QStringLiteral("/src"), {"*.cpp", "*.h", "*.mm"},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) files.append(it.next());
    files.sort();
    return files;
}

bool isComment(const QString &line) {
    const QString trimmed = line.trimmed();
    return trimmed.startsWith(QLatin1String("//")) || trimmed.startsWith(QLatin1Char('*')) ||
           trimmed.startsWith(QLatin1String("/*"));
}

}  // namespace

class PreferencesTest : public QObject {
    Q_OBJECT
private slots:

    // Direction one: CLASH_QT_DATA_DIR set. Everything lands under it and the
    // native store never sees any of it.
    void isolatesEveryReadAndWriteBeneathTheDataDirectory() {
        testsupport::ScopedEnvironment env(QStringLiteral("prefs"));
        QVERIFY2(env.isValid(), qPrintable(env.errorString()));

        const QString dataDir = QFileInfo(env.dataDir()).absoluteFilePath();

        // Asserted BEFORE anything is written. If isolation regresses, the run
        // stops here rather than going on to write into the real preferences.
        QVERIFY2(core::preferences::isIsolated(),
                 "CLASH_QT_DATA_DIR is set but the accessor did not isolate");
        const QString resolved = core::preferences::fileName();
        QVERIFY2(QDir::isAbsolutePath(resolved), qPrintable(resolved));
        QVERIFY2(resolved.startsWith(dataDir + QLatin1Char('/')),
                 qPrintable(QStringLiteral("Preferences resolve to %1, outside the isolated %2")
                                .arg(resolved, dataDir)));
        QVERIFY2(resolved != env.productionSettingsFilePath(),
                 qPrintable(QStringLiteral("Preferences still resolve to the native store at ") +
                            resolved));
        QCOMPARE(core::preferences::open().format(), QSettings::IniFormat);
        QCOMPARE(core::preferences::isolatedRoot(), dataDir);

        // Now write, under every production key, values that cannot collide
        // with anything a real user has.
        QHash<QString, QString> written;
        {
            QSettings settings = core::preferences::open();
            for (const QString &key : kProductionKeys) {
                written.insert(key, uniqueSentinel());
                settings.setValue(key, written.value(key));
            }
            settings.sync();
            QCOMPARE(settings.status(), QSettings::NoError);
            QCOMPARE(settings.fileName(), resolved);
        }

        // The isolated file exists, is under the data directory, and holds it.
        QVERIFY2(QFileInfo::exists(resolved), qPrintable(resolved));
        QFile file(resolved);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray contents = file.readAll();
        for (const QString &key : kProductionKeys)
            QVERIFY2(contents.contains(written.value(key).toUtf8()),
                     qPrintable(QStringLiteral("%1 is not in %2").arg(key, resolved)));

        // A fresh accessor reads the same values back under the same keys.
        {
            QSettings reopened = core::preferences::open();
            for (const QString &key : kProductionKeys)
                QCOMPARE(reopened.value(key).toString(), written.value(key));
        }

        // Nothing reached the native store. Checked through CFPreferences, so
        // an unflushed write cannot hide, and then again against the real
        // preference file's content hash.
        for (const QString &key : kProductionKeys)
            QVERIFY2(!nativeStoreHolds(written.value(key)),
                     qPrintable(QStringLiteral("%1 leaked into the native store at %2")
                                    .arg(key, env.productionSettingsFilePath())));
        QVERIFY2(env.realPreferencesUnchanged(),
                 qPrintable(QStringLiteral("The real user preference store at %1 changed")
                                .arg(env.productionSettingsFilePath())));
    }

    // A scratch identity resolves the same way, which is what lets the
    // unisolated direction below be checked without writing anywhere real.
    void isolationIsIndependentOfTheIdentity() {
        testsupport::ScopedEnvironment env(QStringLiteral("prefs2"));
        QVERIFY2(env.isValid(), qPrintable(env.errorString()));
        const QString dataDir = QFileInfo(env.dataDir()).absoluteFilePath();
        const QString scratch = core::preferences::fileNameFor(
            QStringLiteral("cqt-scratch-org"), QStringLiteral("cqt-scratch-app"));
        QVERIFY2(scratch.startsWith(dataDir + QLatin1Char('/')), qPrintable(scratch));
        QVERIFY(scratch != core::preferences::fileName());

        const QString sentinel = uniqueSentinel();
        {
            QSettings settings = core::preferences::openAs(QStringLiteral("cqt-scratch-org"),
                                                           QStringLiteral("cqt-scratch-app"));
            settings.setValue(QStringLiteral("window/geometry"), sentinel);
            settings.sync();
            QCOMPARE(settings.status(), QSettings::NoError);
        }
        QVERIFY(QFileInfo::exists(scratch));
        QVERIFY(!nativeStoreHolds(sentinel));
        QVERIFY(env.realPreferencesUnchanged());
    }

    // Direction two: CLASH_QT_DATA_DIR unset. The accessor must be the native
    // store, unchanged - same format, scope, identity and file - so that an
    // existing user's preferences stay exactly where they are.
    //
    // Read-only throughout. Proving the location is identical is the proof that
    // reads and writes land where they always did; writing a probe value into
    // com.clash-qt.clash-qt to demonstrate it is the very pollution under test.
    void matchesTheNativeStoreExactlyWhenNotIsolated() {
        testsupport::ScopedEnvironment env(QStringLiteral("prefs3"));
        QVERIFY2(env.isValid(), qPrintable(env.errorString()));
        env.unsetEnvironment(QByteArrayLiteral("CLASH_QT_DATA_DIR"));

        QVERIFY(!core::preferences::isIsolated());
        QVERIFY(core::preferences::isolatedRoot().isEmpty());

        const QSettings native(kOrganization, kApplication);
        const QSettings accessor = core::preferences::open();
        QCOMPARE(accessor.fileName(), native.fileName());
        QCOMPARE(accessor.format(), native.format());
        QCOMPARE(accessor.scope(), native.scope());
        QCOMPARE(accessor.organizationName(), native.organizationName());
        QCOMPARE(accessor.applicationName(), native.applicationName());
        QCOMPARE(core::preferences::fileName(), native.fileName());
        QCOMPARE(core::preferences::fileName(), env.productionSettingsFilePath());
#ifdef Q_OS_DARWIN
        // The platform contract this module exists for: on macOS the identity
        // constructor is the native CFPreferences domain, and nothing the
        // process does globally can move it.
        QCOMPARE(native.format(), QSettings::NativeFormat);
#endif
        QVERIFY(env.realPreferencesUnchanged());
    }

    // The runtime checks above only exercise the accessor. This one fails when
    // a production file goes back to building the store itself, or to the
    // global mechanism that cannot isolate it on macOS.
    //
    // src/platform/system/autostart.cpp and src/platform/proxy/system_proxy.cpp
    // use QSettings with an explicit NativeFormat to reach the Windows registry.
    // That is OS integration, not application preferences, and the patterns
    // below are deliberately narrow enough not to touch it.
    void noProductionSiteReachesTheStoreWithoutTheAccessor() {
        const QString root = QString::fromUtf8(CLASH_QT_SOURCE_DIR);
        QVERIFY2(QFileInfo(root + QStringLiteral("/src")).isDir(), qPrintable(root));
        const QString accessorDir = root + QStringLiteral("/src/core/preferences/");

        const QStringList files = productionSources(root);
        QVERIFY(files.size() > 50);

        QStringList offences;
        int accessorCalls = 0;
        for (const QString &path : files) {
            QFile file(path);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
            const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size(); ++i) {
                const QString &line = lines.at(i);
                if (line.contains(QLatin1String("preferences::open"))) ++accessorCalls;
                if (path.startsWith(accessorDir) || isComment(line)) continue;
                const auto flag = [&](const char *what) {
                    offences.append(QStringLiteral("%1:%2: %3 -- %4")
                                        .arg(QDir(root).relativeFilePath(path))
                                        .arg(i + 1)
                                        .arg(QString::fromLatin1(what), line.trimmed()));
                };
                if (line.contains(QRegularExpression(
                        QStringLiteral("QSettings\\s*(<[^>]*>)?\\s*\\(\\s*\"clash-qt\""))))
                    flag("constructs the preference store directly; use core::preferences::open()");
                if (line.contains(QLatin1String("QSettings::setDefaultFormat")) ||
                    line.contains(QLatin1String("QSettings::setPath")))
                    flag("QSettings global state cannot redirect QSettings(org, app) on macOS");
            }
        }
        QVERIFY2(offences.isEmpty(), qPrintable(QStringLiteral("\n") + offences.join(QLatin1Char('\n'))));
        // Guards against the scan passing vacuously if the accessor is dropped.
        QVERIFY2(accessorCalls >= 10,
                 qPrintable(QStringLiteral("Only %1 call sites use the accessor").arg(accessorCalls)));
    }
};

QTEST_GUILESS_MAIN(PreferencesTest)
#include "preferences_test.moc"
