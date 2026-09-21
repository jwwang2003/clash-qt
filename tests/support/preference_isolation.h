// Preference isolation shared by the suites that `tests/ui/data_pages_test.cpp`
// was split into (seven ui/pages suites plus the two benchmark suites under
// tests/benchmarks/).
//
// Every one of them constructs real production widgets, and ProxiesPage --
// among others -- persists settings through core::preferences. QSettings
// redirection is process-global, so each split executable must perform the same
// isolation in its own initTestCase(). These two functions carry the checks the
// original suite made inline, so the nine copies cannot drift apart.
//
// They return a message instead of asserting: QVERIFY2 inside a helper would
// return from the helper, not from the calling test, and would hide the failure.
#pragma once

#include <QFileInfo>
#include <QLatin1Char>
#include <QString>

#include "core/preferences/preferences.h"
#include "support/scoped_environment.h"

namespace testsupport {

// Empty when `environment` was created successfully *and* core::preferences
// really resolved into it. A non-empty result is the message to report.
inline QString preferenceIsolationFailure(const ScopedEnvironment &environment) {
    if (!environment.isValid()) return environment.errorString();
    if (!core::preferences::isIsolated())
        return QStringLiteral("core::preferences reports that it is not isolated");
    const QString root = QFileInfo(environment.dataDir()).absoluteFilePath() + QLatin1Char('/');
    if (!core::preferences::fileName().startsWith(root))
        return QStringLiteral("core::preferences resolved to %1, outside %2")
            .arg(core::preferences::fileName(), root);
    return {};
}

// Empty when the developer's real preference store is byte-identical to what it
// was when `environment` was constructed. Called from cleanupTestCase().
inline QString preferenceEscapeFailure(const ScopedEnvironment &environment) {
    if (environment.realPreferencesUnchanged()) return {};
    return QStringLiteral("The real user preference store at %1 changed during this run")
        .arg(environment.productionSettingsFilePath());
}

} // namespace testsupport
