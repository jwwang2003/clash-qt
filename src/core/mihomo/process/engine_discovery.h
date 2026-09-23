#pragma once

// Engine resolution. The MANAGED engine is the one this application built and
// staged; it is never PATH, never another Clash installation and never a
// download. Anything else is an explicit, separately labelled user choice, and
// whatever is resolved is reported so provenance can never be implied.
//
// Before this existed, CoreProcess::discoverBinary() fell through to
// QStandardPaths::findExecutable("mihomo") and then to a hardcoded
// "/Applications/Clash Verge.app/Contents/MacOS/verge-mihomo". On a machine with
// Clash Verge installed - this developer's - an application with no staged
// engine silently supervised Clash Verge's binary while reporting its own
// provenance.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>

namespace core {

enum class EngineSource : quint8 {
    None = 0,
    /// Staged beside the application executable by `make package`. The managed
    /// engine, and the only source resolveManagedEngine() prefers.
    Staged = 1,
    /// $CLASH_QT_CORE_BINARY: the locally built engine `make run` points at.
    /// Still this project's own build output, so still a managed engine.
    LocalBuild = 2,
    /// A path the user set explicitly.
    UserChosen = 3,
    /// Another Clash installation on this host. OFFERED, never resolved.
    ExternalInstall = 4,
    /// An executable named "mihomo" on PATH. OFFERED, never resolved.
    SearchPath = 5,
};

/// What was resolved, always reportable. `path` is empty exactly when nothing
/// usable was found, and `problem` then says what to do about it.
struct EngineResolution {
    QString path;
    EngineSource source = EngineSource::None;
    QString label;       // short and user-facing
    QString provenance;  // one line from mihomo-provenance.json, or empty
    QString problem;     // actionable; non-empty only when path is empty
    bool isResolved() const { return !path.isEmpty(); }
};

struct EngineCandidate {
    QString path;
    EngineSource source = EngineSource::None;
    QString label;
};

// Header-only, and every function is inline. It is reached from
// core_process.cpp, which lives in clash_mihomo_impl; giving it a translation
// unit of its own would mean a new source registration, and this is a leaf of
// pure functions over the filesystem and the environment - nothing to link.

inline QString engineSourceLabel(EngineSource source);
inline QString engineProvenance(const QString &binary);

namespace engine_detail {

inline QString engineFileName() {
#ifdef Q_OS_WIN
    return QStringLiteral("mihomo.exe");
#else
    return QStringLiteral("mihomo");
#endif
}

inline bool isUsable(const QString &path) {
    if (path.isEmpty()) return false;
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable();
}

inline QString absolute(const QString &path) { return QFileInfo(path).absoluteFilePath(); }

/// Where the build stages the engine: beside the application executable.
/// `make package` installs it at clash-qt.app/Contents/MacOS/mihomo.
inline QString stagedEnginePath() {
    const QString directory = QCoreApplication::applicationDirPath();
    if (directory.isEmpty()) return {};
    return directory + QLatin1Char('/') + engineFileName();
}

/// Where the build stages mihomo-provenance.json: beside the binary, and - in a
/// macOS bundle - in Contents/Resources, because a bundle's MacOS directory is
/// for executables.
inline QStringList provenanceCandidates(const QString &binary) {
    const QString directory = QFileInfo(binary).absolutePath();
    QStringList paths{directory + QStringLiteral("/mihomo-provenance.json")};
    paths << QDir::cleanPath(directory + QStringLiteral("/../Resources/mihomo-provenance.json"));
    return paths;
}

inline QStringList otherClashInstallations() {
#ifdef Q_OS_MACOS
    return {
        QStringLiteral("/Applications/Clash Verge.app/Contents/MacOS/verge-mihomo"),
        QStringLiteral("/Applications/Clash Verge.app/Contents/MacOS/verge-mihomo-alpha"),
    };
#elif defined(Q_OS_WIN)
    QStringList paths;
    for (const char *variable : {"LOCALAPPDATA", "ProgramFiles", "ProgramFiles(x86)"}) {
        const QString root = qEnvironmentVariable(variable);
        if (root.isEmpty()) continue;
        paths << root + QStringLiteral("/Programs/Clash Verge/verge-mihomo.exe")
              << root + QStringLiteral("/Programs/Clash Verge/verge-mihomo-alpha.exe")
              << root + QStringLiteral("/Clash Verge/verge-mihomo.exe")
              << root + QStringLiteral("/Clash Verge/verge-mihomo-alpha.exe");
    }
    return paths;
#else
    return {
        QStringLiteral("/usr/lib/clash-verge-rev/verge-mihomo"),
        QStringLiteral("/usr/lib/clash-verge-rev/verge-mihomo-alpha"),
        QStringLiteral("/opt/clash-verge-rev/verge-mihomo"),
        QStringLiteral("/opt/clash-verge-rev/verge-mihomo-alpha"),
    };
#endif
}

inline QString unresolvedMessage() {
    return QCoreApplication::translate(
        "core::EngineDiscovery",
        "No managed mihomo engine is staged with this application.\n"
        "Build it from the recorded source with \"make core\" (packaging stages it "
        "beside the application executable), or set CLASH_QT_CORE_BINARY to a local "
        "build.\n"
        "clash-qt will not silently use PATH or another Clash installation: an engine "
        "it did not build is an explicit choice you make in Settings, and it is "
        "reported as such.");
}

inline EngineResolution resolved(const QString &path, EngineSource source) {
    EngineResolution resolution;
    resolution.path = absolute(path);
    resolution.source = source;
    resolution.label = engineSourceLabel(source);
    resolution.provenance = engineProvenance(resolution.path);
    return resolution;
}

}  // namespace engine_detail

inline QString engineSourceLabel(EngineSource source) {
    switch (source) {
        case EngineSource::Staged:
            return QCoreApplication::translate("core::EngineDiscovery",
                                               "Staged engine (built from local source)");
        case EngineSource::LocalBuild:
            return QCoreApplication::translate("core::EngineDiscovery",
                                               "Local build (CLASH_QT_CORE_BINARY)");
        case EngineSource::UserChosen:
            return QCoreApplication::translate("core::EngineDiscovery",
                                               "Engine you selected");
        case EngineSource::ExternalInstall:
            return QCoreApplication::translate("core::EngineDiscovery",
                                               "Another Clash installation on this computer");
        case EngineSource::SearchPath:
            return QCoreApplication::translate("core::EngineDiscovery",
                                               "An executable named mihomo on PATH");
        case EngineSource::None:
            break;
    }
    return QCoreApplication::translate("core::EngineDiscovery", "No engine");
}

inline QString engineProvenance(const QString &binary) {
    if (binary.isEmpty()) return {};
    for (const QString &candidate : engine_detail::provenanceCandidates(binary)) {
        QFile file(candidate);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument document = QJsonDocument::fromJson(file.read(256 * 1024));
        if (!document.isObject()) continue;
        const QJsonObject manifest = document.object();
        const QString recorded = manifest.value(QStringLiteral("artifact_sha256")).toString();
        if (recorded.isEmpty()) continue;

        // The manifest describes ONE artifact. Matching it by content is what
        // stops a stale manifest lending its provenance to a different binary.
        QFile image(binary);
        if (!image.open(QIODevice::ReadOnly)) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&image)) continue;
        const QString actual = QString::fromLatin1(hash.result().toHex());
        if (actual.compare(recorded, Qt::CaseInsensitive) != 0) {
            return QCoreApplication::translate(
                "core::EngineDiscovery",
                "no recorded provenance (sha256 %1 does not match the manifest beside it)")
                .arg(actual.left(12));
        }
        const QString describe = manifest.value(QStringLiteral("source_describe")).toString();
        const QString commit = manifest.value(QStringLiteral("source_commit")).toString();
        const QString state = manifest.value(QStringLiteral("source_state")).toString();
        const QString toolchain = manifest.value(QStringLiteral("go_toolchain")).toString();
        return QCoreApplication::translate("core::EngineDiscovery",
                                           "%1 from %2 (%3), %4, sha256 %5")
            .arg(describe.isEmpty() ? QStringLiteral("unknown version") : describe,
                 commit.left(12).isEmpty() ? QStringLiteral("unknown commit") : commit.left(12),
                 state.isEmpty() ? QStringLiteral("unknown state") : state,
                 toolchain.isEmpty() ? QStringLiteral("unknown toolchain") : toolchain,
                 actual.left(12));
    }
    return QCoreApplication::translate("core::EngineDiscovery", "no recorded provenance");
}

inline EngineResolution resolveManagedEngine() {
    if (const QString staged = engine_detail::stagedEnginePath(); engine_detail::isUsable(staged))
        return engine_detail::resolved(staged, EngineSource::Staged);

    // The development equivalent of the staged engine: `make run` exports it and
    // it is this project's own build output, not a foreign install.
    if (const QString local = qEnvironmentVariable("CLASH_QT_CORE_BINARY"); engine_detail::isUsable(local))
        return engine_detail::resolved(local, EngineSource::LocalBuild);

    EngineResolution resolution;
    resolution.label = engineSourceLabel(EngineSource::None);
    resolution.problem = engine_detail::unresolvedMessage();
    return resolution;
}

inline EngineResolution describeChosenEngine(const QString &path) {
    if (path.isEmpty()) return resolveManagedEngine();
    if (!engine_detail::isUsable(path)) {
        EngineResolution resolution;
        resolution.label = engineSourceLabel(EngineSource::None);
        resolution.problem =
            QCoreApplication::translate("core::EngineDiscovery",
                                        "The selected engine %1 is not an executable file.")
                .arg(path);
        return resolution;
    }
    // An explicitly chosen engine keeps its own label even when it happens to be
    // the staged one: what the user picked is what is reported.
    EngineResolution resolution = engine_detail::resolved(path, EngineSource::UserChosen);
    const QString staged = engine_detail::stagedEnginePath();
    if (!staged.isEmpty() && engine_detail::absolute(staged) == resolution.path) {
        resolution.source = EngineSource::Staged;
        resolution.label = engineSourceLabel(EngineSource::Staged);
    }
    return resolution;
}

inline QVector<EngineCandidate> externalEngineCandidates() {
    QVector<EngineCandidate> candidates;
    QStringList seen;
    const auto offer = [&](const QString &path, EngineSource source) {
        if (!engine_detail::isUsable(path)) return;
        const QString full = engine_detail::absolute(path);
        if (seen.contains(full)) return;
        seen.append(full);
        candidates.append(EngineCandidate{full, source, engineSourceLabel(source)});
    };

    offer(QStandardPaths::findExecutable(QStringLiteral("mihomo")), EngineSource::SearchPath);
    for (const QString &path : engine_detail::otherClashInstallations()) offer(path, EngineSource::ExternalInstall);
    return candidates;
}

}  // namespace core
