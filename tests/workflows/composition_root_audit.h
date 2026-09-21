// The composition-root audit.
//
// WHY THIS EXISTS. wf::AssembledApp rebuilds src/main.cpp's object graph in
// parallel. That is what lets a journey run without widgets, a root-owned
// helper or the developer's proxy settings - and it is also a hole: before this
// file, deleting a connection from src/main.cpp changed nothing that any
// workflow suite could see. The audit that found the hole deleted the
// `warningRaised -> QMessageBox` connection - the one src/main.cpp itself
// documents as the only thing standing between an unacknowledged warning and a
// quit that hangs forever - and all five suites, app-smoke included, still
// passed.
//
// WHAT THIS ASSERTS. That the duplicate is still a duplicate. Both files are
// read from the source tree and their wiring is extracted and normalised:
//
//   * every signal src/main.cpp connects must be connected in AssembledApp too,
//     to the same receiver and slot - unless it is named below as an edge whose
//     receiver is a widget or the QApplication, which a headless harness cannot
//     have;
//   * every signal AssembledApp connects must be connected in src/main.cpp, so
//     the harness cannot invent behaviour the application does not have;
//   * every busy gate, quit action, final cleanup and quit-guard registration
//     must appear in both, by name;
//   * every SHELL-ONLY statement listed below must still be present in
//     src/main.cpp, with the user-visible consequence of its loss written down
//     beside it.
//
// WHAT THIS IS NOT. It is not a behavioural test, and it does not pretend to
// be. Where a connection can be driven through the shipping binary from outside
// its process, the behavioural case that drives it is named in `covered`
// below - and those cases are the real proof. This file is the net under
// everything that cannot be reached that way, which today is every connection
// on the quit path: there is no supported way to ask the shipped application to
// quit from outside it, and adding one would be exactly the test-control API
// docs/TEST_STRATEGY.md forbids. See the worker report for the production
// change that would replace this file with a behavioural check: a composition
// function in src/app/composition that both main() and this directory call.
//
// A FAILURE HERE IS NOT NECESSARILY A BUG IN src/main.cpp. It means the two
// files disagree. Whoever changed one of them decides which is right, then
// updates the other and the inventory below.
#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace workflows::audit {

// ---------------------------------------------------------------- the model

/// One `QObject::connect(...)`, normalised: `&backups` and `backups.get()` are
/// the same object, `&lifecycle::QuitGuard::quitRequested` and
/// `&app::lifecycle::QuitGuard::quitRequested` are the same signal, and any
/// lambda is just "lambda".
struct Edge {
    QString sender;
    QString signalName;  // Class::member
    QString receiver;
    QString slot;  // Class::member, or "lambda"
    int line = 0;

    QString key() const {
        return sender + QStringLiteral("::") + signalName + QStringLiteral(" -> ") + receiver +
               QStringLiteral("::") + slot;
    }
    QString signalKey() const { return sender + QStringLiteral("::") + signalName; }
    QString describe(const QString &file) const {
        return QStringLiteral("%1:%2  %3").arg(file).arg(line).arg(key());
    }
};

/// One registration that is not a connect: a busy gate, a quit action, the
/// final cleanup, the quit guard, an event filter, or a startup call.
struct Statement {
    QString object;
    QString call;
    QString argument;  // the first argument, unwrapped from QStringLiteral(...)
    int line = 0;

    QString key() const {
        return object + QStringLiteral(".") + call + QStringLiteral("(") + argument +
               QStringLiteral(")");
    }
    QString describe(const QString &file) const {
        return QStringLiteral("%1:%2  %3").arg(file).arg(line).arg(key());
    }
};

struct Wiring {
    QVector<Edge> edges;
    QVector<Statement> statements;

    bool hasEdgeKey(const QString &key) const {
        for (const Edge &edge : edges)
            if (edge.key() == key) return true;
        return false;
    }
    bool hasSignal(const QString &signalKey) const {
        for (const Edge &edge : edges)
            if (edge.signalKey() == signalKey) return true;
        return false;
    }
    bool hasStatement(const QString &key) const {
        for (const Statement &statement : statements)
            if (statement.key() == key) return true;
        return false;
    }
};

// ------------------------------------------------------------- the inventory

/// A signal src/main.cpp routes into a widget or into the QApplication, which a
/// headless harness cannot have. The harness must still connect the SAME signal
/// from the SAME object - it records instead of rendering - so the journeys keep
/// observing what the shell would have shown.
struct MirroredBySignal {
    const char *signalKey;
    const char *shell;    // what src/main.cpp does with it
    const char *harness;  // what AssembledApp does with it instead
};

inline QVector<MirroredBySignal> mirroredBySignal() {
    return {
        {"profiles::ProfileStore::errorOccurred",
         "collected into startupErrors during the settings bootstrap, then shown in a modal",
         "collected into AssembledApp::startupErrors and AssembledApp::storeErrors"},
        {"enhancer::ConfigEnhancer::errorOccurred",
         "collected into startupErrors during the settings bootstrap, then shown in a modal",
         "collected into AssembledApp::startupErrors and AssembledApp::storeErrors"},
        {"shutdown::ShutdownCoordinator::shutdownStarted",
         "disables the window and the tray menu and shows the reason in the status bar",
         "records AssembledApp::shutdownStarted"},
        {"shutdown::ShutdownCoordinator::warningRaised",
         "opens a modal QMessageBox that holds the quit open until it is acknowledged",
         "records AssembledApp::shutdownWarnings; the journey dismisses explicitly"},
        {"routing::RoutingController::errorOccurred",
         "shows the message in the status bar for eight seconds",
         "records AssembledApp::routingErrors"},
    };
}

/// A connection that exists only in the composition root. `consequence` is what
/// a user loses when it is deleted; `covered` names the case that would notice,
/// or says plainly that nothing but this audit would.
struct ShellOnly {
    const char *key;
    const char *consequence;
    const char *covered;
};

inline QVector<ShellOnly> shellOnlyEdges() {
    return {
        {"shutdown::ShutdownCoordinator::quitApproved -> app::QApplication::quit",
         "the shutdown completes and the application never exits",
         "structural only: the shipped binary offers no way to request a quit from outside "
         "its own process, and adding one is the test-control API the strategy forbids"},
        {"dialog::QMessageBox::finished -> shutdown::lambda",
         "the user acknowledges the warning and the quit still never completes",
         "structural only: same reason - the quit path is unreachable from outside"},
        {"instance::QLocalServer::newConnection -> window::lambda",
         "launching the application a second time on the same data directory does nothing: "
         "the running window is never raised and the request is never read",
         "app-smoke: theRunningInstanceAnswersItsSingleInstanceChannel"},
        {"bridge::BackendBridge::trafficSample -> tray::TrayIcon::setTraffic",
         "the tray icon stops reporting throughput",
         "structural only: the tray is an in-process widget with no external observable"},
        {"poll::QTimer::timeout -> bridge::lambda",
         "the process stops probing the controller, so a controller that comes back is "
         "never noticed and the proxy list never refreshes",
         "app-smoke: theCompositionRootAttachesToADiscoveredControllerAndKeepsPollingIt"},
    };
}

/// A startup statement that is not a connect and that only the composition root
/// performs.
inline QVector<ShellOnly> shellOnlyStatements() {
    return {
        {"app.installEventFilter(quitGuard)",
         "Cmd-Q and the window close stop reaching the quit gate: the application either "
         "exits without stopping the core or cannot be quit at all",
         "structural only: the quit path is unreachable from outside the process"},
        {"bridge.attach()",
         "the process never attaches to an already-running controller, so an external core "
         "is invisible until one is launched",
         "app-smoke: theCompositionRootAttachesToADiscoveredControllerAndKeepsPollingIt"},
        {"bridge.openTrafficStream()",
         "no throughput anywhere: the tray tooltip and the overview graph stay empty",
         "app-smoke: theCompositionRootAttachesToADiscoveredControllerAndKeepsPollingIt"},
        {"poll.start()",
         "the liveness probe never runs; see the timeout connection above",
         "app-smoke: theCompositionRootAttachesToADiscoveredControllerAndKeepsPollingIt"},
    };
}

/// Registrations that must appear, by name, in both files.
inline QStringList mirroredStatements() {
    return {
        QStringLiteral("shutdown.addBusyGate(backup)"),
        QStringLiteral("shutdown.addBusyGate(profile-runtime)"),
        QStringLiteral("shutdown.addBusyGate(profile-files)"),
        QStringLiteral("shutdown.addBusyGate(enhancer-files)"),
        QStringLiteral("shutdown.addQuitAction(runtime)"),
        QStringLiteral("shutdown.addQuitAction(profiles)"),
        QStringLiteral("shutdown.addQuitAction(enhancer)"),
        QStringLiteral("shutdown.addQuitAction(backups)"),
        QStringLiteral("shutdown.setFinalCleanup(lambda)"),
        QStringLiteral("shutdown.setQuitGuard(quitGuard)"),
    };
}

// ----------------------------------------------------------------- scanning

/// Removes line and block comments, leaving string and character literals
/// untouched. src/main.cpp's comments quote code ("main.cpp:256", signal names,
/// whole call expressions), so scanning without this would find wiring that is
/// only described.
inline QString stripComments(const QString &source) {
    QString out;
    out.reserve(source.size());
    const int n = source.size();
    for (int i = 0; i < n;) {
        const QChar c = source.at(i);
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            const QChar quote = c;
            out.append(c);
            ++i;
            while (i < n) {
                if (source.at(i) == QLatin1Char('\\')) {
                    out.append(source.at(i));
                    if (i + 1 < n) out.append(source.at(i + 1));
                    i += 2;
                    continue;
                }
                out.append(source.at(i));
                const bool closing = source.at(i) == quote;
                ++i;
                if (closing) break;
            }
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && source.at(i + 1) == QLatin1Char('/')) {
            while (i < n && source.at(i) != QLatin1Char('\n')) ++i;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && source.at(i + 1) == QLatin1Char('*')) {
            i += 2;
            // Newlines are kept so the reported line numbers are the file's.
            while (i + 1 < n &&
                   !(source.at(i) == QLatin1Char('*') && source.at(i + 1) == QLatin1Char('/'))) {
                if (source.at(i) == QLatin1Char('\n')) out.append(QLatin1Char('\n'));
                ++i;
            }
            i += 2;
            continue;
        }
        out.append(c);
        ++i;
    }
    return out;
}

inline bool isIdentifierChar(QChar c) {
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

/// The index just past the `)` that closes the `(` at `open`, or -1.
inline int matchingParen(const QString &source, int open) {
    int depth = 0;
    const int n = source.size();
    for (int i = open; i < n; ++i) {
        const QChar c = source.at(i);
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            const QChar quote = c;
            ++i;
            while (i < n) {
                if (source.at(i) == QLatin1Char('\\')) {
                    i += 2;
                    continue;
                }
                if (source.at(i) == quote) break;
                ++i;
            }
            continue;
        }
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) {
            --depth;
            if (depth == 0) return i + 1;
        }
    }
    return -1;
}

/// Splits an argument list on commas that are not inside a nested call, a
/// lambda body, a template argument list or a string.
inline QStringList splitTopLevelArgs(const QString &inside) {
    QStringList args;
    QString current;
    int depth = 0;
    const int n = inside.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = inside.at(i);
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            // `i` is left ON the closing quote: the loop's own ++i moves past
            // it. Advancing here as well would swallow the comma that follows a
            // string argument, which is how every gate name used to merge with
            // the lambda beside it.
            const QChar quote = c;
            current.append(c);
            ++i;
            while (i < n) {
                const QChar ch = inside.at(i);
                current.append(ch);
                if (ch == QLatin1Char('\\')) {
                    if (i + 1 < n) current.append(inside.at(i + 1));
                    i += 2;
                    continue;
                }
                if (ch == quote) break;
                ++i;
            }
            continue;
        }
        // Brackets only. `<` and `>` are not tracked: `->` appears in every
        // lambda body here and would unbalance the count, while no argument in
        // either file carries a template argument list with a comma in it.
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) --depth;
        if (c == QLatin1Char(',') && depth == 0) {
            args << current;
            current.clear();
            continue;
        }
        current.append(c);
    }
    args << current;
    QStringList collapsed;
    collapsed.reserve(args.size());
    for (const QString &arg : args) collapsed << arg.simplified();
    return collapsed;
}

/// `&backups`, `backups.get()`, `this->backups` and `backups` are one object.
inline QString normaliseObject(QString expression) {
    expression = expression.simplified();
    while (expression.startsWith(QLatin1Char('&'))) expression = expression.mid(1).simplified();
    if (expression.startsWith(QLatin1String("this->"))) expression = expression.mid(6);
    if (expression.endsWith(QLatin1String(".get()")))
        expression.chop(QStringLiteral(".get()").size());
    if (expression.endsWith(QLatin1String("->get()")))
        expression.chop(QStringLiteral("->get()").size());
    return expression.simplified();
}

/// `&lifecycle::ShutdownCoordinator::reevaluate` and
/// `&app::lifecycle::ShutdownCoordinator::reevaluate` are one member: namespace
/// aliases differ between the two files, the class and the member do not.
inline QString normaliseMember(QString expression) {
    expression = expression.simplified();
    while (expression.startsWith(QLatin1Char('&'))) expression = expression.mid(1).simplified();
    const QStringList parts = expression.split(QStringLiteral("::"), Qt::SkipEmptyParts);
    if (parts.size() >= 2) return parts.at(parts.size() - 2) + QStringLiteral("::") + parts.last();
    return expression;
}

/// The literal inside `QStringLiteral("backup")`, `QLatin1String("backup")` or
/// `"backup"`. A non-literal argument is normalised as an object instead, so
/// `setQuitGuard(&quitGuard)` and `setQuitGuard(quitGuard.get())` agree.
inline QString normaliseArgument(QString expression) {
    expression = expression.simplified();
    if (expression.isEmpty()) return QString();
    if (expression.startsWith(QLatin1Char('['))) return QStringLiteral("lambda");
    for (const QString &wrapper :
         {QStringLiteral("QStringLiteral("), QStringLiteral("QLatin1String("),
          QStringLiteral("QByteArrayLiteral("), QStringLiteral("tr(")}) {
        if (expression.startsWith(wrapper) && expression.endsWith(QLatin1Char(')'))) {
            expression = expression.mid(wrapper.size(), expression.size() - wrapper.size() - 1)
                             .simplified();
            break;
        }
    }
    if (expression.startsWith(QLatin1Char('"')) && expression.endsWith(QLatin1Char('"')) &&
        expression.size() >= 2)
        return expression.mid(1, expression.size() - 2);
    return normaliseObject(expression);
}

inline int lineOf(const QString &source, int index) {
    return static_cast<int>(source.left(index).count(QLatin1Char('\n'))) + 1;
}

/// The object a member call was made on: the identifier immediately before the
/// `.` or `->` that precedes `call`.
inline QString receiverBefore(const QString &source, int callStart) {
    int i = callStart - 1;
    while (i >= 0 && source.at(i).isSpace()) --i;
    if (i >= 1 && source.at(i) == QLatin1Char('>') && source.at(i - 1) == QLatin1Char('-')) i -= 2;
    else if (i >= 0 && source.at(i) == QLatin1Char('.')) i -= 1;
    else return QString();
    while (i >= 0 && source.at(i).isSpace()) --i;
    const int end = i + 1;
    while (i >= 0 && isIdentifierChar(source.at(i))) --i;
    return source.mid(i + 1, end - i - 1);
}

/// Every wiring statement in `source`, which has already had its comments
/// removed. `calls` names the member calls to record beyond QObject::connect.
inline Wiring scan(const QString &source, const QStringList &calls) {
    Wiring wiring;
    const QString needle = QStringLiteral("QObject::connect(");
    for (int at = source.indexOf(needle); at >= 0; at = source.indexOf(needle, at + 1)) {
        if (at > 0 && isIdentifierChar(source.at(at - 1))) continue;
        const int open = at + needle.size() - 1;
        const int close = matchingParen(source, open);
        if (close < 0) continue;
        const QStringList args = splitTopLevelArgs(source.mid(open + 1, close - open - 2));
        if (args.size() < 3) continue;
        Edge edge;
        edge.sender = normaliseObject(args.at(0));
        edge.signalName = normaliseMember(args.at(1));
        edge.receiver = normaliseObject(args.at(2));
        edge.slot = args.size() >= 4 ? normaliseArgument(args.at(3)) : QStringLiteral("lambda");
        if (edge.slot != QLatin1String("lambda")) edge.slot = normaliseMember(args.at(3));
        edge.line = lineOf(source, at);
        wiring.edges.append(edge);
    }
    for (const QString &call : calls) {
        const QString target = call + QStringLiteral("(");
        for (int at = source.indexOf(target); at >= 0; at = source.indexOf(target, at + 1)) {
            if (at > 0 && isIdentifierChar(source.at(at - 1))) continue;
            const QString object = receiverBefore(source, at);
            if (object.isEmpty()) continue;
            const int open = at + target.size() - 1;
            const int close = matchingParen(source, open);
            if (close < 0) continue;
            const QStringList args = splitTopLevelArgs(source.mid(open + 1, close - open - 2));
            Statement statement;
            statement.object = object;
            statement.call = call;
            statement.argument = args.isEmpty() ? QString() : normaliseArgument(args.at(0));
            statement.line = lineOf(source, at);
            wiring.statements.append(statement);
        }
    }
    return wiring;
}

// ------------------------------------------------------------ source lookup

/// The checkout this test was built from. `__FILE__` is what the build system
/// compiled this header from, and tests/CMakeLists.txt puts an ABSOLUTE
/// `${CMAKE_CURRENT_SOURCE_DIR}` on the include path, so it resolves without a
/// registration of its own. CLASH_QT_SOURCE_DIR overrides it where that is not
/// true.
inline QString sourceRoot() {
    const QString override = qEnvironmentVariable("CLASH_QT_SOURCE_DIR");
    if (!override.isEmpty()) return QDir(override).absolutePath();
    QDir dir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath());
    for (int up = 0; up < 8; ++up) {
        if (QFileInfo::exists(dir.filePath(QStringLiteral("src/main.cpp"))) &&
            QFileInfo::exists(dir.filePath(QStringLiteral("tests/workflows/workflow_support.h"))))
            return dir.absolutePath();
        if (!dir.cdUp()) break;
    }
    return QString();
}

inline QString readSource(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read %1: %2").arg(path, file.errorString());
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

// ------------------------------------------------------------- the audit

/// Empty when src/main.cpp and wf::AssembledApp still describe one program.
/// Otherwise a report naming every difference, one per line.
inline QString compositionRootDrift() {
    QStringList problems;
    const QString root = sourceRoot();
    if (root.isEmpty()) {
        return QStringLiteral(
            "the source checkout was not found from __FILE__ (%1). This audit reads "
            "src/main.cpp; set CLASH_QT_SOURCE_DIR to the checkout to run it. It is not "
            "skipped: an audit that silently answers 'no drift' is worse than none.")
            .arg(QString::fromUtf8(__FILE__));
    }

    const QString mainPath = root + QStringLiteral("/src/main.cpp");
    const QString harnessPath = root + QStringLiteral("/tests/workflows/workflow_support.h");
    QString error;
    const QString mainSource = readSource(mainPath, &error);
    if (!error.isEmpty()) return error;
    QString harnessSource = readSource(harnessPath, &error);
    if (!error.isEmpty()) return error;

    // Only AssembledApp's own wiring. ControllerRelay above it connects sockets
    // to sockets, which is fixture plumbing and not a claim about the
    // application.
    const int assembled = harnessSource.indexOf(QStringLiteral("class AssembledApp"));
    if (assembled < 0)
        return QStringLiteral("workflow_support.h no longer declares class AssembledApp");
    const int harnessLineOffset =
        static_cast<int>(harnessSource.left(assembled).count(QLatin1Char('\n')));
    harnessSource = harnessSource.mid(assembled);

    const QStringList calls{QStringLiteral("addBusyGate"), QStringLiteral("addQuitAction"),
                            QStringLiteral("setFinalCleanup"), QStringLiteral("setQuitGuard"),
                            QStringLiteral("installEventFilter"), QStringLiteral("attach"),
                            QStringLiteral("openTrafficStream"), QStringLiteral("start")};
    const Wiring shell = scan(stripComments(mainSource), calls);
    Wiring harness = scan(stripComments(harnessSource), calls);
    // The harness was scanned from a slice, so its line numbers are the file's
    // only once the skipped prelude is added back.
    for (Edge &edge : harness.edges) edge.line += harnessLineOffset;
    for (Statement &statement : harness.statements) statement.line += harnessLineOffset;

    if (shell.edges.isEmpty())
        return QStringLiteral("no QObject::connect found in %1 - the scanner is broken, or the "
                              "composition root no longer wires anything")
            .arg(mainPath);

    QSet<QString> bySignal;
    for (const MirroredBySignal &entry : mirroredBySignal())
        bySignal.insert(QString::fromLatin1(entry.signalKey));
    QSet<QString> shellOnlyKeys;
    for (const ShellOnly &entry : shellOnlyEdges())
        shellOnlyKeys.insert(QString::fromLatin1(entry.key));

    // ---- main.cpp -> the harness ------------------------------------------
    QSet<QString> seenShellKeys;
    for (const Edge &edge : shell.edges) {
        seenShellKeys.insert(edge.key());
        if (shellOnlyKeys.contains(edge.key())) continue;
        if (bySignal.contains(edge.signalKey())) {
            if (!harness.hasSignal(edge.signalKey()))
                problems << QStringLiteral(
                                "%1 is routed into the shell by the composition root and NOTHING "
                                "in wf::AssembledApp observes it, so no journey can see it fire")
                                .arg(edge.describe(QStringLiteral("src/main.cpp")));
            continue;
        }
        if (!harness.hasEdgeKey(edge.key()))
            problems << QStringLiteral(
                            "%1 is wired by the composition root and NOT by wf::AssembledApp: the "
                            "journeys are exercising a different graph")
                            .arg(edge.describe(QStringLiteral("src/main.cpp")));
    }

    // ---- the harness -> main.cpp ------------------------------------------
    for (const Edge &edge : harness.edges) {
        if (shell.hasEdgeKey(edge.key())) continue;
        if (bySignal.contains(edge.signalKey()) && shell.hasSignal(edge.signalKey())) continue;
        problems << QStringLiteral(
                        "%1 is wired by wf::AssembledApp and NOT by the composition root: the "
                        "journeys are asserting behaviour the application does not have")
                        .arg(edge.describe(QStringLiteral("tests/workflows/workflow_support.h")));
    }

    // ---- the connections only the shell can make --------------------------
    for (const ShellOnly &entry : shellOnlyEdges()) {
        if (seenShellKeys.contains(QString::fromLatin1(entry.key))) continue;
        problems << QStringLiteral("src/main.cpp no longer wires `%1`. Consequence: %2. Covered "
                                   "by: %3.")
                        .arg(QString::fromLatin1(entry.key), QString::fromLatin1(entry.consequence),
                             QString::fromLatin1(entry.covered));
    }
    for (const MirroredBySignal &entry : mirroredBySignal()) {
        if (shell.hasSignal(QString::fromLatin1(entry.signalKey))) continue;
        problems << QStringLiteral("src/main.cpp no longer connects `%1`, which %2.")
                        .arg(QString::fromLatin1(entry.signalKey),
                             QString::fromLatin1(entry.shell));
    }

    // ---- registrations, in both files, by name ----------------------------
    for (const QString &key : mirroredStatements()) {
        if (!shell.hasStatement(key))
            problems << QStringLiteral("src/main.cpp no longer registers `%1`").arg(key);
        if (!harness.hasStatement(key))
            problems << QStringLiteral("wf::AssembledApp no longer registers `%1`, so no journey "
                                       "exercises it")
                            .arg(key);
    }
    for (const Statement &statement : shell.statements) {
        if (statement.call == QLatin1String("addBusyGate") ||
            statement.call == QLatin1String("addQuitAction")) {
            if (!mirroredStatements().contains(statement.key()))
                problems << QStringLiteral(
                                "%1 is registered by the composition root and is not in this "
                                "audit's inventory: add it here and to wf::AssembledApp")
                                .arg(statement.describe(QStringLiteral("src/main.cpp")));
        }
    }
    for (const Statement &statement : harness.statements) {
        if (statement.call == QLatin1String("addBusyGate") ||
            statement.call == QLatin1String("addQuitAction")) {
            if (!shell.hasStatement(statement.key()))
                problems << QStringLiteral("%1 is registered by wf::AssembledApp and not by the "
                                           "composition root")
                                .arg(statement.describe(
                                    QStringLiteral("tests/workflows/workflow_support.h")));
        }
    }

    // ---- the startup statements only the shell performs --------------------
    for (const ShellOnly &entry : shellOnlyStatements()) {
        const QString key = QString::fromLatin1(entry.key);
        const int paren = key.indexOf(QLatin1Char('('));
        const QString prefix = key.left(paren + 1);
        bool found = false;
        for (const Statement &statement : shell.statements)
            if (statement.key().startsWith(prefix)) found = true;
        if (!found)
            problems << QStringLiteral("src/main.cpp no longer performs `%1`. Consequence: %2. "
                                       "Covered by: %3.")
                            .arg(key, QString::fromLatin1(entry.consequence),
                                 QString::fromLatin1(entry.covered));
    }

    if (problems.isEmpty()) return QString();
    return QStringLiteral("the composition root and wf::AssembledApp have drifted apart:\n  ") +
           problems.join(QStringLiteral("\n  ")) +
           QStringLiteral("\n(tests/workflows/composition_root_audit.h says what to do about it)");
}

}  // namespace workflows::audit
