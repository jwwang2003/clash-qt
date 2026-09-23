#include "app/backup/backup_coordinator.h"

#include <utility>

#include <QCoreApplication>

#include "app/lifecycle/shutdown_ports.h"

namespace app::backup {

namespace cb = core::backend;

namespace {

// The strings keep ui::BackupPage's translation context: they are the same
// user-visible sentences, and an existing translation must not be orphaned by
// the move out of that file.
QString waitingForFilesMessage() {
    return QCoreApplication::translate("ui::BackupPage",
                                       "Waiting for pending profile changes to finish\xE2\x80\xA6");
}

QString stoppingCoreMessage() {
    return QCoreApplication::translate("ui::BackupPage",
                                       "Stopping the core before restoring\xE2\x80\xA6");
}

QString restoringFilesMessage() {
    return QCoreApplication::translate("ui::BackupPage",
                                       "Restoring files and preferences\xE2\x80\xA6");
}

}  // namespace

BackupCoordinator::BackupCoordinator(std::unique_ptr<BackupSession> session,
                                     cb::MihomoBackend &backend, StoreHooks profiles,
                                     StoreHooks enhancer,
                                     const app::lifecycle::ShutdownState *shutdown,
                                     QObject *parent)
    : QObject(parent), session_(std::move(session)), backend_(backend), lifecycle_(backend),
      profiles_(std::move(profiles)), enhancer_(std::move(enhancer)), shutdown_(shutdown) {
    BackupSession::Events events;
    events.busyChanged = [this](bool busy) { emit busyChanged(busy); };
    events.localBusyChanged = [this](bool busy, bool restoring) {
        onLocalBusyChanged(busy, restoring);
    };
    events.operationPreparing = [this] { onOperationPreparing(); };
    events.restorePrepared = [this] { onRestorePrepared(); };
    events.restored = [this] { onRestored(); };
    session_->setEvents(std::move(events));

    // backup_page.cpp:30. Without it the worker runs straight through and the
    // pending-write gate below never gets a chance to hold it.
    session_->requirePreparation(true);

    backend_.addObserver(this);
}

BackupCoordinator::~BackupCoordinator() { backend_.removeObserver(this); }

core::BackupStore *BackupCoordinator::store() const noexcept { return session_->store(); }

bool BackupCoordinator::isBusy() const { return session_->isBusy(); }

void BackupCoordinator::cancel() { session_->cancel(); }

// ------------------------------------------------------------ maintenance

void BackupCoordinator::onLocalBusyChanged(bool busy, bool restoring) {
    (void)restoring;  // only the progress dialog, which stays in the page, cares

    // backup_page.cpp:138-140, with the QCoreApplication property read replaced
    // by an explicit query. A shutdown forces
    // maintenance mode on even when no backup operation is running, so nothing
    // writes to the profile directory while the process is going away.
    const bool maintenance = busy || (shutdown_ != nullptr && shutdown_->isShuttingDown());
    maintenanceActive_ = maintenance;
    if (profiles_.setMaintenanceMode) profiles_.setMaintenanceMode(maintenance);
    if (enhancer_.setMaintenanceMode) enhancer_.setMaintenanceMode(maintenance);

    // backup_page.cpp:149-153. An operation that ended - completed, cancelled
    // or failed - leaves nothing to wait for.
    if (!busy) {
        waitingForCore_ = false;
        waitingForFiles_ = false;
    }
}

// --------------------------------------------------- the pending-write gate

void BackupCoordinator::onOperationPreparing() {
    // backup_page.cpp:160-164.
    waitingForFiles_ = true;
    emit statusMessage(waitingForFilesMessage());
    continueWhenIdle();
}

void BackupCoordinator::onFileBusyChanged() { continueWhenIdle(); }

void BackupCoordinator::continueWhenIdle() {
    // backup_page.cpp:156-158, verbatim in its conditions and their order. Both
    // stores must be idle: a snapshot taken while either is mid-write captures
    // a half-written profile directory.
    if (!waitingForFiles_) return;
    if (profiles_.isFileBusy && profiles_.isFileBusy()) return;
    if (enhancer_.isFileBusy && enhancer_.isFileBusy()) return;
    waitingForFiles_ = false;
    session_->continuePreparation();
}

// ----------------------------------------------------- restore preparation

void BackupCoordinator::onRestorePrepared() {
    // backup_page.cpp:170-179.
    waitingForCore_ = true;
    emit statusMessage(stoppingCoreMessage());
    lifecycle_.stop();

    // The synchronous fast path, and it is not an optimisation: a core that is
    // already Stopped will never deliver a stop event, so without this the
    // restore would wait forever.
    if (waitingForCore_ && lifecycle_.state() == cb::CoreState::Stopped) {
        waitingForCore_ = false;
        session_->continueRestore(true);
    }
}

void BackupCoordinator::coreStopped(cb::Generation generation) noexcept {
    if (!admit(generation)) return;
    // backup_page.cpp:180-185, guarded by waitingForCore_ so an unrelated stop
    // does not resume a restore that was never prepared.
    if (!waitingForCore_) return;
    waitingForCore_ = false;
    emit restoreProgressMessage(restoringFilesMessage());
    session_->continueRestore(true);
}

void BackupCoordinator::onRestored() {
    // backup_page.cpp:186-189. The order is load-bearing: ProfileStore::load()
    // composes against the enhancement chain, so the chain has to be the
    // restored one before the profiles are read back.
    if (enhancer_.reload) enhancer_.reload();
    if (profiles_.reload) profiles_.reload();
}

// ---------------------------------------------------------- generations

void BackupCoordinator::coreStateChanged(cb::Generation generation, cb::CoreState state,
                                         cb::Ownership ownership) noexcept {
    (void)state;
    (void)ownership;
    admit(generation);
}

bool BackupCoordinator::admit(cb::Generation generation) noexcept {
    // The contract's consumer obligation: an event stamped older than the
    // newest already observed is rejected.
    if (cb::isSuperseded(generation, lastObserved_)) return false;
    lastObserved_ = generation;
    return true;
}

}  // namespace app::backup
