#include "support/fake_core.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace testsupport {
namespace {

QString executableName(const QString &name) {
#ifdef Q_OS_WIN
    return name.endsWith(QStringLiteral(".exe")) ? name : name + QStringLiteral(".exe");
#else
    return name;
#endif
}

QString scriptFor(const QString &binaryPath) {
#ifdef Q_OS_WIN
    if (binaryPath.endsWith(QStringLiteral(".exe")))
        return binaryPath.chopped(4) + QStringLiteral(".script");
#endif
    return binaryPath + QStringLiteral(".script");
}

} // namespace

QString FakeCore::fixtureBinaryPath() {
    const QString fromEnvironment = qEnvironmentVariable("CLASH_QT_FAKE_CORE");
    if (!fromEnvironment.isEmpty()) return fromEnvironment;
    const QString sibling = QCoreApplication::applicationDirPath() +
                            QStringLiteral("/") + executableName(QStringLiteral("clash-qt-fake-core"));
    return sibling;
}

TerminationContract FakeCore::terminationContract() {
    TerminationContract contract;
#ifdef Q_OS_WIN
    contract.terminateIsCooperative = false;
    contract.crashReportsCrashExit = false;
    contract.crashExitCode = 3;  // abort() under the UCRT
#else
    contract.terminateIsCooperative = true;
    contract.crashReportsCrashExit = true;  // SIGABRT
    contract.crashExitCode = 0;
#endif
    return contract;
}

FakeCore::FakeCore(const QString &directory, const QString &name) {
    install(fixtureBinaryPath(), directory, name);
}

FakeCore::FakeCore(const QString &sourceBinary, const QString &directory, const QString &name) {
    install(sourceBinary, directory, name);
}

void FakeCore::install(const QString &sourceBinary, const QString &directory, const QString &name) {
    directory_ = directory;
    if (!QDir().mkpath(directory)) {
        error_ = QStringLiteral("Cannot create ") + directory;
        return;
    }
    if (!QFileInfo::exists(sourceBinary)) {
        error_ = QStringLiteral("Fake-core fixture binary not found at '%1'. Register the test with "
                                "ENVIRONMENT \"CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>\".")
                     .arg(sourceBinary);
        return;
    }
    binaryPath_ = directory + QStringLiteral("/") + executableName(name);
    QFile::remove(binaryPath_);
    if (!QFile::copy(sourceBinary, binaryPath_)) {
        error_ = QStringLiteral("Cannot copy %1 to %2").arg(sourceBinary, binaryPath_);
        return;
    }
    if (!QFile::setPermissions(binaryPath_, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                                QFile::ReadUser | QFile::ExeUser)) {
        error_ = QStringLiteral("Cannot make %1 executable").arg(binaryPath_);
        return;
    }
    QFile::remove(binaryPath_ + QStringLiteral(".args"));
    valid_ = true;
}

bool FakeCore::isValid() const { return valid_; }
QString FakeCore::errorString() const { return error_; }
QString FakeCore::binaryPath() const { return binaryPath_; }
QString FakeCore::directory() const { return directory_; }

FakeCore &FakeCore::validationSucceeds(const QString &message) {
    validateExit_ = 0;
    validateMessage_ = message;
    return *this;
}

FakeCore &FakeCore::validationFails(const QString &message, int exitCode) {
    validateExit_ = exitCode == 0 ? 1 : exitCode;
    validateMessage_ = message;
    return *this;
}

FakeCore &FakeCore::validationWaitsFor(const QString &gate) {
    validateGate_ = gate;
    return *this;
}

FakeCore &FakeCore::ignoresTerminate() {
    ignoreTerminate_ = true;
    return *this;
}

FakeCore &FakeCore::printsLine(const QString &line) {
    run_.append(QStringLiteral("stdout=") + line);
    return *this;
}

FakeCore &FakeCore::touches(const QString &marker) {
    run_.append(QStringLiteral("touch=") + marker);
    return *this;
}

FakeCore &FakeCore::waitsFor(const QString &gate) {
    run_.append(QStringLiteral("wait=") + gate);
    return *this;
}

FakeCore &FakeCore::crashes() {
    run_.append(QStringLiteral("crash"));
    return *this;
}

FakeCore &FakeCore::exitsWith(int code) {
    run_.append(QStringLiteral("exit=") + QString::number(code));
    return *this;
}

FakeCore &FakeCore::runsForever() {
    run_.append(QStringLiteral("run-forever"));
    return *this;
}

bool FakeCore::commit() {
    if (!valid_) return false;
    QStringList lines;
    lines.append(QStringLiteral("validate-exit=") + QString::number(validateExit_));
    if (!validateMessage_.isEmpty()) lines.append(QStringLiteral("validate-message=") + validateMessage_);
    if (!validateGate_.isEmpty()) lines.append(QStringLiteral("validate-wait=") + validateGate_);
    if (ignoreTerminate_) lines.append(QStringLiteral("ignore-terminate"));
    lines += run_;

    QFile script(scriptFor(binaryPath_));
    if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        error_ = QStringLiteral("Cannot write ") + script.fileName();
        return false;
    }
    QTextStream out(&script);
    for (const QString &line : std::as_const(lines)) out << line << "\n";
    out.flush();
    return script.flush();
}

bool FakeCore::release(const QString &gate) {
    QFile marker(directory_ + QStringLiteral("/") + gate);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error_ = QStringLiteral("Cannot release gate ") + gate;
        return false;
    }
    marker.write("1");
    return marker.flush();
}

bool FakeCore::reached(const QString &marker) const {
    return QFileInfo::exists(directory_ + QStringLiteral("/") + marker);
}

QStringList FakeCore::invocations() const {
    QFile log(binaryPath_ + QStringLiteral(".args"));
    if (!log.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList entries;
    QTextStream in(&log);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (!line.isEmpty()) entries.append(line);
    }
    return entries;
}

int FakeCore::invocationCount() const { return static_cast<int>(invocations().size()); }

} // namespace testsupport
