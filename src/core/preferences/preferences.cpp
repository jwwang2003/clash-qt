#include "core/preferences/preferences.h"

#include <QDir>
#include <QFileInfo>

namespace core::preferences {
namespace {

// Mirrors core::ProfileStore::dataDir()'s reading of the same variable, so the
// preference file lands beside the profile data rather than in a second place.
QString isolatedRootOrEmpty() {
    const QString custom = qEnvironmentVariable(kDataDirVariable);
    if (custom.isEmpty()) return {};
    return QFileInfo(custom).absoluteFilePath();
}

// The layout Qt itself uses for IniFormat/UserScope - <root>/<org>/<app>.ini -
// so an isolated store is recognisable, and so two identities inside one data
// directory cannot collide. "settings" separates it from the profile data that
// BackupStore walks; its `roots` list is fixed, so this file is never swept
// into a backup as a file *and* as settings.
QString isolatedFileName(const QString &root, const QString &organization,
                         const QString &application) {
    return root + QStringLiteral("/settings/") + organization + QLatin1Char('/') +
           application + QStringLiteral(".ini");
}

}  // namespace

bool isIsolated() { return !isolatedRootOrEmpty().isEmpty(); }

QString isolatedRoot() { return isolatedRootOrEmpty(); }

QString fileNameFor(const QString &organization, const QString &application) {
    const QString root = isolatedRootOrEmpty();
    if (root.isEmpty())
        return QSettings(organization, application).fileName();
    return isolatedFileName(root, organization, application);
}

QString fileName() {
    return fileNameFor(QString::fromLatin1(kOrganization), QString::fromLatin1(kApplication));
}

QSettings openAs(const QString &organization, const QString &application) {
    const QString root = isolatedRootOrEmpty();
    if (root.isEmpty()) {
        // Byte for byte the construction every call site used before this
        // module existed. Nothing is interposed.
        return QSettings(organization, application);
    }
    const QString path = isolatedFileName(root, organization, application);
    // QSettings creates the file lazily; create the directory eagerly so a
    // first write cannot fail on a missing parent.
    QDir().mkpath(QFileInfo(path).absolutePath());
    return QSettings(path, QSettings::IniFormat);
}

QSettings open() {
    return openAs(QString::fromLatin1(kOrganization), QString::fromLatin1(kApplication));
}

}  // namespace core::preferences
