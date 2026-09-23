#ifndef CLASHQT_APP_BACKUP_BACKUP_SESSION_H
#define CLASHQT_APP_BACKUP_BACKUP_SESSION_H

// BackupSession: core::BackupStore as BackupCoordinator consumes it.
//
// The coordinator drives the store through this port so that every step of a
// restore - the pending-write gate, the core stop, the resumption - can be held
// and released by a test without a thread, a file or a clock. The production
// implementation is BackupStoreSession, which constructs and owns the real
// core::BackupStore.

#include <functional>

namespace core {
class BackupStore;
}

namespace app::backup {

class BackupSession {
  public:
    // The store's outbound signals, as plain callbacks. Installed once by the
    // coordinator; every one is invoked on the owning thread.
    struct Events {
        // BackupStore::busyChanged - the shutdown busy gate.
        std::function<void(bool busy)> busyChanged;
        // BackupStore::localBusyChanged - drives the maintenance gate.
        std::function<void(bool busy, bool restoring)> localBusyChanged;
        // BackupStore::operationPreparing - the pending-write gate opens.
        std::function<void()> operationPreparing;
        // BackupStore::restorePrepared - the archive is validated and the core
        // must stop before anything is written.
        std::function<void()> restorePrepared;
        // BackupStore::restored - the files are back; reload the stores.
        std::function<void()> restored;
    };

    virtual ~BackupSession() = default;

    virtual void setEvents(Events events) = 0;

    virtual bool isBusy() const = 0;
    virtual void cancel() = 0;
    virtual void requirePreparation(bool required) = 0;
    virtual void continuePreparation() = 0;
    virtual void continueRestore(bool approved) = 0;

    // The store the UI binds its widgets to. Null for a test double: the page
    // is the only thing that needs the concrete object, and a test that drives
    // the session directly has no page.
    virtual core::BackupStore *store() const noexcept = 0;
};

}  // namespace app::backup

#endif  // CLASHQT_APP_BACKUP_BACKUP_SESSION_H
