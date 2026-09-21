// Scoped per-test environment: fresh data/settings/socket/artifact directories,
// with *restoration* (not merely unsetting) of every environment variable and of
// the QSettings redirection that src/main.cpp performs for CLASH_QT_DATA_DIR.
//
// Construct one per test function (or per suite in initTestCase()); destruction
// restores the process state it captured. Never a singleton: consumers own the
// instance and its lifetime is the isolation boundary.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QProcessEnvironment>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace testsupport {

class ScopedEnvironment {
public:
    // `label` is embedded in the temporary directory name to make stray
    // directories attributable. Keep it short: socket paths are length-limited.
    explicit ScopedEnvironment(const QString &label = QString());
    ~ScopedEnvironment();

    ScopedEnvironment(const ScopedEnvironment &) = delete;
    ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

    bool isValid() const;
    QString errorString() const;

    // Directory layout. All exist once isValid() is true.
    QString rootPath() const;      // parent of everything below
    QString dataDir() const;       // exported as CLASH_QT_DATA_DIR
    QString settingsDir() const;   // QSettings IniFormat/UserScope path
    QString socketDir() const;     // short enough for AF_UNIX sun_path
    QString artifactDir() const;   // failure artifacts; survives when kept

    // A unique socket path inside socketDir(). Asserts (returns empty and sets
    // errorString) if the platform socket-name limit would be exceeded.
    QString socketPath(const QString &name) const;
    // Path under rootPath(); missing parent directories are created.
    QString filePath(const QString &relative) const;

    // Environment overrides. The prior value - including "was not set at all" -
    // is captured on first use of a name and restored on destruction.
    void setEnvironment(const QByteArray &name, const QByteArray &value);
    void unsetEnvironment(const QByteArray &name);

    // The scoped variables, for handing to a QProcess child.
    QProcessEnvironment processEnvironment() const;

    // QSettings bound to settingsDir(), in the format main.cpp selects.
    std::unique_ptr<QSettings> settings() const;
    QString settingsFilePath() const;

    // Where a production-style QSettings("clash-qt", "clash-qt") actually lands.
    QString productionSettingsFilePath() const;
    // True when that path is inside settingsDir().
    //
    // FALSE on macOS and Windows: QSettings(organization, application) is hard
    // wired to NativeFormat and ignores both setDefaultFormat() and setPath(),
    // so the redirection main.cpp performs never reaches production's own
    // accessors. Every src/** preference read and write uses that constructor.
    bool productionSettingsAreIsolated() const;
    // Best-effort guard for the case above: has the real user preference store
    // changed since this scope was created? A false result means a test wrote
    // into the developer's own preferences. Read-only, so a write the OS
    // preferences daemon has not yet flushed can escape it.
    bool realPreferencesUnchanged() const;

    // Keep rootPath() after destruction, for triaging a failed test.
    void keepArtifacts();

private:
    struct PriorValue {
        bool wasSet = false;
        QByteArray value;
    };

    void captureEnvironment(const QByteArray &name);

    QTemporaryDir root_;
    mutable QString error_;
    QHash<QByteArray, PriorValue> priorEnvironment_;
    QByteArray realPreferenceSnapshot_;
    QSettings::Format priorSettingsFormat_ = QSettings::NativeFormat;
    bool settingsRedirected_ = false;
    bool valid_ = false;
};

} // namespace testsupport
