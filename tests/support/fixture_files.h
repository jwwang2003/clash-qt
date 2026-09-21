// Fixture file I/O shared by the four suites that `tests/core/runtime_test.cpp`
// was split into (config-generation, profile-store, core-process,
// runtime-maintenance). All four write a YAML fixture to disk and read a
// generated artefact back byte-for-byte, so the pair lives here rather than
// being copied four times.
//
// Header-only on purpose: it adds nothing to link, and it is deliberately two
// functions rather than a fixture base class. Anything that only one partition
// needs stays private to that partition.
#pragma once

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QtTest>

namespace testsupport {

// Writes `body` to `path`, failing the calling test function's *helper frame*
// if the file cannot be opened or is short-written. Carried over verbatim from
// the original suite, including that a QVERIFY failure here returns from this
// helper rather than from the test function.
inline void writeFile(const QString &path, const QByteArray &body) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(body), body.size());
}

// Whole-file contents, or an empty array when the file cannot be read. Callers
// compare the result against a previously captured copy to prove a failed
// operation left the file untouched, so "unreadable" and "empty" are
// deliberately not distinguished.
inline QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

} // namespace testsupport
