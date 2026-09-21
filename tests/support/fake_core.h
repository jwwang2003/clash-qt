// Test-side handle for the compiled fake-core executable
// (tests/fixtures/fake_core_main.cpp).
//
// It copies the fixture binary into a directory the test owns, writes the
// behaviour script beside it, and exposes the gates and markers the child uses.
// Consumers hand binaryPath() to core::CoreProcess exactly as they would hand
// it a real mihomo path; nothing about the production code has to change.
#pragma once

#include <QString>
#include <QStringList>

namespace testsupport {

// What a caller may assert after terminating or crashing a fake core. These
// differ by OS and are stated rather than assumed; see fake_core_main.cpp.
struct TerminationContract {
    // QProcess::terminate() can be observed - and refused - by the child.
    // POSIX: true (SIGTERM). Windows: false (WM_CLOSE reaches no console app).
    bool terminateIsCooperative = false;
    // crashes() is reported as QProcess::CrashExit rather than NormalExit.
    bool crashReportsCrashExit = false;
    // Exit code of a crash when it is *not* reported as CrashExit.
    int crashExitCode = 0;
};

class FakeCore {
public:
    // Copies fixtureBinaryPath() into `directory` under `name`.
    explicit FakeCore(const QString &directory, const QString &name = QStringLiteral("fake-core"));
    // Same, from an explicit source binary.
    FakeCore(const QString &sourceBinary, const QString &directory, const QString &name);

    bool isValid() const;
    QString errorString() const;
    // Give this to core::CoreProcess::setBinaryPath().
    QString binaryPath() const;
    QString directory() const;

    // --- behaviour, chained; commit() must follow ---------------------------
    FakeCore &validationSucceeds(const QString &message = QString());
    FakeCore &validationFails(const QString &message, int exitCode = 1);
    // A `-t` run blocks until release(gate); lets a test observe a pending
    // validation instead of racing it.
    FakeCore &validationWaitsFor(const QString &gate);
    FakeCore &ignoresTerminate();

    // Ordered run directives, executed in call order.
    FakeCore &printsLine(const QString &line);
    FakeCore &touches(const QString &marker);
    FakeCore &waitsFor(const QString &gate);
    FakeCore &crashes();
    FakeCore &exitsWith(int code);
    FakeCore &runsForever();

    // Writes the script. Call before the process starts; calling it again
    // rewrites the script for the next start.
    bool commit();

    // --- gates and observation ----------------------------------------------
    bool release(const QString &gate);        // let a waitsFor()/validationWaitsFor() proceed
    bool reached(const QString &marker) const;  // did touches() run?
    // One entry per invocation, arguments joined by spaces - e.g.
    // "-t -d <dir> -f <config>" then "-d <dir> -f <config>".
    QStringList invocations() const;
    int invocationCount() const;

    // Path of the built fixture executable: $CLASH_QT_FAKE_CORE when the test
    // is registered with that property, otherwise a sibling of the test binary.
    static QString fixtureBinaryPath();
    static TerminationContract terminationContract();

private:
    void install(const QString &sourceBinary, const QString &directory, const QString &name);

    QString binaryPath_;
    QString directory_;
    mutable QString error_;
    bool valid_ = false;

    int validateExit_ = 0;
    QString validateMessage_;
    QString validateGate_;
    bool ignoreTerminate_ = false;
    QStringList run_;
};

} // namespace testsupport
