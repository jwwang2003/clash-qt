// The application's preference store, in exactly one place.
//
// Production code must never construct QSettings("clash-qt", "clash-qt")
// itself. On macOS that constructor is hard wired to NativeFormat - it ignores
// QSettings::setDefaultFormat() and QSettings::setPath() - so it always reaches
// the real CFPreferences domain com.clash-qt.clash-qt. A test process that used
// it wrote into the developer's own ~/Library/Preferences, which is the defect
// this module exists to make impossible.
//
// A leaf by design: this header and its implementation use Qt6::Core and
// nothing else in the project, so clash_config, clash_backups, the UI and the
// composition root can all read a setting without any of them depending on one
// another.
#pragma once

#include <QSettings>
#include <QString>

namespace core::preferences {

// The identity the native per-user store has always been keyed by. Changing
// either value would orphan every existing installation's preferences, so they
// are constants rather than parameters.
inline constexpr auto kOrganization = "clash-qt";
inline constexpr auto kApplication = "clash-qt";

// The environment variable that moves the whole application - profiles, chain,
// backups and now preferences - into one isolated directory. main.cpp sets it
// from --data-dir before anything reads it; core::ProfileStore keys off the
// same variable.
inline constexpr auto kDataDirVariable = "CLASH_QT_DATA_DIR";

// The store every production read and write goes through.
//
//   CLASH_QT_DATA_DIR unset - exactly QSettings(kOrganization, kApplication):
//       the native per-user store, same format, same scope, same location and
//       the same keys as before this module existed. An existing user sees no
//       change whatsoever.
//
//   CLASH_QT_DATA_DIR set   - an INI file at an explicit absolute path beneath
//       that directory. The native store is not opened, read or written.
//
// Returned by value: QSettings is neither copyable nor movable, so this relies
// on guaranteed copy elision - every return statement is a prvalue that
// initialises the caller's object directly.
QSettings open();

// The same resolution as open(), for an arbitrary organization/application.
// Exists so a test can exercise both directions against a scratch identity
// without reading or writing the real user store.
QSettings openAs(const QString &organization, const QString &application);

// Where open() resolves to. Absolute in both directions. Side effect free: it
// creates no directories and opens no store.
QString fileName();

// Where openAs() resolves to.
QString fileNameFor(const QString &organization, const QString &application);

// True when CLASH_QT_DATA_DIR redirects the store away from the native one.
bool isIsolated();

// The absolute directory open() is confined to while isIsolated(); empty
// otherwise. Everything the application persists lives beneath it.
QString isolatedRoot();

}  // namespace core::preferences
