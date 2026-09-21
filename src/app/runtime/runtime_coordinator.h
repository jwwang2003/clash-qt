#ifndef CLASHQT_APP_RUNTIME_RUNTIME_COORDINATOR_H
#define CLASHQT_APP_RUNTIME_RUNTIME_COORDINATOR_H

// RuntimeCoordinator - reload scheduling and snapshot retention.
//
// Extracted from src/main.cpp groups B (lines 111-112, 129-160, 280-283) and
// C (lines 113-128, 187-190) of the PRE-ARCH section 4b inventory. Those
// behaviours lived in six connect() lambdas over three captured stack locals,
// which is why none of them could be stated, let alone tested.
//
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r2. This is a
// CONSUMER of MihomoBackend; it never reaches into the engine.
//
// WHAT THIS OWNS
//   * the reload gate, as ONE expression in ONE place (canReload);
//   * the 100 ms single-shot reload debounce;
//   * which snapshot files exist and when they may be deleted;
//   * the managed-to-attached endpoint handoff when a core becomes ready.
//
// WHAT THIS DOES NOT OWN
//   * shutdown ordering, the finishQuit gate and the owned-proxy restore at
//     quit. Those are group E, app/lifecycle. The only handshake in this
//     direction is beginShutdown() / pruneAllSnapshots().
//   * proxy restoration during normal operation - RoutingController, group D.

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

#include "app/runtime/config_source.h"
#include "core/backend/backend.h"

namespace app::runtime {

class RuntimeCoordinator : public QObject, public core::backend::BackendObserver {
    Q_OBJECT

  public:
    // main.cpp:137. Repeated triggers inside this window coalesce into one
    // reload; a profile edited character by character regenerates once.
    static constexpr int kReloadDebounceMs = 100;

    // main.cpp:131. Only files the profile store generated are ever tracked,
    // and therefore only they are ever deleted.
    static constexpr char kSnapshotPrefix[] = ".runtime-";

    // The leaf file operation, substituted in tests so the thread the deletion
    // runs on is observable. The DISPATCH is not substitutable: prune always
    // hands the batch to QThreadPool::globalInstance(), because deleting on the
    // GUI thread is the defect this indirection must not be able to hide
    // (inventory group C, point 3). Invoked on a pool thread, so an
    // implementation must be safe to call there.
    using SnapshotRemover = std::function<void(const QString &path)>;

    // `backend` and `configs` must outlive this object. The coordinator
    // registers itself as a backend observer for its lifetime.
    RuntimeCoordinator(core::backend::MihomoBackend &backend, ConfigSource &configs,
                       QObject *parent = nullptr);
    ~RuntimeCoordinator() override;

    RuntimeCoordinator(const RuntimeCoordinator &) = delete;
    RuntimeCoordinator &operator=(const RuntimeCoordinator &) = delete;

    void setSnapshotRemover(SnapshotRemover remover);

    // ----------------------------------------------------- the reload gate

    // main.cpp:138-142 and main.cpp:151-153, which duplicated the three-way
    // liveness test between two lambdas. It is one expression here, and the
    // liveness half is the backend's published predicate
    // (BackendLifecycle::isManagedCoreActive, backend-r2 section 4) rather than
    // a recombination of state() and isRestartPending().
    //
    //   !shutting down && (Running || Starting || (Stopping && restart pending))
    bool canReload() const;

    // The debounced entry point - main.cpp's `reloadManaged` (149-154). Every
    // reload trigger goes through here: currentProfileChanged (155),
    // profileUpdated for the current uid (156-159) and chainChanged (160).
    void scheduleReload();

    bool isReloadScheduled() const;
    int reloadDebounceMs() const;

    // main.cpp:156-159. A profile that is not the selected one must not cause
    // a reload; this is the filter, not the caller's job.
    void onProfileUpdated(const QString &uid);

    // ------------------------------------------------------------- launch

    // main.cpp:129-134, the ProfileStore::runtimeConfigReady handler.
    // Generating the configuration IS the start action: the core is started
    // from here and from nowhere else.
    void onRuntimeConfigReady(const QString &configPath);

    // main.cpp:280-283. Deferred with singleShot(0) so it runs inside the event
    // loop, after the window is shown. Returns false, and schedules nothing,
    // when there is no selected profile or shutdown has begun. The caller keeps
    // the two gates it owns: the --no-autostart flag and startup/startCore.
    bool requestAutostart();

    // ---------------------------------------------------------- snapshots

    // Snapshot paths that are still tracked, in no particular order.
    QStringList trackedSnapshots() const;

    // main.cpp:114-126 with final == false. A snapshot survives while it is in
    // BackendLifecycle::activeConfigPaths(), which deliberately includes the
    // configuration of a CANCELLED validation child that has not exited yet
    // (backend-r2 section 5.1). That set is asked for, never re-derived.
    void pruneSnapshots();

    // main.cpp:187-190, the final prune inside finishQuit. Discards every
    // tracked snapshot regardless of the active set. Idempotent in itself: a
    // second call discards nothing and dispatches nothing, so app/lifecycle
    // does not have to carry a guard for correctness.
    void pruneAllSnapshots();

    bool finalPruneDone() const;

    // --------------------------------------------- handshake with group E

    // main.cpp:237-240. Sets the `quitting` short-circuit that main.cpp
    // applied in three places (132, 139, 150) and stops the reload timer
    // (241). Idempotent.
    void beginShutdown();
    bool isShuttingDown() const;

    // The newest generation this consumer has observed (backend-r2 section 2).
    core::backend::Generation lastObservedGeneration() const;

  Q_SIGNALS:
    // The batch selected for deletion, emitted on the GUI thread BEFORE the
    // removal is dispatched. Selection is the decision worth observing; the
    // unlink is not.
    void snapshotsDiscarded(const QStringList &paths);

    // Diagnostics for the composition root and the tests: which branch a fired
    // reload took (main.cpp:143-147).
    void reloadRequested();
    void coreStopRequested();
    void coreStartRequested(const QString &configPath);

  public:
    // ---- core::backend::BackendObserver
    //
    // main.cpp wired CoreProcess::ready twice: to MihomoClient::setEndpoint
    // (111-112) and to pruneSnapshots(false) (127-128). Both survive here, in
    // that order - the handoff from managed to attached happens before the
    // retention pass that the new endpoint has no bearing on.
    void coreReady(const core::backend::Completion &completion,
                   const core::backend::Endpoint &endpoint) noexcept override;
    void coreStateChanged(core::backend::Generation generation, core::backend::CoreState state,
                          core::backend::Ownership ownership) noexcept override;
    void endpointChanged(core::backend::Generation generation,
                         const core::backend::Endpoint &endpoint,
                         core::backend::Ownership ownership) noexcept override;

  private:
    void reloadNow();
    void prune(bool final);
    // The backend-r2 section 2 consumer obligation. False when the event must
    // be rejected: an explicitly superseded completion, or one stamped older
    // than the newest generation already seen.
    bool admit(const core::backend::Completion &completion) noexcept;
    void observe(core::backend::Generation generation) noexcept;

    core::backend::MihomoBackend &backend_;
    ConfigSource &configs_;
    QTimer reload_;
    QSet<QString> snapshots_;
    SnapshotRemover remover_;
    core::backend::Generation lastObserved_ = core::backend::Generation::Initial;
    bool quitting_ = false;
    bool finalPruneDone_ = false;
};

}  // namespace app::runtime

#endif  // CLASHQT_APP_RUNTIME_RUNTIME_COORDINATOR_H
