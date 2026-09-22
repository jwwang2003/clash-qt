// Shared machinery for the complete-journey suites.
//
// A workflow suite is not a unit test with more objects in it. It assembles the
// same graph src/main.cpp assembles, in the same construction order, over a
// real filesystem, real Qt networking and a real child process, and then walks
// a user's journey through it. What lives here is only what more than one
// journey needs:
//
//   * AssembledApp - the composition root's object graph, minus the widgets and
//     minus the two things a test may never touch (the real SystemProxyService
//     singleton and the real privileged helper). Construction and destruction
//     order are main.cpp's, because that order is load-bearing: every
//     coordinator is destroyed before the backend it observes, and the module
//     the backend lives in is unmapped after everything it produced.
//   * moduleArtifactPath / moduleRequirementFailure - the one environment input
//     these journeys REQUIRE. The engine is a separately built shared library
//     rather than a linked one: the journeys drive it through
//     clashqt::integration::ModuleLoader and ModuleBackend, exactly as
//     src/main.cpp does, and nothing here includes core/mihomo/** any more.
//     The artifact is never searched for - CLASH_QT_MODULE_PATH names it, the
//     journeys FAIL with that requirement when it is absent, and
//     ModuleLoader::load() is asked for the SHIPPING module id, so the test
//     double cannot be loaded here by accident.
//   * deadlineFor - a wait sized from a budget the backend PUBLISHES. There is
//     no setTimings() across the boundary and there is deliberately no
//     replacement for it: a journey that wants to see a readiness timeout waits
//     out the module's real 10 s idle deadline, and one that wants to see a
//     termination escalation waits out its real 3 s terminate wait.
//   * ProxyOperations - what the OS proxy was asked to do, recorded from the
//     worker thread. platform::SystemProxyService is REAL; only the one
//     std::function that would shell out to the machine is substituted, so
//     nothing in this directory can change the developer's proxy settings.
//   * waitFor / drainPostedTimers / drainPast - a deadline, never a sleep. Each
//     returns on the first iteration in which its predicate holds; the callers
//     print the pending operations, the backend state and the redacted request
//     transcript when one expires, which is what makes a timeout diagnosable.
//   * liveProcessesOf / awaitNoLiveProcess - an oracle for "the owned child
//     exited" that does not ask the object under test. A leaked process is a
//     test failure, so it is asked of the operating system.
//   * ControllerRelay - the fixed controller address core::ProfileStore writes
//     into every configuration it generates, bridged onto the loopback
//     fixture's ephemeral port.
//
// Everything here is header-only: tests/workflows has no library of its own and
// the four suites must not acquire a shared compiled artefact that could drift
// from what they assert.
#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QIODevice>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "app/backup/backup_coordinator.h"
#include "app/backup/backup_store_session.h"
#include "app/lifecycle/quit_guard.h"
#include "app/lifecycle/shutdown_coordinator.h"
#include "app/lifecycle/shutdown_ports.h"
#include "app/runtime/profile_config_source.h"
#include "app/runtime/routing_controller.h"
#include "app/runtime/runtime_coordinator.h"
#include "core/backend/backend.h"
#include "core/backend/privileged_core_service.h"
#include "core/component/abi/backend_abi.h"
#include "core/component/com_ptr.h"
#include "core/config/enhance/config_enhancer.h"
#include "core/profiles/profile_store.h"
#include "integrations/component/module_backend.h"
#include "integrations/component/module_loader.h"
#include "platform/proxy/system_proxy_service.h"

namespace workflows {

namespace cb = core::backend;
namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;

using ModuleBackend = clashqt::integration::ModuleBackend;
using ModuleLoader = clashqt::integration::ModuleLoader;

// The deadline every await in this directory runs against. Only ever a bound on
// a failure: a passing journey returns as soon as its predicate holds.
inline constexpr int kDeadlineMs = 15000;

// ---------------------------------------------------------------- awaiting

/// Spins the calling thread's event loop until `predicate` holds or the
/// deadline passes. No sleep: it returns on the first iteration in which the
/// predicate is true.
inline bool waitFor(const std::function<bool()> &predicate, int timeoutMs = kDeadlineMs) {
    if (predicate()) return true;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (predicate()) return true;
    }
    return predicate();
}

/// Runs the event loop until every zero-delay timer posted so far has fired,
/// and then once more. Used where production defers a decision with
/// singleShot(0) - the autostart hop, the proxy re-validation hop - so a test
/// can observe the decision rather than race it.
inline bool drainPostedTimers() {
    bool marked = false;
    QTimer::singleShot(0, QCoreApplication::instance(), [&marked] {
        QTimer::singleShot(0, QCoreApplication::instance(), [&marked] { marked = true; });
    });
    return waitFor([&marked] { return marked; }, kDeadlineMs);
}

/// A deadline for an event whose own budget the backend PUBLISHES - the
/// readiness idle deadline, the termination wait - plus room for the event loop
/// and the operating system.
///
/// Every journey that used to shrink such a budget through
/// MihomoBackendImpl::setTimings() sizes its wait with this instead. There is no
/// setTimings() on the far side of the module boundary and this deliberately
/// does not reintroduce one: a private knob that makes a 10-second readiness
/// deadline expire in 400 ms proves that *some* deadline works, not that the one
/// the application ships does. The cost is a journey that takes ten seconds to
/// watch a core fail to become ready, which is the honest price of the claim.
inline int deadlineFor(std::uint32_t publishedMs, int slackMs = kDeadlineMs) {
    return static_cast<int>(publishedMs) + slackMs;
}

/// Runs the event loop past `ms` and then past everything that expired with it.
/// The only time-based wait in this directory, and it is used solely to prove
/// that a *compressed* grace window elapsed without a restore happening - the
/// absence of an event cannot be awaited on a predicate.
inline bool drainPast(int ms) {
    bool marked = false;
    QTimer::singleShot(ms, QCoreApplication::instance(), [&marked] {
        QTimer::singleShot(0, QCoreApplication::instance(), [&marked] { marked = true; });
    });
    return waitFor([&marked] { return marked; }, kDeadlineMs + ms);
}

// ------------------------------------------------------- process liveness

/// How many live processes were started from `binaryPath`.
///
/// An oracle INDEPENDENT of the objects under test: it asks the operating
/// system, not the backend that claims to have stopped the child. Each journey
/// copies the fake core into its own temporary directory, so the path is unique
/// to one test and the count is exact rather than approximate.
///
/// POSIX only. Returns -1 where it cannot answer, and a caller treats -1 as
/// "not asserted" rather than "zero" - a guard that silently answers zero is
/// worse than no guard.
inline int liveProcessesOf(const QString &binaryPath) {
#if defined(Q_OS_WIN)
    Q_UNUSED(binaryPath);
    return -1;
#else
    QProcess pgrep;
    pgrep.start(QStringLiteral("/usr/bin/pgrep"), {QStringLiteral("-f"), binaryPath});
    if (!pgrep.waitForStarted(3000)) return -1;
    if (!pgrep.waitForFinished(5000)) {
        pgrep.kill();
        pgrep.waitForFinished(2000);
        return -1;
    }
    // pgrep exits 1 with no output when nothing matches, which is a real zero.
    const QByteArray output = pgrep.readAllStandardOutput().trimmed();
    if (output.isEmpty()) return pgrep.exitCode() <= 1 ? 0 : -1;
    return static_cast<int>(output.split('\n').size());
#endif
}

/// True once nothing started from `binaryPath` is running any more, or once the
/// platform cannot answer. Bounded: a child that refuses to die fails the test
/// instead of hanging it.
inline bool awaitNoLiveProcess(const QString &binaryPath, int timeoutMs = kDeadlineMs) {
    return waitFor([&binaryPath] { return liveProcessesOf(binaryPath) == 0; }, timeoutMs);
}

// -------------------------------------------------------------- proxy seam

using ProxyAction = platform::SystemProxyAction;
using ProxyConfig = platform::ProxyConfig;
using ProxyResult = platform::SystemProxyResult;

/// Every OS proxy command the journey caused, recorded from whichever worker
/// thread ran it. `restoreSucceeds` is how a journey makes a restore fail
/// without touching the machine.
class ProxyOperations {
  public:
    QVector<ProxyAction> actions() const {
        QMutexLocker locker(&mutex_);
        return actions_;
    }
    int count(ProxyAction action) const {
        QMutexLocker locker(&mutex_);
        return static_cast<int>(std::count(actions_.begin(), actions_.end(), action));
    }
    int total() const {
        QMutexLocker locker(&mutex_);
        return static_cast<int>(actions_.size());
    }
    void record(ProxyAction action) {
        QMutexLocker locker(&mutex_);
        actions_.append(action);
    }
    QString transcript() const {
        QMutexLocker locker(&mutex_);
        QStringList names;
        for (ProxyAction action : actions_) {
            switch (action) {
                case ProxyAction::Enable: names << QStringLiteral("Enable"); break;
                case ProxyAction::Disable: names << QStringLiteral("Disable"); break;
                case ProxyAction::Restore: names << QStringLiteral("Restore"); break;
                case ProxyAction::Refresh: names << QStringLiteral("Refresh"); break;
            }
        }
        return names.join(QStringLiteral(" -> "));
    }

    std::atomic_bool restoreSucceeds{true};
    std::atomic_bool enableSucceeds{true};

  private:
    mutable QMutex mutex_;
    QVector<ProxyAction> actions_;
};

/// A REAL platform::SystemProxyService whose single OS call is an in-process
/// function. The serialisation, the queueing, the shutdown ordering and the
/// ownership bookkeeping are production code; only the command that would reach
/// the machine is replaced. `osState` is what the "OS" now reports, so a
/// read-back is a read-back rather than an echo.
inline std::unique_ptr<platform::SystemProxyService> makeIsolatedProxyService(
    const std::shared_ptr<ProxyOperations> &log, const std::shared_ptr<ProxyConfig> &osState) {
    return std::make_unique<platform::SystemProxyService>(
        nullptr, [log, osState](ProxyAction action, const ProxyConfig &config) {
            log->record(action);
            ProxyResult result;
            result.state.supported = true;
            result.state.valid = true;
            switch (action) {
                case ProxyAction::Enable:
                    if (!log->enableSucceeds.load()) {
                        result.success = false;
                        result.error = QStringLiteral("the OS refused the proxy change");
                        result.state.config = *osState;
                        result.state.owned = osState->port != 0;
                        return result;
                    }
                    *osState = config;
                    break;
                case ProxyAction::Disable:
                    *osState = ProxyConfig{};
                    break;
                case ProxyAction::Restore:
                    result.success = log->restoreSucceeds.load();
                    if (!result.success) {
                        result.error = QStringLiteral("the helper refused");
                        result.state.config = *osState;
                        result.state.owned = osState->port != 0;
                        return result;
                    }
                    *osState = ProxyConfig{};
                    break;
                case ProxyAction::Refresh:
                    break;
            }
            result.state.config = *osState;
            result.state.owned = result.state.config.port != 0;
            return result;
        });
}

// ---------------------------------------------------------- the module seam

/// The artifact CLASH_QT_MODULE_PATH names, or empty.
///
/// The ONLY way a journey chooses a module. ModuleLoader's other constructor
/// input, installedModulePath(), is installation-relative - a bundle's
/// Frameworks directory - and a build tree has no such layout, so a journey
/// that fell back to it would load nothing and say "not installed" instead of
/// "not registered". tests/workflows/CMakeLists.txt sets the variable from
/// $<TARGET_FILE:clash_qt_backend_module>.
inline QString moduleArtifactPath() {
    return qEnvironmentVariable("CLASH_QT_MODULE_PATH");
}

/// Why this process cannot drive a module, or empty when it can. A journey
/// asserts this rather than skipping: an unregistered module is a build defect,
/// and the whole point of these suites is that the engine is reached across the
/// module boundary rather than linked in.
inline QString moduleRequirementFailure() {
    const QString path = moduleArtifactPath();
    if (path.isEmpty()) {
        return QStringLiteral(
            "CLASH_QT_MODULE_PATH is not set. The engine lives in a separately built "
            "shared library and these journeys load it exactly as "
            "src/main.cpp does; there is no source-tree, build-tree or PATH fallback by "
            "design. Register the suite with ENVIRONMENT "
            "\"CLASH_QT_MODULE_PATH=$<TARGET_FILE:clash_qt_backend_module>\".");
    }
    if (!QFileInfo(path).isFile()) {
        return QStringLiteral("CLASH_QT_MODULE_PATH names %1, which is not a file. The module "
                              "target has to be built before this suite runs "
                              "(add_dependencies(<suite> clash_qt_backend_module)).")
            .arg(path);
    }
    return {};
}

/// Loads the SHIPPING module through `loader` and wraps one session.
///
/// `expectedModuleId` defaults to the shipping id inside ModuleLoader::load(),
/// so a stray CLASH_QT_MODULE_PATH pointing at the test double is refused by the
/// handshake rather than silently accepted - the journeys assert the real
/// supervisor's behaviour and a fake one answering in its place would be worse
/// than a failure.
///
/// A failure does NOT throw and does not return null: the graph below still has
/// to be constructible so that its construction order stays the thing under
/// test. It returns an inert ModuleBackend (no session, isValid() false, every
/// command failing) and writes the reason into `*error`, which every journey
/// asserts is empty before it does anything else.
inline std::unique_ptr<ModuleBackend> openModuleBackend(ModuleLoader &loader,
                                                        core::PrivilegedCoreService *service,
                                                        QString *error) {
    com::ComPtr<abi::IBackendSession> session;
    const QString requirement = moduleRequirementFailure();
    if (!requirement.isEmpty()) {
        *error = requirement;
    } else if (!loader.load() || !loader.createSession(session)) {
        *error = QStringLiteral("could not load the engine component at %1: %2")
                     .arg(loader.artifactPath(), loader.lastError());
    }
    auto backend = std::make_unique<ModuleBackend>(std::move(session), service);
    if (error->isEmpty() && !backend->isValid()) {
        *error = QStringLiteral("the module at %1 would not accept a host: %2")
                     .arg(loader.artifactPath(), backend->lastError().message);
    }
    return backend;
}

// ------------------------------------------------- the fixed controller port

/// The address core::ProfileStore writes into every runtime configuration it
/// generates. It is a PROTECTED field: `buildRuntime()` overwrites whatever the
/// profile and the user overrides said, after applying them, so a journey that
/// launches a core through the real profile store cannot choose where that core
/// is expected to answer. Duplicated here as a literal on purpose - the
/// constant is private to profile_store.cpp - and every journey that relies on
/// it re-reads the generated file and compares, so a production change to the
/// address fails a workflow loudly instead of turning it into a silent skip.
inline constexpr quint16 kGeneratedControllerPort = 29097;

/// A transparent TCP bridge from a fixed port onto testsupport::LoopbackServer's
/// ephemeral one.
///
/// WHY THIS EXISTS. The shared loopback fixture binds port 0, which is right for
/// every suite that writes its own configuration. A complete journey does not
/// write its own configuration - that is the point of it - so the controller has
/// to answer where the generated file says, and the fixture cannot be asked to
/// bind there. Rather than reimplement HTTP, WebSocket framing, request
/// recording, gating and redaction beside it, this relays bytes: the fixture
/// keeps doing all of that, one hop away.
///
/// A fixed port is a shared machine resource. listen() returning false is not a
/// fixture bug - it usually means the developer's own core, or another workflow
/// suite, already holds it - and the caller SKIPS with that reason rather than
/// asserting something weaker.
class ControllerRelay {
  public:
    ControllerRelay() = default;
    ~ControllerRelay() { close(); }

    ControllerRelay(const ControllerRelay &) = delete;
    ControllerRelay &operator=(const ControllerRelay &) = delete;

    bool listen(quint16 target, quint16 port = kGeneratedControllerPort) {
        target_ = target;
        QObject::connect(&server_, &QTcpServer::newConnection, &context_, [this] { accept(); });
        if (!server_.listen(QHostAddress::LocalHost, port)) {
            error_ = QStringLiteral("cannot bind 127.0.0.1:%1 (%2)")
                         .arg(port)
                         .arg(server_.errorString());
            return false;
        }
        return true;
    }

    void close() {
        server_.close();
        for (const auto &pair : std::as_const(pairs_)) {
            if (pair.client) pair.client->abort();
            if (pair.upstream) pair.upstream->abort();
        }
        qDeleteAll(owned_);
        owned_.clear();
        pairs_.clear();
    }

    quint16 port() const { return server_.serverPort(); }
    QString errorString() const { return error_; }
    int acceptedConnections() const { return accepted_; }

  private:
    struct Pair {
        QPointer<QTcpSocket> client;
        QPointer<QTcpSocket> upstream;
    };

    void accept() {
        while (QTcpSocket *client = server_.nextPendingConnection()) {
            ++accepted_;
            auto *upstream = new QTcpSocket(&context_);
            owned_.append(client);
            owned_.append(upstream);
            auto pending = std::make_shared<QByteArray>();
            auto connected = std::make_shared<bool>(false);
            pairs_.append(Pair{client, upstream});

            QObject::connect(client, &QTcpSocket::readyRead, &context_,
                             [client, upstream, pending, connected] {
                                 const QByteArray chunk = client->readAll();
                                 if (*connected) upstream->write(chunk);
                                 else pending->append(chunk);
                             });
            QObject::connect(upstream, &QTcpSocket::connected, &context_,
                             [upstream, pending, connected] {
                                 *connected = true;
                                 if (!pending->isEmpty()) {
                                     upstream->write(*pending);
                                     pending->clear();
                                 }
                             });
            QObject::connect(upstream, &QTcpSocket::readyRead, &context_,
                             [client, upstream] { client->write(upstream->readAll()); });
            QObject::connect(client, &QTcpSocket::disconnected, &context_,
                             [upstream] { upstream->disconnectFromHost(); });
            QObject::connect(upstream, &QTcpSocket::disconnected, &context_,
                             [client] { client->disconnectFromHost(); });
            upstream->connectToHost(QHostAddress::LocalHost, target_);
        }
    }

    QObject context_;
    QTcpServer server_;
    QList<QTcpSocket *> owned_;
    QList<Pair> pairs_;
    quint16 target_ = 0;
    int accepted_ = 0;
    QString error_;
};

/// The `external-controller` port the profile store actually wrote, read back
/// out of a generated runtime configuration. 0 when the file has none.
inline quint16 controllerPortOf(const QString &configPath) {
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const QByteArray body = file.readAll();
    for (const QByteArray &line : body.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.startsWith("external-controller:")) continue;
        const QByteArray value = trimmed.mid(trimmed.indexOf(':') + 1).trimmed();
        const int colon = value.lastIndexOf(':');
        if (colon < 0) return 0;
        return static_cast<quint16>(value.mid(colon + 1).toUInt());
    }
    return 0;
}

// ------------------------------------------------------------ observation

/// What reached a backend observer during a journey. Deliberately a record
/// rather than a set of expectations: a journey asserts on the sequence it
/// produced, and prints it when a deadline expires.
class JourneyObserver final : public cb::BackendObserver {
  public:
    struct StateEvent {
        cb::Generation generation;
        cb::CoreState state;
        cb::Ownership ownership;
    };

    std::vector<StateEvent> states;
    std::vector<cb::Completion> failures;
    std::vector<cb::Completion> coreFailures;
    std::vector<cb::Endpoint> readyEndpoints;
    std::vector<cb::StopCompleted> stops;
    std::vector<cb::TunChangeCompleted> tunChanges;
    std::vector<QString> modes;
    std::vector<QString> versions;
    std::vector<QString> logLines;
    int connectedChanges = 0;
    bool connected = false;

    bool sawState(cb::CoreState state) const {
        for (const auto &event : states)
            if (event.state == state) return true;
        return false;
    }
    QString transcript() const {
        QStringList out;
        for (const auto &event : states)
            out << QStringLiteral("state=%1").arg(static_cast<int>(event.state));
        for (const auto &stop : stops)
            out << QStringLiteral("stop(confirmed=%1,status=%2)")
                       .arg(stop.confirmed ? QStringLiteral("true") : QStringLiteral("false"))
                       .arg(static_cast<int>(stop.status));
        for (const auto &failure : coreFailures)
            out << QStringLiteral("coreFailed(code=%1)").arg(static_cast<int>(failure.error.code));
        return out.join(QStringLiteral(", "));
    }

    void coreStateChanged(cb::Generation generation, cb::CoreState state,
                          cb::Ownership ownership) noexcept override {
        states.push_back({generation, state, ownership});
    }
    void coreReady(const cb::Completion &, const cb::Endpoint &endpoint) noexcept override {
        readyEndpoints.push_back(endpoint);
    }
    void coreLogLine(cb::Generation, const QString &line) noexcept override {
        logLines.push_back(line);
    }
    void coreFailed(const cb::Completion &completion) noexcept override {
        coreFailures.push_back(completion);
    }
    void stopCompleted(const cb::StopCompleted &result) noexcept override {
        stops.push_back(result);
    }
    void connectedChanged(cb::Generation, bool isConnected) noexcept override {
        ++connectedChanges;
        connected = isConnected;
    }
    void modeChanged(const cb::Completion &completion, const QString &mode) noexcept override {
        if (completion.isOk()) modes.push_back(mode);
    }
    void tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept override {
        tunChanges.push_back(result);
    }
    void versionReceived(const cb::Completion &completion, const QString &version) noexcept override {
        if (completion.isOk()) versions.push_back(version);
    }
    void errorOccurred(const cb::Completion &completion) noexcept override {
        failures.push_back(completion);
    }
};

// ------------------------------------------------------------ fixture I/O

inline bool writeTextFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(body) == body.size();
}

inline QByteArray readTextFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

/// A DIRECT-only profile pointed at a loopback controller. No subscription, no
/// remote node, nothing that could reach the network. A journey's configuration
/// is synthetic and DIRECT-only, with TUN off and loopback ports, and a routine
/// run never contacts a real subscription.
inline QByteArray directOnlyProfile(quint16 controllerPort, const QString &marker = QString()) {
    QByteArray yaml;
    yaml += "mixed-port: 0\n";
    yaml += "allow-lan: false\n";
    yaml += "mode: rule\n";
    yaml += "log-level: info\n";
    yaml += "external-controller: 127.0.0.1:" + QByteArray::number(controllerPort) + "\n";
    yaml += "tun:\n  enable: false\n";
    yaml += "proxies: []\n";
    yaml += "proxy-groups: []\n";
    yaml += "rules:\n  - MATCH,DIRECT\n";
    if (!marker.isEmpty()) yaml += "clash-qt-workflow-marker: " + marker.toUtf8() + "\n";
    return yaml;
}

// ------------------------------------------------------- the assembled app

/// The composition root's object graph, minus the shell.
///
/// DECLARATION ORDER IS src/main.cpp's ORDER, and therefore destruction is its
/// reverse: every coordinator is destroyed before the backend it observes, and
/// the proxy service outlives the controller that holds a pointer to it. That
/// is not a stylistic echo - RoutingController's header states that destroying
/// it is what cancels a pending restore, so a journey that assembled the graph
/// in a different order would be testing a different program.
///
/// THE ENGINE IS BEHIND THE MODULE BOUNDARY, exactly as it is in main.cpp.
/// `backend` is a clashqt::integration::ModuleBackend over a session the loader
/// created: the same class, over the same ABI, with the same host-owned
/// privileged seam. The loader is declared before it, so the library is
/// unmapped only after every object, buffer and callback it produced is gone -
/// ModuleLoader::unload() refuses while any is alive, and its destructor
/// honours that refusal rather than pulling the mapping out from under a live
/// vtable.
///
/// TWO DELIBERATE DIFFERENCES FROM main.cpp, both required because a test may
/// not touch the developer's machine:
///   * the proxy service is a private instance with a substituted OS command,
///     not platform::SystemProxyService::instance(). The singleton writes the
///     developer's machine.
///   * the privileged seam is core::NullPrivilegedCoreService, not the real
///     client. The real one connects to a root-owned helper socket - and it
///     would now be reached through the module's reverse interface, which makes
///     it no less real. Which seam main.cpp actually injects is asserted by the
///     application smoke harness against the shipped binary, because that is
///     the only place it is observable without editing production code.
class AssembledApp {
    // Declared FIRST, deliberately: members are initialised in declaration
    // order and destroyed in its reverse, so the seam handed to the backend
    // below must be constructed before it and destroyed after it. main.cpp
    // makes the same statement in a comment over the same two lines. It is also
    // what the module marshals its privileged requests back to, so it has to
    // outlive every pending one.
    core::NullPrivilegedCoreService privilegedService_;

  public:
    /// Why the engine component could not be loaded, empty when it was.
    /// Declared before the graph because openModuleBackend() writes it while
    /// the graph is still being constructed.
    QString moduleError;

    /// Registered for the WHOLE life of the graph, from the constructor, so a
    /// journey never asserts on a recorder that started listening after the
    /// event it is asking about. Removed in the destructor, before the backend
    /// it is registered with is destroyed.
    JourneyObserver events;

    explicit AssembledApp(const std::shared_ptr<ProxyOperations> &proxyLog,
                          const std::shared_ptr<ProxyConfig> &osProxyState,
                          app::runtime::RestoreDelays delays = {})
        : profiles(std::make_unique<core::ProfileStore>()),
          enhancer(std::make_unique<core::ConfigEnhancer>()),
          moduleLoader(std::make_unique<ModuleLoader>(moduleArtifactPath())),
          backend(openModuleBackend(*moduleLoader, &privilegedService_, &moduleError)),
          proxyService(makeIsolatedProxyService(proxyLog, osProxyState)),
          configs(std::make_unique<app::runtime::ProfileStoreConfigSource>(profiles.get())),
          runtimeCoordinator(
              std::make_unique<app::runtime::RuntimeCoordinator>(*backend, *configs)),
          routing(std::make_unique<app::runtime::RoutingController>(*backend, proxyService.get(),
                                                                    delays)),
          quitGuard(std::make_unique<app::lifecycle::QuitGuard>()),
          proxyShutdown(std::make_unique<app::lifecycle::FunctionProxyShutdown>(
              [this] { proxyService->shutdown(); })),
          drain(std::make_unique<app::lifecycle::GlobalThreadPoolDrain>()),
          shutdown(std::make_unique<app::lifecycle::ShutdownCoordinator>(*backend, *proxyShutdown,
                                                                         *drain)),
          backups(std::make_unique<app::backup::BackupCoordinator>(
              std::make_unique<app::backup::BackupStoreSession>(profiles->dataDir()), *backend,
              app::backup::StoreHooks{
                  [this](bool enabled) { profiles->setMaintenanceMode(enabled); },
                  [this] { return profiles->isFileBusy(); }, [this] { profiles->load(); }},
              app::backup::StoreHooks{
                  [this](bool enabled) { enhancer->setMaintenanceMode(enabled); },
                  [this] { return enhancer->isFileBusy(); }, [this] { enhancer->load(); }},
              shutdown.get())) {
        backend->addObserver(&events);
        wire();
    }

    ~AssembledApp() {
        // The observer first: core/backend/observer.h requires it to be removed
        // before it is destroyed, and it is a member of this object. Across the
        // boundary this is also what stops a queued module event from reaching
        // a half-destroyed recorder.
        backend->removeObserver(&events);
        // Everything else is reverse declaration order, which is main.cpp's
        // reverse construction order - stated rather than implied, because a
        // journey that leaked a coordinator past its backend would fail
        // somewhere else entirely.
    }

    AssembledApp(const AssembledApp &) = delete;
    AssembledApp &operator=(const AssembledApp &) = delete;

    /// main.cpp's settings bootstrap: the saved binary, the execution mode, the
    /// chain and the profiles, with the startup error channel connected first.
    void bootstrap(const QString &binaryPath) {
        const auto profileErrors =
            QObject::connect(profiles.get(), &core::ProfileStore::errorOccurred, profiles.get(),
                             [this](const QString &message) { startupErrors.append(message); });
        const auto chainErrors =
            QObject::connect(enhancer.get(), &core::ConfigEnhancer::errorOccurred, enhancer.get(),
                             [this](const QString &message) { startupErrors.append(message); });
        backend->setBinaryPath(binaryPath);
        enhancer->load();
        profiles->setEnhancer(enhancer.get());
        profiles->load();
        QObject::disconnect(profileErrors);
        QObject::disconnect(chainErrors);
    }

    /// The quit the shell would have driven. Returns once the coordinator has
    /// approved, or false on the deadline - a quit that never completes is a
    /// bounded failure, not a hang.
    bool quit(int timeoutMs = kDeadlineMs) {
        shutdown->requestQuit();
        return waitFor([this] { return shutdown->isQuitApproved(); }, timeoutMs);
    }

    QString blockingReason() const { return shutdown->blockingReason(); }

    /// The module loaded, handshook and produced a usable session. Asserted by
    /// every journey immediately after construction: without it the graph is
    /// intact but inert, and a later "the core never started" would name the
    /// wrong cause.
    bool moduleLoaded() const { return moduleError.isEmpty() && backend->isValid(); }
    /// The artifact this graph is actually driving, for a failure message and
    /// for the journeys that assert which one was selected.
    QString moduleArtifact() const { return moduleLoader->artifactPath(); }
    /// Events the module produced that this host did not understand. Non-zero
    /// with a matching handshake is a marshalling bug, so journeys assert zero
    /// rather than assuming it.
    std::uint64_t unknownModuleEvents() const { return backend->unknownEventCount(); }

    // Declared in src/main.cpp's construction order; destroyed in its reverse.
    std::unique_ptr<core::ProfileStore> profiles;
    std::unique_ptr<core::ConfigEnhancer> enhancer;
    /// Before `backend`, so the library outlives everything it created.
    std::unique_ptr<ModuleLoader> moduleLoader;
    std::unique_ptr<ModuleBackend> backend;
    std::unique_ptr<platform::SystemProxyService> proxyService;
    std::unique_ptr<app::runtime::ProfileStoreConfigSource> configs;
    std::unique_ptr<app::runtime::RuntimeCoordinator> runtimeCoordinator;
    std::unique_ptr<app::runtime::RoutingController> routing;
    std::unique_ptr<app::lifecycle::QuitGuard> quitGuard;
    std::unique_ptr<app::lifecycle::FunctionProxyShutdown> proxyShutdown;
    std::unique_ptr<app::lifecycle::GlobalThreadPoolDrain> drain;
    std::unique_ptr<app::lifecycle::ShutdownCoordinator> shutdown;
    std::unique_ptr<app::backup::BackupCoordinator> backups;

    /// Errors the stores raised during bootstrap(), which is main.cpp's
    /// startup-error window.
    QStringList startupErrors;
    /// Errors the stores raised at any other time. main.cpp has no such
    /// channel - after bootstrap the two connections are disconnected and the
    /// pages take over - so this is the journey's stand-in for the page that
    /// would have shown them.
    QStringList storeErrors;
    QStringList routingErrors;
    QStringList shutdownWarnings;
    bool shutdownStarted = false;

  private:
    /// Exactly the connections src/main.cpp makes, minus the ones whose only
    /// endpoint is a widget. Where main.cpp's slot is a widget call the journey
    /// records the message instead, so "the failure is visible" can be asserted
    /// without a window.
    void wire() {
        QObject::connect(profiles.get(), &core::ProfileStore::errorOccurred, profiles.get(),
                         [this](const QString &message) { storeErrors.append(message); });
        QObject::connect(enhancer.get(), &core::ConfigEnhancer::errorOccurred, enhancer.get(),
                         [this](const QString &message) { storeErrors.append(message); });
        QObject::connect(profiles.get(), &core::ProfileStore::runtimeConfigReady,
                         runtimeCoordinator.get(), [this](const QString &path) {
                             runtimeCoordinator->onRuntimeConfigReady(path);
                         });
        QObject::connect(profiles.get(), &core::ProfileStore::currentProfileChanged,
                         runtimeCoordinator.get(),
                         [this] { runtimeCoordinator->scheduleReload(); });
        QObject::connect(profiles.get(), &core::ProfileStore::profileUpdated,
                         runtimeCoordinator.get(),
                         [this](const QString &uid) { runtimeCoordinator->onProfileUpdated(uid); });
        QObject::connect(enhancer.get(), &core::ConfigEnhancer::chainChanged,
                         runtimeCoordinator.get(),
                         [this] { runtimeCoordinator->scheduleReload(); });
        // Saving a preset changes what the SELECTED profile composes to, so it
        // has to reach the running core the same way a chain change does.
        // main.cpp makes this connection (config-r1); without it a preset the
        // user saved takes effect only at the next profile switch or restart,
        // and the subscription-update journey asserts the reload it schedules.
        QObject::connect(profiles.get(), &core::ProfileStore::presetsChanged,
                         runtimeCoordinator.get(),
                         [this] { runtimeCoordinator->scheduleReload(); });

        shutdown->addBusyGate(QStringLiteral("backup"), [this] { return backups->isBusy(); });
        shutdown->addBusyGate(QStringLiteral("profile-runtime"),
                              [this] { return profiles->isRuntimeBusy(); });
        shutdown->addBusyGate(QStringLiteral("profile-files"),
                              [this] { return profiles->isFileBusy(); });
        shutdown->addBusyGate(QStringLiteral("enhancer-files"),
                              [this] { return enhancer->isFileBusy(); });

        shutdown->addQuitAction(QStringLiteral("runtime"),
                                [this] { runtimeCoordinator->beginShutdown(); });
        shutdown->addQuitAction(QStringLiteral("profiles"), [this] { profiles->beginShutdown(); });
        shutdown->addQuitAction(QStringLiteral("enhancer"), [this] { enhancer->beginShutdown(); });
        shutdown->addQuitAction(QStringLiteral("backups"), [this] { backups->cancel(); });
        shutdown->setFinalCleanup([this] { runtimeCoordinator->pruneAllSnapshots(); });
        shutdown->setQuitGuard(quitGuard.get());

        QObject::connect(quitGuard.get(), &app::lifecycle::QuitGuard::quitRequested,
                         shutdown.get(), &app::lifecycle::ShutdownCoordinator::requestQuit);
        QObject::connect(proxyService.get(), &platform::SystemProxyService::shutdownFinished,
                         shutdown.get(),
                         &app::lifecycle::ShutdownCoordinator::onProxyShutdownFinished);
        QObject::connect(backups.get(), &app::backup::BackupCoordinator::busyChanged,
                         shutdown.get(), &app::lifecycle::ShutdownCoordinator::reevaluate);
        QObject::connect(profiles.get(), &core::ProfileStore::runtimeBusyChanged, shutdown.get(),
                         &app::lifecycle::ShutdownCoordinator::reevaluate);
        QObject::connect(profiles.get(), &core::ProfileStore::fileBusyChanged, shutdown.get(),
                         &app::lifecycle::ShutdownCoordinator::reevaluate);
        QObject::connect(profiles.get(), &core::ProfileStore::fileBusyChanged, backups.get(),
                         &app::backup::BackupCoordinator::onFileBusyChanged);
        QObject::connect(enhancer.get(), &core::ConfigEnhancer::fileBusyChanged, shutdown.get(),
                         &app::lifecycle::ShutdownCoordinator::reevaluate);
        QObject::connect(enhancer.get(), &core::ConfigEnhancer::fileBusyChanged, backups.get(),
                         &app::backup::BackupCoordinator::onFileBusyChanged);

        // main.cpp routes these into the status bar and a modal dialog. The
        // journey records them, and a shutdown warning is dismissed explicitly
        // so the "a warning blocks the quit" contract stays observable.
        QObject::connect(shutdown.get(), &app::lifecycle::ShutdownCoordinator::shutdownStarted,
                         shutdown.get(), [this](const QString &) { shutdownStarted = true; });
        QObject::connect(shutdown.get(), &app::lifecycle::ShutdownCoordinator::warningRaised,
                         shutdown.get(),
                         [this](quint64, const QString &title, const QString &message) {
                             shutdownWarnings.append(title + QStringLiteral(": ") + message);
                         });
        QObject::connect(routing.get(), &app::runtime::RoutingController::errorOccurred,
                         routing.get(),
                         [this](const QString &message) { routingErrors.append(message); });
    }
};

}  // namespace workflows
