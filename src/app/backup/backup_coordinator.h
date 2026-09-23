#ifndef CLASHQT_APP_BACKUP_BACKUP_COORDINATOR_H
#define CLASHQT_APP_BACKUP_BACKUP_COORDINATOR_H

// BackupCoordinator: owns the backup store and every non-widget rule that used
// to live in ui/backup_page.cpp, so that shutdown no longer depends on a page
// existing.
//
// What moved here, and nothing else:
//   backup_page.cpp:28       construction with ProfileStore::dataDir()
//   backup_page.cpp:30       requirePreparation(true)
//   backup_page.cpp:137-140  the maintenance gate, including the "shuttingDown"
//                            read, which is now an explicit ShutdownState query
//   backup_page.cpp:149-153  the waiting flags reset when the operation ends
//   backup_page.cpp:155-166  continueWhenIdle - the pending-write gate
//   backup_page.cpp:170-179  restorePrepared -> stop the core, with the
//                            synchronous already-stopped fast path
//   backup_page.cpp:180-185  the core stopped -> continueRestore(true)
//   backup_page.cpp:186-189  restored -> enhancer reload, THEN profiles reload
//
// What stays in the page: the progress dialog, the confirmation dialog, the
// status label, the list and the buttons. The page receives the store and binds
// its widgets to it.
//
// What app/lifecycle consumes: isBusy(), cancel() and busyChanged().
//
// THE BACKUP PAGE CHANGE THIS NEEDS (applied by the composition root; this
// package does not edit ui/):
//
//   explicit BackupPage(const app::Context &context,
//                       app::backup::BackupCoordinator &backups,
//                       QWidget *parent = nullptr);
//
//   :29  store_(backups.store())            instead of new core::BackupStore(...)
//   :30  delete - requirePreparation(true) is the coordinator's
//   :137-140 delete the two setMaintenanceMode lines and the `maintenance`
//        computation; KEEP the progress-dialog half of that connection and the
//        :149-153 dialog teardown
//   :155-166 delete continueWhenIdle, the operationPreparing connection and
//        both fileBusyChanged connections
//   :170-185 delete the restorePrepared and CoreProcess::stopped connections
//   :186-189 delete the restored connection
//   add: connect(&backups, &BackupCoordinator::statusMessage, this,
//            [this](const QString &t){ status_->setText(t);
//                                      if (progress_) progress_->setLabelText(t); });
//        connect(&backups, &BackupCoordinator::restoreProgressMessage, this,
//            [this](const QString &t){ if (progress_) progress_->setLabelText(t); });
//   The members waitingForCore_ and waitingForFiles_ leave the page with them.

#include <functional>
#include <memory>

#include <QObject>
#include <QString>

#include "app/backup/backup_session.h"
#include "core/backend/backend.h"

namespace app::lifecycle {
class ShutdownState;
}

namespace core {
class BackupStore;
}

namespace app::backup {

// core::ProfileStore and core::ConfigEnhancer as this coordinator consumes
// them: they present the same three calls, so one shape covers both and the
// composition root binds them without this package linking either library.
struct StoreHooks {
    // ProfileStore::setMaintenanceMode / ConfigEnhancer::setMaintenanceMode
    std::function<void(bool enabled)> setMaintenanceMode;
    // ProfileStore::isFileBusy / ConfigEnhancer::isFileBusy - the pending-write
    // gate reads both; a restore must not overwrite a save that is in flight.
    std::function<bool()> isFileBusy;
    // ProfileStore::load / ConfigEnhancer::load, after a restore.
    std::function<void()> reload;
};

class BackupCoordinator final : public QObject, public core::backend::BackendObserver {
    Q_OBJECT

  public:
    // Takes ownership of `session`; in production that is a BackupStoreSession,
    // which in turn owns the one core::BackupStore in the process.
    // `shutdown` may be null, in which case no shutdown is ever in progress.
    BackupCoordinator(std::unique_ptr<BackupSession> session,
                      core::backend::MihomoBackend &backend, StoreHooks profiles,
                      StoreHooks enhancer, const app::lifecycle::ShutdownState *shutdown = nullptr,
                      QObject *parent = nullptr);
    ~BackupCoordinator() override;

    BackupCoordinator(const BackupCoordinator &) = delete;
    BackupCoordinator &operator=(const BackupCoordinator &) = delete;

    // The store the BackupPage binds to. Never null in production.
    core::BackupStore *store() const noexcept;

    // ---- what app/lifecycle's quit gate consumes (main.cpp:185, 228-229, 241)
    bool isBusy() const;
    void cancel();

    // ---- observation, for tests and diagnostics
    bool isWaitingForFiles() const noexcept { return waitingForFiles_; }
    bool isWaitingForCore() const noexcept { return waitingForCore_; }
    bool isMaintenanceActive() const noexcept { return maintenanceActive_; }

    core::backend::BackendObserver &backendObserver() noexcept { return *this; }

  public slots:
    // ProfileStore::fileBusyChanged and ConfigEnhancer::fileBusyChanged both
    // connect here (backup_page.cpp:165-166).
    void onFileBusyChanged();

  signals:
    // BackupStore::busyChanged, republished so the quit gate does not have to
    // find the store.
    void busyChanged(bool busy);

    // Text for the page's status label - and, while a restore dialog is up, for
    // that dialog too, which is what backup_page.cpp:172-173 did.
    void statusMessage(const QString &message);

    // Text for the restore progress dialog only (backup_page.cpp:183).
    void restoreProgressMessage(const QString &message);

  private:
    // ---- BackendObserver: only the managed core's clean stop is consumed.
    void coreStopped(core::backend::Generation generation) noexcept override;
    void coreStateChanged(core::backend::Generation generation, core::backend::CoreState state,
                          core::backend::Ownership ownership) noexcept override;

    void onLocalBusyChanged(bool busy, bool restoring);
    void onOperationPreparing();
    void onRestorePrepared();
    void onRestored();
    void continueWhenIdle();
    bool admit(core::backend::Generation generation) noexcept;

    std::unique_ptr<BackupSession> session_;
    core::backend::MihomoBackend &backend_;
    core::backend::BackendLifecycle &lifecycle_;
    StoreHooks profiles_;
    StoreHooks enhancer_;
    const app::lifecycle::ShutdownState *shutdown_ = nullptr;

    bool waitingForCore_ = false;
    bool waitingForFiles_ = false;
    bool maintenanceActive_ = false;

    core::backend::Generation lastObserved_ = core::backend::Generation::Initial;
};

}  // namespace app::backup

#endif  // CLASHQT_APP_BACKUP_BACKUP_COORDINATOR_H
