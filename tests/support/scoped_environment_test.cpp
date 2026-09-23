#include <QtTest>

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include "support/scoped_environment.h"

using testsupport::ScopedEnvironment;

class ScopedEnvironmentTest : public QObject {
    Q_OBJECT
    QTemporaryDir suitePreferences_;

private slots:
    void initTestCase() {
        // The suite must never read the developer's real clash-qt preferences,
        // including in the windows between scopes.
        QVERIFY(suitePreferences_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, suitePreferences_.path());
    }

    void createsFreshIsolatedDirectories() {
        ScopedEnvironment first(QStringLiteral("alpha"));
        ScopedEnvironment second(QStringLiteral("alpha"));
        QVERIFY2(first.isValid(), qPrintable(first.errorString()));
        QVERIFY2(second.isValid(), qPrintable(second.errorString()));
        QVERIFY(first.rootPath() != second.rootPath());
        for (const QString &directory : {first.dataDir(), first.settingsDir(), first.socketDir(),
                                         first.artifactDir()}) {
            QVERIFY2(QFileInfo(directory).isDir(), qPrintable(directory));
            QVERIFY(QDir(directory).isEmpty());
        }
    }

    void exportsDataDirAndRestoresAPriorValue() {
        qputenv("CLASH_QT_DATA_DIR", "/pre-existing/value");
        {
            ScopedEnvironment scope;
            QVERIFY(scope.isValid());
            QCOMPARE(qEnvironmentVariable("CLASH_QT_DATA_DIR"), scope.dataDir());
        }
        // Restored, not unset: this is the failure mode that silently corrupts
        // whichever sibling test happens to run next.
        QVERIFY(qEnvironmentVariableIsSet("CLASH_QT_DATA_DIR"));
        QCOMPARE(qgetenv("CLASH_QT_DATA_DIR"), QByteArray("/pre-existing/value"));
        qunsetenv("CLASH_QT_DATA_DIR");
    }

    void restoresAVariableThatWasNotSetByUnsettingIt() {
        QVERIFY(!qEnvironmentVariableIsSet("CLASH_QT_FIXTURE_PROBE"));
        {
            ScopedEnvironment scope;
            scope.setEnvironment("CLASH_QT_FIXTURE_PROBE", "scoped");
            QCOMPARE(qgetenv("CLASH_QT_FIXTURE_PROBE"), QByteArray("scoped"));
        }
        QVERIFY(!qEnvironmentVariableIsSet("CLASH_QT_FIXTURE_PROBE"));
    }

    void distinguishesAnEmptyValueFromAnUnsetVariable() {
        qputenv("CLASH_QT_FIXTURE_EMPTY", QByteArray());
        QVERIFY(qEnvironmentVariableIsSet("CLASH_QT_FIXTURE_EMPTY"));
        {
            ScopedEnvironment scope;
            scope.setEnvironment("CLASH_QT_FIXTURE_EMPTY", "filled");
            scope.unsetEnvironment("CLASH_QT_FIXTURE_EMPTY");
            QVERIFY(!qEnvironmentVariableIsSet("CLASH_QT_FIXTURE_EMPTY"));
        }
        QVERIFY2(qEnvironmentVariableIsSet("CLASH_QT_FIXTURE_EMPTY"),
                 "An empty prior value must be restored as set-but-empty, not left unset");
        QCOMPARE(qgetenv("CLASH_QT_FIXTURE_EMPTY"), QByteArray());
        qunsetenv("CLASH_QT_FIXTURE_EMPTY");
    }

    void redirectsQSettingsLikeMainDoes() {
        QString scopedFile;
        {
            ScopedEnvironment scope;
            auto settings = scope.settings();
            QCOMPARE(settings->format(), QSettings::IniFormat);
            scopedFile = settings->fileName();
            QVERIFY2(scopedFile.startsWith(scope.settingsDir()), qPrintable(scopedFile));
            settings->setValue(QStringLiteral("dashboard/browser"), QStringLiteral("fixture-browser"));
            settings->sync();
            QVERIFY(QFileInfo::exists(scopedFile));

            // The redirection main.cpp performs, reached the way main.cpp reaches it.
            QSettings redirected(QSettings::IniFormat, QSettings::UserScope,
                                 QStringLiteral("clash-qt"), QStringLiteral("clash-qt"));
            QCOMPARE(redirected.fileName(), scopedFile);
            QCOMPARE(redirected.value(QStringLiteral("dashboard/browser")).toString(),
                     QStringLiteral("fixture-browser"));
        }
        QVERIFY(!QFileInfo::exists(scopedFile));
    }

    // Records the platform contract rather than skipping: QSettings(org, app) -
    // the constructor every src/** preference call uses - is hard wired to
    // NativeFormat, so main.cpp's setDefaultFormat()/setPath() never reach it on
    // macOS or Windows. On Unix without a native backend NativeFormat is Ini and
    // the same redirection does reach it.
    void productionAccessorIsolationFollowsThePlatformContract() {
        ScopedEnvironment scope;
        QVERIFY(scope.isValid());
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        QVERIFY2(!scope.productionSettingsAreIsolated(),
                 qPrintable(QStringLiteral("QSettings(org, app) unexpectedly redirected to ") +
                            scope.productionSettingsFilePath()));
        QVERIFY(!scope.productionSettingsFilePath().startsWith(scope.settingsDir()));
#else
        QVERIFY2(scope.productionSettingsAreIsolated(),
                 qPrintable(scope.productionSettingsFilePath()));
#endif
        // The fixture leaves the real store alone, whichever branch applies.
        QVERIFY2(scope.realPreferencesUnchanged(),
                 "The fixture itself must never write the developer's preferences");
    }

    void restoresTheDefaultFormatItFound() {
        QSettings::setDefaultFormat(QSettings::NativeFormat);
        {
            ScopedEnvironment scope;
            QCOMPARE(QSettings::defaultFormat(), QSettings::IniFormat);
        }
        QCOMPARE(QSettings::defaultFormat(), QSettings::NativeFormat);
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }

    void teardownQuarantinesTheSettingsPathInsteadOfRestoringRealPreferences() {
        {
            ScopedEnvironment scope;
            QVERIFY(scope.isValid());
        }
        // QSettings::setPath() has no getter, so the pre-existing path cannot be
        // restored. The fixture points the user scope at a quarantine directory
        // rather than guessing the developer's real preference path.
        const QSettings after(QSettings::IniFormat, QSettings::UserScope,
                              QStringLiteral("clash-qt"), QStringLiteral("clash-qt"));
        QVERIFY2(after.fileName().contains(QStringLiteral("cqt-quarantine")),
                 qPrintable(after.fileName()));
        QVERIFY(after.value(QStringLiteral("dashboard/browser")).isNull());
    }

    void nestedScopesRestoreTheEnclosingScope() {
        ScopedEnvironment outer(QStringLiteral("outer"));
        QVERIFY(outer.isValid());
        outer.settings()->setValue(QStringLiteral("marker"), QStringLiteral("outer"));
        {
            ScopedEnvironment inner(QStringLiteral("inner"));
            QVERIFY(inner.isValid());
            QVERIFY(inner.settings()->value(QStringLiteral("marker")).isNull());
            inner.settings()->setValue(QStringLiteral("marker"), QStringLiteral("inner"));
            QCOMPARE(inner.settings()->value(QStringLiteral("marker")).toString(),
                     QStringLiteral("inner"));
        }
        QSettings restored(QSettings::IniFormat, QSettings::UserScope,
                           QStringLiteral("clash-qt"), QStringLiteral("clash-qt"));
        QVERIFY2(restored.fileName().startsWith(outer.settingsDir()), qPrintable(restored.fileName()));
        QCOMPARE(restored.value(QStringLiteral("marker")).toString(), QStringLiteral("outer"));
    }

    void socketPathsFitThePlatformLimit() {
        ScopedEnvironment scope(QStringLiteral("sock"));
        const QString path = scope.socketPath(QStringLiteral("privileged-service.sock"));
        QVERIFY2(!path.isEmpty(), qPrintable(scope.errorString()));
        QVERIFY(path.startsWith(scope.socketDir()));
#ifndef Q_OS_WIN
        QVERIFY2(path.toUtf8().size() <= 100, qPrintable(path));
        QVERIFY(scope.socketPath(QString(200, QLatin1Char('x'))).isEmpty());
        QVERIFY(!scope.errorString().isEmpty());
#endif
    }

    void filePathCreatesMissingParents() {
        ScopedEnvironment scope;
        const QString path = scope.filePath(QStringLiteral("profiles/nested/config.yaml"));
        QVERIFY(QFileInfo(path).absolutePath().endsWith(QStringLiteral("profiles/nested")));
        QVERIFY(QFileInfo(QFileInfo(path).absolutePath()).isDir());
    }

    void processEnvironmentCarriesTheScopedOverrides() {
        ScopedEnvironment scope;
        scope.setEnvironment("CLASH_QT_FIXTURE_CHILD", "yes");
        const QProcessEnvironment child = scope.processEnvironment();
        QCOMPARE(child.value(QStringLiteral("CLASH_QT_DATA_DIR")), scope.dataDir());
        QCOMPARE(child.value(QStringLiteral("CLASH_QT_FIXTURE_CHILD")), QStringLiteral("yes"));
    }

    void keptArtifactsSurviveTeardown() {
        QString root;
        {
            ScopedEnvironment scope;
            root = scope.rootPath();
            QFile marker(scope.artifactDir() + QStringLiteral("/failure.log"));
            QVERIFY(marker.open(QIODevice::WriteOnly));
            marker.write("kept");
            marker.close();
            scope.keepArtifacts();
        }
        QVERIFY(QFileInfo::exists(root + QStringLiteral("/artifacts/failure.log")));
        QVERIFY(QDir(root).removeRecursively());
    }
};

QTEST_GUILESS_MAIN(ScopedEnvironmentTest)
#include "scoped_environment_test.moc"
