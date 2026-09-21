#ifndef CLASHQT_APP_LIFECYCLE_SHUTDOWN_COORDINATOR_H
#define CLASHQT_APP_LIFECYCLE_SHUTDOWN_COORDINATOR_H

// ShutdownCoordinator: the quit gate, lifted out of src/main.cpp.
//
// Inventory: PRE-ARCH section 4b group E (main.cpp:34-47, 177, 183-247, 284-289).
// Every numbered item of that group's acceptance checklist has a named test in
// tests/app/lifecycle/shutdown_coordinator_test.cpp.
//
// WHAT IT REPLACES
//   Eleven stack locals in main() - quitting, proxyStopped, coreStopped,
//   shutdownWarnings, shutdownCleanupStarted, the backups list, three lambdas,
//   a QTimer and a QuitGuard - captured by reference into lambdas that had to
//   be disconnected by hand after app.exec() returned (main.cpp:284-289) so
//   they would not outlive the frame. All of it is member state here, so that
//   teardown is a destructor rather than a comment.
//
// THE SEQUENCE, IN ORDER, AND WHY EACH STEP IS WHERE IT IS
//   requestQuit()                     idempotent; main.cpp:234
//     -> quitting, "shuttingDown" property, coreStopped := false
//     -> the registered quit actions, in order  (beginShutdown, cancelAsync)
//     -> shutdownStarted()              the shell disables its own widgets
//     -> the poll starts
//     -> SystemProxyShutdown::requestShutdown()      LAST
//   onProxyShutdownFinished()
//     -> a failure raises a warning that blocks the quit
//     -> BackendLifecycle::stop()       STRICTLY AFTER the proxy, main.cpp:225
//   stopCompleted()
//     -> confirmed == false raises a warning that blocks the quit; it is NOT
//        success (backend-r2 section 6)
//   reevaluate()                      the finishQuit gate, main.cpp:183-194
//     quitting, proxy stopped, core stopped, no warnings
//     -> every BusyGate, in registration order
//     -> the final cleanup, ONCE
//     -> TaskDrain empty
//     -> QuitGuard::approve(), then singleShot(0) before quitApproved()
//
// THREAD AFFINITY
//   The owning thread, which is the backend's owning thread and the GUI thread.
//
// COMPOSITION ROOT WIRING - the exact replacement for main.cpp:33-47, 175-254
// and 284-289. Construction order, because each line needs the one above it:
//
//   core::MihomoBackendImpl backend;                    // MOD-CORE
//   auto *proxyService = platform::SystemProxyService::instance();
//   app::lifecycle::QuitGuard quitGuard;
//   app::lifecycle::FunctionProxyShutdown proxyShutdown(
//       [proxyService] { proxyService->shutdown(); });
//   app::lifecycle::GlobalThreadPoolDrain drain;
//   app::lifecycle::ShutdownCoordinator shutdown(backend, proxyShutdown, drain);
//   app::backup::BackupCoordinator backups(
//       std::make_unique<app::backup::BackupStoreSession>(profiles->dataDir()),
//       backend, profileHooks, enhancerHooks, &shutdown);
//   app::Context context{..., &backups};                // new field, appended
//   ui::MainWindow window(context);                     // AFTER the coordinators
//
// Gates, in this order - it is the order main.cpp:185-186 evaluates them in:
//   shutdown.addBusyGate("backup",          [&]{ return backups.isBusy(); });
//   shutdown.addBusyGate("profile-runtime", [profiles]{ return profiles->isRuntimeBusy(); });
//   shutdown.addBusyGate("profile-files",   [profiles]{ return profiles->isFileBusy(); });
//   shutdown.addBusyGate("enhancer-files",  [enhancer]{ return enhancer->isFileBusy(); });
//
// Quit actions, in this order - main.cpp:238-241:
//   shutdown.addQuitAction("reload-stop", [reload]{ reload->stop(); });   // MOD-RUNTIME
//   shutdown.addQuitAction("profiles",    [profiles]{ profiles->beginShutdown(); });
//   shutdown.addQuitAction("enhancer",    [enhancer]{ enhancer->beginShutdown(); });
//   shutdown.addQuitAction("backups",     [&backups]{ backups.cancel(); });
//   shutdown.setFinalCleanup([&]{ runtime.pruneSnapshots(true); });       // MOD-RUNTIME
//   shutdown.setQuitGuard(&quitGuard);
//   app.installEventFilter(&quitGuard);
//
// Connections that move out of main():
//   quitGuard.quitRequested                     -> shutdown.requestQuit
//   proxyService->shutdownFinished              -> shutdown.onProxyShutdownFinished
//   backups.busyChanged                         -> shutdown.reevaluate
//   profiles->runtimeBusyChanged                -> shutdown.reevaluate
//   profiles->fileBusyChanged                   -> shutdown.reevaluate  AND
//                                                  backups.onFileBusyChanged
//   enhancer->fileBusyChanged                   -> shutdown.reevaluate  AND
//                                                  backups.onFileBusyChanged
//   shutdown.quitApproved                       -> QApplication::quit
//   shutdown.shutdownStarted(message)           -> window.setEnabled(false),
//       tray.contextMenu()->setEnabled(false), window.statusBar()->showMessage(message)
//   shutdown.warningRaised(id, title, message)  -> a non-modal QMessageBox parented
//       to the window with WA_DeleteOnClose, whose finished() calls
//       shutdown.dismissWarning(id). The coordinator owns no widget.
//
// Deleted from main.cpp: the QuitGuard class (33-47), lines 176-254 in full,
// the widget-tree query at 182, the three disconnects at 287-289 and the
// core/backups/backup_store.h include. Nothing else in main() moves here.

#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include "app/lifecycle/shutdown_ports.h"
#include "core/backend/backend.h"

namespace app::lifecycle {

class QuitGuard;

class ShutdownCoordinator final : public QObject,
                                  public core::backend::BackendObserver,
                                  public ShutdownState {
    Q_OBJECT

  public:
    // The 50 ms poll of main.cpp:230. Three of the gate's conditions
    // (isRuntimeBusy, isFileBusy, activeThreadCount) emit nothing, so a
    // re-evaluation has to be provoked. Signalled sources call reevaluate()
    // directly and do not wait for a tick; the poll is the backstop for the
    // rest, unchanged in period from the code it replaces.
    static constexpr int kDefaultPollIntervalMs = 50;

    // `backend` is used for exactly two things: the BackendLifecycle facet
    // (stop(), state()) and observer registration. The coordinator adds itself
    // as an observer here and removes itself in the destructor, so the
    // observer-lifetime rule of core/backend/observer.h cannot be violated by a
    // careless composition root.
    ShutdownCoordinator(core::backend::MihomoBackend &backend, SystemProxyShutdown &proxy,
                        TaskDrain &drain, QObject *parent = nullptr);
    ~ShutdownCoordinator() override;

    ShutdownCoordinator(const ShutdownCoordinator &) = delete;
    ShutdownCoordinator &operator=(const ShutdownCoordinator &) = delete;

    // ------------------------------------------------------------ composition
    // All of this is wired before the first quit request.

    // The guard whose approval flag this coordinator sets. Optional: without
    // one the coordinator still reports quitApproved(), it simply has no event
    // filter to release.
    void setQuitGuard(QuitGuard *guard) noexcept;

    // Gates are evaluated in the order they are added. The borrowed overload
    // does not take ownership; the predicate overload owns the adapter it
    // builds. Both must be called before the first requestQuit().
    void addBusyGate(BusyGate *gate);
    void addBusyGate(const QString &name, std::function<bool()> isBusy);

    // Run once, in order, at the start of requestQuit(): ProfileStore and
    // ConfigEnhancer beginShutdown(), the backup cancel, the reload timer stop.
    // They stop new work from starting before anything is awaited (PRE-ARCH
    // group E item 6).
    void addQuitAction(const QString &name, std::function<void()> action);

    // The final snapshot prune (main.cpp:187-190), which belongs to
    // app/runtime's snapshot retention and is merely SEQUENCED here. Invoked at
    // most once per process, after every busy gate is clear and before the task
    // drain is checked; the one-shot property is enforced by this class, not by
    // the callback.
    void setFinalCleanup(std::function<void()> cleanup);

    void setPollIntervalMs(int ms);

    // ------------------------------------------------------------ observation

    bool isShuttingDown() const noexcept override { return quitting_; }
    bool isQuitApproved() const noexcept { return approved_; }
    bool isSystemProxyStopped() const noexcept { return proxyStopped_; }
    bool isCoreStopped() const noexcept { return coreStopped_; }
    // False whenever a stop reported confirmed == false. Never collapsed into
    // "the core stopped": backend-r2 section 6 forbids reporting it as success.
    bool wasLastStopConfirmed() const noexcept { return lastStopConfirmed_; }
    bool isStopRequested() const noexcept { return stopRequested_; }
    int pendingWarnings() const noexcept { return warnings_.size(); }
    bool isFinalCleanupDone() const noexcept { return finalCleanupStarted_; }

    // The first condition that blocks the quit, in gate order, or an empty
    // string when nothing does. A busy gate reports its own name. Pure: it
    // never runs the final cleanup.
    QString blockingReason() const;

    // The event sink this coordinator registers with the backend. Exposed for
    // tests that want to deliver events by hand.
    core::backend::BackendObserver &backendObserver() noexcept { return *this; }

    core::backend::Generation lastObservedGeneration() const noexcept { return lastObserved_; }

  public slots:
    // QuitGuard::quitRequested. Idempotent (main.cpp:234).
    void requestQuit();

    // SystemProxyService::shutdownFinished(success, error).
    void onProxyShutdownFinished(bool success, const QString &error);

    // finishQuit. Connected to every signalled busy source - BackupStore's
    // busyChanged, ProfileStore's runtimeBusyChanged and fileBusyChanged,
    // ConfigEnhancer's fileBusyChanged - and driven by the poll for the rest.
    void reevaluate();

    // The shell reports that a warning dialog closed. Idempotent per id, which
    // the bare `--shutdownWarnings` of main.cpp:203 was not.
    void dismissWarning(quint64 id);

  signals:
    // The shell disables the window, greys the tray menu and shows the status
    // message. The coordinator owns no widgets (PRE-ARCH group E item 7).
    void shutdownStarted(const QString &statusMessage);

    // A non-blocking warning the shell must present. The quit stays open until
    // dismissWarning(id) is called with this id.
    void warningRaised(quint64 id, const QString &title, const QString &message);

    // Emitted from a zero-millisecond timer, never synchronously from inside
    // the event filter (PRE-ARCH group E item 1). The composition root connects
    // this to QCoreApplication::quit.
    void quitApproved();

  private:
    // ---- BackendObserver. Only the managed-core lifecycle is consumed here.
    void coreStateChanged(core::backend::Generation generation, core::backend::CoreState state,
                          core::backend::Ownership ownership) noexcept override;
    void stopCompleted(const core::backend::StopCompleted &result) noexcept override;

    // True when `generation` is older than the newest already observed, in
    // which case the event is dropped (backend-r2 section 2's consumer
    // obligation). Advances lastObserved_ otherwise.
    bool admit(core::backend::Generation generation) noexcept;

    QString blockingReasonBeforeCleanup() const;
    void raiseWarning(const QString &title, const QString &message);
    void approve();

    core::backend::MihomoBackend &backend_;
    core::backend::BackendLifecycle &lifecycle_;
    SystemProxyShutdown &proxy_;
    TaskDrain &drain_;
    QuitGuard *guard_ = nullptr;

    std::vector<BusyGate *> gates_;
    std::vector<std::unique_ptr<BusyGate>> ownedGates_;
    std::vector<std::pair<QString, std::function<void()>>> quitActions_;
    std::function<void()> finalCleanup_;

    QTimer poll_;
    QSet<quint64> warnings_;
    quint64 nextWarningId_ = 1;

    bool quitting_ = false;
    bool proxyStopped_ = false;
    bool coreStopped_ = false;
    bool stopRequested_ = false;
    bool lastStopConfirmed_ = true;
    bool finalCleanupStarted_ = false;
    bool approved_ = false;

    core::backend::Generation lastObserved_ = core::backend::Generation::Initial;
};

}  // namespace app::lifecycle

#endif  // CLASHQT_APP_LIFECYCLE_SHUTDOWN_COORDINATOR_H
