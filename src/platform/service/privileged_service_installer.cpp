#include "platform/service/privileged_service_installer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrentRun>

#ifdef Q_OS_MACOS
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#endif

namespace platform {
namespace {
struct Result { bool installed = false; bool success = false; QString error; };
constexpr auto kInstalledHelper = "/Library/PrivilegedHelperTools/org.clash-qt.service/helper";
constexpr auto kInstalledCore = "/Library/PrivilegedHelperTools/org.clash-qt.service/mihomo";
constexpr auto kInstalledPlist = "/Library/LaunchDaemons/org.clash-qt.service.plist";

Result inspectInstallation() {
    Result result;
#ifdef Q_OS_MACOS
    struct stat helper{}, core{}, plist{}, directory{};
    const bool safeDirectory = lstat("/Library/PrivilegedHelperTools/org.clash-qt.service", &directory) == 0 &&
        S_ISDIR(directory.st_mode) && directory.st_uid == 0 && !(directory.st_mode & (S_IWGRP | S_IWOTH));
    const bool hasHelper = lstat(kInstalledHelper, &helper) == 0;
    const bool hasPlist = lstat(kInstalledPlist, &plist) == 0;
    const bool hasCore = lstat(kInstalledCore, &core) == 0;
    result.installed = safeDirectory && hasHelper && hasCore && hasPlist && S_ISREG(helper.st_mode) && S_ISREG(core.st_mode) && S_ISREG(plist.st_mode) &&
        helper.st_uid == 0 && core.st_uid == 0 && plist.st_uid == 0 &&
        (helper.st_mode & S_IXUSR) && (core.st_mode & S_IXUSR) &&
        !(helper.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) &&
        !(core.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) &&
        !(plist.st_mode & (S_IWGRP | S_IWOTH));
    result.success = true;
    if ((hasHelper || hasCore || hasPlist) && !result.installed)
        result.error = QObject::tr("The privileged service installation is incomplete or has unsafe permissions. Repair it before use.");
#else
    result.error = QObject::tr("The privileged core service is supported only on macOS.");
#endif
    return result;
}

#ifdef Q_OS_MACOS
QString authorizationError(OSStatus status) {
    if (status == errAuthorizationCanceled) return QObject::tr("Administrator authorization was cancelled.");
    if (status == errAuthorizationDenied) return QObject::tr("Administrator authorization was denied.");
    return QObject::tr("Administrator authorization failed (code %1).").arg(status);
}

Result installAuthorized(bool install, const QString &corePath, const QString &helperPath, uid_t uid) {
    Result result;
    struct stat helper{};
    const QByteArray helperBytes = QFile::encodeName(helperPath);
    if (lstat(helperBytes.constData(), &helper) != 0 || !S_ISREG(helper.st_mode) ||
        !(helper.st_mode & S_IXUSR) || (helper.st_uid != uid && helper.st_uid != 0) ||
        (helper.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID))) {
        result.error = QObject::tr("The bundled privileged helper is missing or has unsafe permissions. Reinstall the app.");
        return result;
    }
    if (install) {
        struct stat core{};
        const QByteArray coreBytes = QFile::encodeName(corePath);
        if (!QFileInfo(corePath).isAbsolute() || lstat(coreBytes.constData(), &core) != 0 ||
            !S_ISREG(core.st_mode) || !(core.st_mode & S_IXUSR) ||
            (core.st_uid != uid && core.st_uid != 0) || (core.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID))) {
            result.error = QObject::tr("Choose a regular executable core owned by you or root, without group/world write or set-ID permissions.");
            return result;
        }
    }

    AuthorizationRef authorization = nullptr;
    OSStatus status = AuthorizationCreate(nullptr, kAuthorizationEmptyEnvironment,
                                         kAuthorizationFlagDefaults, &authorization);
    if (status != errAuthorizationSuccess) { result.error = authorizationError(status); return result; }
    const QByteArray prompt = install
        ? QByteArray("Install the clash-qt privileged core service for TUN networking.")
        : QByteArray("Remove the clash-qt privileged core service.");
    AuthorizationItem promptItem{kAuthorizationEnvironmentPrompt, static_cast<UInt32>(prompt.size()),
                                 const_cast<char *>(prompt.constData()), 0};
    AuthorizationEnvironment environment{1, &promptItem};
    AuthorizationItem execute{kAuthorizationRightExecute, static_cast<UInt32>(helperBytes.size()),
                              const_cast<char *>(helperBytes.constData()), 0};
    AuthorizationRights rights{1, &execute};
    status = AuthorizationCopyRights(authorization, &rights, &environment,
        kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights, nullptr);
    if (status != errAuthorizationSuccess) {
        AuthorizationFree(authorization, kAuthorizationFlagDestroyRights);
        result.error = authorizationError(status);
        return result;
    }
    QList<QByteArray> argumentStorage{install ? QByteArray("--install") : QByteArray("--uninstall")};
    if (install) argumentStorage << QByteArray::number(uid) << QFile::encodeName(corePath);
    QList<char *> arguments;
    for (auto &argument : argumentStorage) arguments.append(argument.data());
    arguments.append(nullptr);
    FILE *communications = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    status = AuthorizationExecuteWithPrivileges(authorization, helperBytes.constData(),
                                                kAuthorizationFlagDefaults, arguments.data(), &communications);
#pragma clang diagnostic pop
    if (status != errAuthorizationSuccess || !communications) {
        AuthorizationFree(authorization, kAuthorizationFlagDestroyRights);
        result.error = authorizationError(status);
        return result;
    }

    QByteArray output;
    QElapsedTimer deadline;
    deadline.start();
    bool ended = false;
    const int descriptor = fileno(communications);
    while (deadline.elapsed() < 120000 && output.size() <= 1024 * 1024) {
        pollfd pipe{descriptor, POLLIN | POLLHUP, 0};
        const int ready = ::poll(&pipe, 1, 250);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (ready == 0) continue;
        if (pipe.revents & (POLLERR | POLLNVAL)) break;
        char bytes[4096];
        const ssize_t count = ::read(descriptor, bytes, sizeof(bytes));
        if (count == 0) { ended = true; break; }
        if (count < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
        output.append(bytes, count);
    }
    fclose(communications);
    AuthorizationFree(authorization, kAuthorizationFlagDestroyRights);
    if (!ended) {
        result.error = QObject::tr("The privileged installer did not finish within its output/time limit. Check service status before retrying; installation may have completed.");
        result.installed = inspectInstallation().installed;
        return result;
    }
    constexpr auto marker = "CLASH_QT_SERVICE_RESULT ";
    for (const auto &line : output.split('\n')) {
        if (!line.startsWith(marker)) continue;
        const auto report = QJsonDocument::fromJson(line.mid(qstrlen(marker))).object();
        if (!report.value("ok").isBool()) continue;
        result.success = report.value("ok").toBool();
        result.error = report.value("error").toString();
    }
    const Result state = inspectInstallation();
    result.installed = state.installed;
    if (result.success && result.installed != install) {
        result.success = false;
        result.error = QObject::tr("The privileged installer finished, but the installed files do not match the requested state.");
    }
    if (!result.success && result.error.isEmpty())
        result.error = QObject::tr("The privileged installer did not confirm completion.");
    return result;
}
#endif
} // namespace

PrivilegedServiceInstaller::PrivilegedServiceInstaller(QObject *parent) : QObject(parent) {}
bool PrivilegedServiceInstaller::isSupported() {
#ifdef Q_OS_MACOS
    return true;
#else
    return false;
#endif
}
QString PrivilegedServiceInstaller::bundledHelperPath() {
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../Helpers/clash-qt-service-helper");
}
void PrivilegedServiceInstaller::refresh() {
    if (busy_) return;
    const quint64 epoch = ++refreshEpoch_;
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, epoch] {
        const Result result = watcher->result();
        watcher->deleteLater();
        if (epoch != refreshEpoch_ || busy_) return;
        installed_ = result.installed;
        emit statusChanged(installed_, result.error);
    });
    watcher->setFuture(QtConcurrent::run(inspectInstallation));
}
void PrivilegedServiceInstaller::install(const QString &corePath) { run(true, corePath); }
void PrivilegedServiceInstaller::uninstall() { run(false, {}); }
void PrivilegedServiceInstaller::run(bool install, const QString &corePath) {
    if (busy_) return;
    if (!isSupported()) {
        emit finished(false, false, tr("The privileged core service is supported only on macOS."));
        return;
    }
    ++refreshEpoch_;
    busy_ = true;
    emit busyChanged(true);
    const QString helper = bundledHelperPath();
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher] {
        const Result result = watcher->result();
        watcher->deleteLater();
        installed_ = result.installed;
        busy_ = false;
        emit busyChanged(false);
        emit statusChanged(installed_, result.error);
        emit finished(installed_, result.success, result.error);
    });
#ifdef Q_OS_MACOS
    const uid_t uid = getuid();
    watcher->setFuture(QtConcurrent::run([install, corePath, helper, uid] {
        Result result = installAuthorized(install, corePath, helper, uid);
        // Cancellation/failure must not erase a previously installed status.
        result.installed = inspectInstallation().installed;
        return result;
    }));
#else
    Q_UNUSED(install);
    Q_UNUSED(corePath);
    Q_UNUSED(helper);
#endif
}
} // namespace platform
