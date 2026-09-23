// A BackupSession that holds every step of a backup or restore until the test
// releases it. No thread, no archive, no clock: the real core::BackupStore runs
// its work on a QThread and answers a semaphore, which makes "prove the gate
// blocked, then release it" impossible to assert without waiting.
//
// The emission ORDER mirrors core::BackupStore::runAsync exactly
// (backup_store.cpp:202-205): localBusyChanged, then busyChanged, then
// operationPreparing. A fake that reordered them would let a coordinator pass
// here and deadlock in production.
#ifndef CLASHQT_TESTS_APP_BACKUP_FAKE_BACKUP_SESSION_H
#define CLASHQT_TESTS_APP_BACKUP_FAKE_BACKUP_SESSION_H

#include <utility>
#include <vector>

#include "app/backup/backup_session.h"

namespace testsupport::backup {

class FakeBackupSession final : public ::app::backup::BackupSession {
  public:
    // ---- BackupSession
    void setEvents(Events events) override { events_ = std::move(events); }
    bool isBusy() const override { return busy_; }
    void cancel() override { ++cancels; }
    void requirePreparation(bool required) override { preparationRequired = required; }
    void continuePreparation() override { ++preparationsContinued; }
    void continueRestore(bool approved) override { restoreApprovals.push_back(approved); }
    core::BackupStore *store() const noexcept override { return nullptr; }

    // ---- driving
    void beginOperation(bool restoring = false) {
        busy_ = true;
        if (events_.localBusyChanged) events_.localBusyChanged(true, restoring);
        if (events_.busyChanged) events_.busyChanged(true);
        if (preparationRequired && events_.operationPreparing) events_.operationPreparing();
    }
    void finishOperation(bool restoring = false) {
        busy_ = false;
        if (events_.localBusyChanged) events_.localBusyChanged(false, restoring);
        if (events_.busyChanged) events_.busyChanged(false);
    }
    void prepareRestore() {
        if (events_.restorePrepared) events_.restorePrepared();
    }
    void reportRestored() {
        if (events_.restored) events_.restored();
    }

    // ---- what was asked of it
    bool preparationRequired = false;
    int preparationsContinued = 0;
    int cancels = 0;
    std::vector<bool> restoreApprovals;

  private:
    Events events_;
    bool busy_ = false;
};

}  // namespace testsupport::backup

#endif  // CLASHQT_TESTS_APP_BACKUP_FAKE_BACKUP_SESSION_H
