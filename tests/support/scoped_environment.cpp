#include "support/scoped_environment.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>

namespace testsupport {
namespace {

// QSettings::setPath() is process-global and has no getter, so the prior path
// cannot be read back. Two consequences, both deliberate:
//
//  * Nested scopes restore the enclosing scope's directory, tracked here.
//  * The outermost scope cannot restore "whatever Qt would have used" - it
//    would have to guess the real user preference path. Rather than guess, it
//    points IniFormat/UserScope at a process-lifetime quarantine directory, so
//    a later test that forgot to isolate still cannot read or write the
//    developer's real clash-qt/clash-qt preferences. The redirection is never
//    left pointing at a deleted directory either.
QList<const ScopedEnvironment *> &scopeStack() {
    static QList<const ScopedEnvironment *> stack;
    return stack;
}

QString quarantineDir() {
    static QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/cqt-quarantine-XXXXXX"));
    return dir.isValid() ? dir.path() : QDir::tempPath();
}

// A content hash of the real preference store, so a leak is detectable without
// ever surfacing what the store contains.
QByteArray snapshotOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QByteArrayLiteral("absent");
    return QCryptographicHash::hash(file.read(1 << 20), QCryptographicHash::Sha256);
}

// AF_UNIX sun_path is 104 bytes on macOS and 108 on Linux; QLocalServer on
// Windows uses named pipes and is not length-constrained in the same way.
constexpr int kMaxSocketPathLength = 100;

QString sanitizedLabel(const QString &label) {
    QString out;
    for (const QChar c : label) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-')) out.append(c.toLower());
        if (out.size() >= 8) break;
    }
    return out;
}

QString templatePath(const QString &label) {
    const QString tag = sanitizedLabel(label);
    return QDir::tempPath() + QStringLiteral("/cqt-") +
           (tag.isEmpty() ? QString() : tag + QLatin1Char('-')) + QStringLiteral("XXXXXX");
}

} // namespace

ScopedEnvironment::ScopedEnvironment(const QString &label) : root_(templatePath(label)) {
    if (!root_.isValid()) {
        error_ = QStringLiteral("Cannot create a scoped temporary directory: ") + root_.errorString();
        return;
    }
    const QDir root(root_.path());
    for (const char *name : {"data", "settings", "artifacts", "s"}) {
        if (!root.mkpath(QLatin1String(name))) {
            error_ = QStringLiteral("Cannot create %1/%2").arg(root_.path(), QLatin1String(name));
            return;
        }
    }

    setEnvironment(QByteArrayLiteral("CLASH_QT_DATA_DIR"), QFile::encodeName(dataDir()));

    // Mirror src/main.cpp: an isolated data directory switches QSettings to the
    // INI format and redirects the user scope into that directory.
    priorSettingsFormat_ = QSettings::defaultFormat();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir());
    settingsRedirected_ = true;
    scopeStack().append(this);

    if (!productionSettingsAreIsolated())
        realPreferenceSnapshot_ = snapshotOf(productionSettingsFilePath());

    valid_ = true;
}

ScopedEnvironment::~ScopedEnvironment() {
    for (auto it = priorEnvironment_.cbegin(); it != priorEnvironment_.cend(); ++it) {
        if (it.value().wasSet)
            qputenv(it.key().constData(), it.value().value);
        else
            qunsetenv(it.key().constData());
    }
    priorEnvironment_.clear();

    if (settingsRedirected_ && !realPreferenceSnapshot_.isEmpty() && !realPreferencesUnchanged()) {
        qWarning("ScopedEnvironment: the real user preference store at %s changed during this scope. "
                 "QSettings(organization, application) cannot be redirected on this platform.",
                 qPrintable(productionSettingsFilePath()));
    }
    if (settingsRedirected_) {
        scopeStack().removeAll(this);
        const ScopedEnvironment *enclosing = scopeStack().isEmpty() ? nullptr : scopeStack().constLast();
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           enclosing ? enclosing->settingsDir() : quarantineDir());
        QSettings::setDefaultFormat(priorSettingsFormat_);
        settingsRedirected_ = false;
    }
    // root_ (QTemporaryDir) removes the tree here unless keepArtifacts() ran.
}

bool ScopedEnvironment::isValid() const { return valid_; }
QString ScopedEnvironment::errorString() const { return error_; }

QString ScopedEnvironment::rootPath() const { return root_.path(); }
QString ScopedEnvironment::dataDir() const { return root_.filePath(QStringLiteral("data")); }
QString ScopedEnvironment::settingsDir() const { return root_.filePath(QStringLiteral("settings")); }
QString ScopedEnvironment::socketDir() const { return root_.filePath(QStringLiteral("s")); }
QString ScopedEnvironment::artifactDir() const { return root_.filePath(QStringLiteral("artifacts")); }

QString ScopedEnvironment::socketPath(const QString &name) const {
    const QString path = socketDir() + QLatin1Char('/') + name;
#ifndef Q_OS_WIN
    if (path.toUtf8().size() > kMaxSocketPathLength) {
        error_ = QStringLiteral("Socket path is %1 bytes, over the %2 byte platform limit: %3")
                     .arg(path.toUtf8().size())
                     .arg(kMaxSocketPathLength)
                     .arg(path);
        return {};
    }
#endif
    return path;
}

QString ScopedEnvironment::filePath(const QString &relative) const {
    const QString path = root_.filePath(relative);
    const QString parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent))
        error_ = QStringLiteral("Cannot create ") + parent;
    return path;
}

void ScopedEnvironment::captureEnvironment(const QByteArray &name) {
    if (priorEnvironment_.contains(name)) return;
    PriorValue prior;
    prior.wasSet = qEnvironmentVariableIsSet(name.constData());
    if (prior.wasSet) prior.value = qgetenv(name.constData());
    priorEnvironment_.insert(name, prior);
}

void ScopedEnvironment::setEnvironment(const QByteArray &name, const QByteArray &value) {
    captureEnvironment(name);
    qputenv(name.constData(), value);
}

void ScopedEnvironment::unsetEnvironment(const QByteArray &name) {
    captureEnvironment(name);
    qunsetenv(name.constData());
}

QProcessEnvironment ScopedEnvironment::processEnvironment() const {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (auto it = priorEnvironment_.cbegin(); it != priorEnvironment_.cend(); ++it) {
        const QString key = QString::fromLocal8Bit(it.key());
        if (qEnvironmentVariableIsSet(it.key().constData()))
            environment.insert(key, QString::fromLocal8Bit(qgetenv(it.key().constData())));
        else
            environment.remove(key);
    }
    return environment;
}

std::unique_ptr<QSettings> ScopedEnvironment::settings() const {
    return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope,
                                       QStringLiteral("clash-qt"), QStringLiteral("clash-qt"));
}

QString ScopedEnvironment::settingsFilePath() const {
    return settings()->fileName();
}

QString ScopedEnvironment::productionSettingsFilePath() const {
    return QSettings(QStringLiteral("clash-qt"), QStringLiteral("clash-qt")).fileName();
}

bool ScopedEnvironment::productionSettingsAreIsolated() const {
    return productionSettingsFilePath().startsWith(settingsDir());
}

bool ScopedEnvironment::realPreferencesUnchanged() const {
    if (productionSettingsAreIsolated()) return true;
    // Read-only on purpose. QSettings::sync() would flush the real store and
    // move its modification time, which is exactly what a fixture must never
    // do; the cost is that a write the OS preferences daemon has not yet
    // flushed can escape this check.
    return snapshotOf(productionSettingsFilePath()) == realPreferenceSnapshot_;
}

void ScopedEnvironment::keepArtifacts() { root_.setAutoRemove(false); }

} // namespace testsupport
