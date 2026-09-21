#ifndef CLASHQT_APP_LIFECYCLE_QUIT_GUARD_H
#define CLASHQT_APP_LIFECYCLE_QUIT_GUARD_H

// QuitGuard: the application event filter that holds QEvent::Quit until the
// shutdown sequence has finished. Lifted from src/main.cpp:33-47 unchanged in
// behaviour; the std::function callback became a signal so the coordinator can
// be connected rather than assigned into.
//
// Inventory: PRE-ARCH section 4b group E, lines 34-47 and 177.
//   "the app must not quit until `approved`, and approval is granted exactly
//    once, through singleShot(0, quit) (not a direct quit() from inside the
//    filter)."
// The deferral itself lives in ShutdownCoordinator; this class only holds the
// door shut and reports that someone tried to open it.

#include <QObject>

class QEvent;

namespace app::lifecycle {

class QuitGuard final : public QObject {
    Q_OBJECT

  public:
    explicit QuitGuard(QObject *parent = nullptr);

    // Once approved the filter stops intercepting, permanently. There is no
    // un-approve: the application is on its way out.
    bool isApproved() const noexcept { return approved_; }
    void approve() noexcept { approved_ = true; }

    // Public, as QObject::eventFilter itself is. main.cpp's copy narrowed it to
    // protected, which left no way to assert what the filter does with an event
    // except by letting QCoreApplication::event() act on it - and for
    // QEvent::Quit that means quitting.
    bool eventFilter(QObject *object, QEvent *event) override;

  signals:
    // Emitted for every intercepted QEvent::Quit. Idempotence is the
    // coordinator's obligation (main.cpp:234, `if (quitting) return;`), exactly
    // as it was when this was a std::function.
    void quitRequested();

  private:
    bool approved_ = false;
};

}  // namespace app::lifecycle

#endif  // CLASHQT_APP_LIFECYCLE_QUIT_GUARD_H
