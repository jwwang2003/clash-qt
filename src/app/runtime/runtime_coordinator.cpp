#include "app/runtime/runtime_coordinator.h"

#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QThreadPool>

namespace app::runtime {

namespace cb = core::backend;

namespace {

// main.cpp:121-125. The GUI thread selects; a pool thread unlinks. A profile
// switch can retire several snapshots at once and each unlink is a filesystem
// round trip.
void removeFile(const QString &path) { QFile::remove(path); }

}  // namespace

RuntimeCoordinator::RuntimeCoordinator(cb::MihomoBackend &backend, ConfigSource &configs,
                                       QObject *parent)
    : QObject(parent), backend_(backend), configs_(configs), reload_(this), remover_(&removeFile) {
    reload_.setSingleShot(true);
    reload_.setInterval(kReloadDebounceMs);
    connect(&reload_, &QTimer::timeout, this, &RuntimeCoordinator::reloadNow);
    backend_.addObserver(this);
}

RuntimeCoordinator::~RuntimeCoordinator() { backend_.removeObserver(this); }

void RuntimeCoordinator::setSnapshotRemover(SnapshotRemover remover) {
    remover_ = remover ? std::move(remover) : SnapshotRemover(&removeFile);
}

// ------------------------------------------------------------- reload gate

bool RuntimeCoordinator::canReload() const {
    // The whole of main.cpp:139-142 and 150-153. `isManagedCoreActive()` is
    // the backend's own publication of
    //     Running || Starting || (Stopping && isRestartPending())
    // so the three-way test is not reassembled from implementation state here
    // and cannot drift from the engine's idea of it.
    return !quitting_ && backend_.isManagedCoreActive();
}

void RuntimeCoordinator::scheduleReload() {
    if (!canReload()) return;
    // Restarting an already-running single-shot timer is what coalesces a burst
    // of triggers into one reload.
    reload_.start();
}

bool RuntimeCoordinator::isReloadScheduled() const { return reload_.isActive(); }

int RuntimeCoordinator::reloadDebounceMs() const { return reload_.interval(); }

void RuntimeCoordinator::onProfileUpdated(const QString &uid) {
    if (uid != configs_.currentSelection()) return;
    scheduleReload();
}

void RuntimeCoordinator::reloadNow() {
    // Re-checked at fire time, not only at schedule time: the core can stop
    // during the debounce window, and a reload must never be started against a
    // core that is genuinely stopped.
    if (!canReload()) return;
    if (configs_.currentSelection().isEmpty()) {
        // main.cpp:144. No selected profile means STOP, not reload.
        backend_.stop();
        Q_EMIT coreStopRequested();
        return;
    }
    configs_.requestRuntimeConfig();
    Q_EMIT reloadRequested();
}

// ------------------------------------------------------------------ launch

void RuntimeCoordinator::onRuntimeConfigReady(const QString &configPath) {
    // main.cpp:131. Tracked before the quit check, deliberately: a snapshot
    // generated while shutting down is still a file that must be cleaned up,
    // even though no core will be started from it.
    if (QFileInfo(configPath).fileName().startsWith(QLatin1String(kSnapshotPrefix)))
        snapshots_.insert(configPath);
    if (quitting_) return;
    backend_.start(configPath, configs_.workDir());
    Q_EMIT coreStartRequested(configPath);
}

bool RuntimeCoordinator::requestAutostart() {
    if (quitting_) return false;
    // main.cpp:281. Evaluated before scheduling, as it was.
    if (configs_.currentSelection().isEmpty()) return false;
    // main.cpp:282. Deferred into the event loop so it runs after the window is
    // shown, rather than during composition.
    QTimer::singleShot(0, this, [this] {
        if (quitting_) return;
        configs_.requestRuntimeConfig();
        Q_EMIT reloadRequested();
    });
    return true;
}

// --------------------------------------------------------------- snapshots

QStringList RuntimeCoordinator::trackedSnapshots() const {
    return QStringList(snapshots_.begin(), snapshots_.end());
}

void RuntimeCoordinator::pruneSnapshots() { prune(false); }

void RuntimeCoordinator::pruneAllSnapshots() {
    finalPruneDone_ = true;
    prune(true);
}

bool RuntimeCoordinator::finalPruneDone() const { return finalPruneDone_; }

void RuntimeCoordinator::prune(bool final) {
    // Asked for every time. The set covers the running core's configuration, a
    // validating candidate's, a pending launch's, and the configuration of
    // every cancelled validation child that has not exited yet. Caching it, or
    // reconstructing it from state the coordinator can see, reintroduces the
    // file-deletion race the contract's section 5.1 names.
    const QStringList active = backend_.activeConfigPaths();
    QStringList obsolete;
    for (auto it = snapshots_.begin(); it != snapshots_.end();) {
        if (final || !active.contains(*it)) {
            obsolete.append(*it);
            it = snapshots_.erase(it);
        } else {
            ++it;
        }
    }
    if (obsolete.isEmpty()) return;
    Q_EMIT snapshotsDiscarded(obsolete);
    QThreadPool::globalInstance()->start([obsolete, remover = remover_] {
        for (const QString &path : obsolete) remover(path);
    });
}

// ------------------------------------------------------- group E handshake

void RuntimeCoordinator::beginShutdown() {
    if (quitting_) return;
    quitting_ = true;
    reload_.stop();
}

bool RuntimeCoordinator::isShuttingDown() const { return quitting_; }

cb::Generation RuntimeCoordinator::lastObservedGeneration() const { return lastObserved_; }

// ---------------------------------------------------------------- observer

void RuntimeCoordinator::coreReady(const cb::Completion &completion,
                                   const cb::Endpoint &endpoint) noexcept {
    if (!admit(completion)) return;
    // main.cpp:111-112: the handoff from managed to attached. It overwrites the
    // discovered attachment the composition root made at startup.
    backend_.attach(endpoint);
    // main.cpp:127-128, in that order.
    prune(false);
}

void RuntimeCoordinator::coreStateChanged(cb::Generation generation, cb::CoreState,
                                          cb::Ownership) noexcept {
    observe(generation);
}

void RuntimeCoordinator::endpointChanged(cb::Generation generation, const cb::Endpoint &,
                                         cb::Ownership) noexcept {
    observe(generation);
}

bool RuntimeCoordinator::admit(const cb::Completion &completion) noexcept {
    // The backend MARKS what an abort invalidated, because a completion queued
    // before the invalidating event cannot be recognised from its generation
    // alone.
    if (completion.status == cb::CompletionStatus::Superseded) return false;
    if (cb::isSuperseded(completion.generation, lastObserved_)) return false;
    observe(completion.generation);
    return completion.isOk();
}

void RuntimeCoordinator::observe(cb::Generation generation) noexcept {
    if (lastObserved_ < generation) lastObserved_ = generation;
}

}  // namespace app::runtime
