#include "app/lifecycle/shutdown_coordinator.h"

#include <utility>

#include <QCoreApplication>

#include "app/lifecycle/quit_guard.h"

namespace app::lifecycle {

namespace cb = core::backend;

namespace {

// The literals main.cpp used. The dialog titles were never translated there and
// are not translated here either; the status line was QObject::tr(), so it keeps
// that translation context rather than acquiring this class's - an existing
// translation must not be orphaned by a file move.
constexpr const char *kCoreWarningTitle = "Core Shutdown";
constexpr const char *kProxyWarningTitle = "System Proxy";

QString shutdownStatusMessage() {
    return QCoreApplication::translate("QObject",
                                       "Closing: restoring system proxy and stopping core\xE2\x80\xA6");
}

QString unconfirmedStopMessage(const QString &reason) {
    if (!reason.isEmpty()) return reason;
    // backend-r2 section 6: confirmed == false means cleanup was requested and
    // nothing confirmed the child exited. A missing reason does not make it a
    // success, so it still gets a dialog - with the one sentence that is always
    // true about it.
    return QCoreApplication::translate(
        "QObject", "The core could not confirm that it stopped. It may still be running.");
}

}  // namespace

ShutdownCoordinator::ShutdownCoordinator(cb::MihomoBackend &backend, SystemProxyShutdown &proxy,
                                         TaskDrain &drain, QObject *parent)
    : QObject(parent), backend_(backend), lifecycle_(backend), proxy_(proxy), drain_(drain) {
    poll_.setInterval(kDefaultPollIntervalMs);
    connect(&poll_, &QTimer::timeout, this, &ShutdownCoordinator::reevaluate);
    backend_.addObserver(this);
}

ShutdownCoordinator::~ShutdownCoordinator() {
    // main.cpp:284-289 did this by hand after app.exec() returned, because the
    // shutdown lambdas captured stack locals. Here there are no captures to
    // outlive, only this registration.
    backend_.removeObserver(this);
}

// ---------------------------------------------------------------- composition

void ShutdownCoordinator::setQuitGuard(QuitGuard *guard) noexcept { guard_ = guard; }

void ShutdownCoordinator::addBusyGate(BusyGate *gate) {
    if (gate) gates_.push_back(gate);
}

void ShutdownCoordinator::addBusyGate(const QString &name, std::function<bool()> isBusy) {
    ownedGates_.push_back(std::make_unique<FunctionGate>(name, std::move(isBusy)));
    gates_.push_back(ownedGates_.back().get());
}

void ShutdownCoordinator::addQuitAction(const QString &name, std::function<void()> action) {
    if (action) quitActions_.emplace_back(name, std::move(action));
}

void ShutdownCoordinator::setFinalCleanup(std::function<void()> cleanup) {
    finalCleanup_ = std::move(cleanup);
}

void ShutdownCoordinator::setPollIntervalMs(int ms) { poll_.setInterval(ms); }

// -------------------------------------------------------------------- quit

void ShutdownCoordinator::requestQuit() {
    if (quitting_) return;  // main.cpp:234
    quitting_ = true;

    // The dynamic property ui/backup_page.cpp:137 reads. Kept while that read
    // exists; isShuttingDown() is the query that replaces it.
    if (auto *application = QCoreApplication::instance())
        application->setProperty("shuttingDown", true);

    // main.cpp:237. Deliberately reset AFTER it may already have been set by an
    // earlier, unrelated stop - the quit waits for its own stop, not a previous
    // one.
    coreStopped_ = false;
    lastStopConfirmed_ = true;
    stopRequested_ = false;

    for (const auto &action : quitActions_) action.second();

    emit shutdownStarted(shutdownStatusMessage());

    poll_.start();

    // LAST, exactly as in main.cpp:246. Everything that stops new work from
    // starting has already run, so nothing can be queued behind this.
    proxy_.requestShutdown();
}

void ShutdownCoordinator::onProxyShutdownFinished(bool success, const QString &error) {
    proxyStopped_ = true;
    if (!success) raiseWarning(QString::fromLatin1(kProxyWarningTitle), error);

    // main.cpp:225. The core stop is issued from inside the proxy's completion
    // and nowhere else: the restore of the OS proxy settings strictly precedes
    // it. Issued even when the restore failed, because leaving a managed core
    // running is worse than a failed restore.
    stopRequested_ = true;
    lifecycle_.stop();

    reevaluate();
}

void ShutdownCoordinator::dismissWarning(quint64 id) {
    if (warnings_.remove(id)) reevaluate();
}

void ShutdownCoordinator::raiseWarning(const QString &title, const QString &message) {
    const quint64 id = nextWarningId_++;
    warnings_.insert(id);
    emit warningRaised(id, title, message);
}

// ------------------------------------------------------------ the quit gate

QString ShutdownCoordinator::blockingReasonBeforeCleanup() const {
    // main.cpp:184, in its written order.
    if (!quitting_) return QStringLiteral("not-quitting");
    if (!proxyStopped_) return QStringLiteral("system-proxy");
    if (!coreStopped_) return QStringLiteral("core-stop");
    if (!warnings_.isEmpty()) return QStringLiteral("warnings");
    // main.cpp:185-186, in registration order.
    for (const BusyGate *gate : gates_)
        if (gate->isBusy()) return gate->name();
    return {};
}

QString ShutdownCoordinator::blockingReason() const {
    const QString reason = blockingReasonBeforeCleanup();
    if (!reason.isEmpty()) return reason;
    if (drain_.activeTaskCount() > 0) return QStringLiteral("thread-pool");
    return {};
}

void ShutdownCoordinator::reevaluate() {
    if (approved_) return;
    if (!blockingReasonBeforeCleanup().isEmpty()) return;

    // main.cpp:187-190. One-shot, and sequenced HERE: after every busy gate and
    // before the drain check, because this is what queues the last deletions.
    if (!finalCleanupStarted_) {
        finalCleanupStarted_ = true;
        if (finalCleanup_) finalCleanup_();
    }

    if (drain_.activeTaskCount() > 0) return;  // main.cpp:191
    approve();
}

void ShutdownCoordinator::approve() {
    if (approved_) return;
    approved_ = true;
    if (guard_) guard_->approve();
    poll_.stop();
    // main.cpp:193. Never a direct quit from inside the event filter: the
    // filter is still on the stack when the last gate clears synchronously.
    QTimer::singleShot(0, this, [this] { emit quitApproved(); });
}

// ------------------------------------------------------------ BackendObserver

bool ShutdownCoordinator::admit(cb::Generation generation) noexcept {
    // backend-r3 section 2, the consumer obligation. Safe for a terminal
    // outcome only because of what backend-r3 B1 now requires of the backend: a
    // StopCompleted carries the generation current AFTER every bump its own
    // teardown caused, not the one captured when stop() was submitted.
    //
    // Delivery order alone does not buy this. The unconfirmed path delivers
    // coreFailed before stopCompleted, in order, from one teardown, and the
    // failure bumps -- so a submit-time stamp arrives strictly older than an
    // event already admitted, and the stop is dropped, taking the quit-blocking
    // warning with it. Until B1 this coordinator escaped that only because it
    // does not override coreFailed, so lastObserved_ never saw the higher
    // generation.
    if (cb::isSuperseded(generation, lastObserved_)) return false;
    lastObserved_ = generation;
    return true;
}

void ShutdownCoordinator::coreStateChanged(cb::Generation generation, cb::CoreState state,
                                           cb::Ownership ownership) noexcept {
    (void)state;
    (void)ownership;
    admit(generation);
}

void ShutdownCoordinator::stopCompleted(const cb::StopCompleted &result) noexcept {
    if (!admit(result.generation)) return;

    coreStopped_ = true;  // main.cpp:197, set for any stop, quitting or not
    lastStopConfirmed_ = result.confirmed;

    // main.cpp:198-209 / backend-r2 section 6. An unconfirmed stop is not
    // success: cleanup was requested and the privileged service disconnected
    // before confirming the child exited. It becomes a warning that holds the
    // quit open until the user acknowledges it.
    // A superseded stop is one this teardown abandoned in favour of a later
    // one, not a stop that failed to confirm. Warning on it would hold the quit
    // open for an outcome no longer being waited on.
    if (quitting_ && !result.confirmed && result.status != cb::CompletionStatus::Superseded)
        raiseWarning(QString::fromLatin1(kCoreWarningTitle),
                     unconfirmedStopMessage(result.reason.message));

    reevaluate();
}

}  // namespace app::lifecycle
