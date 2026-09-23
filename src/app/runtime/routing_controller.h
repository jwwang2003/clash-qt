#ifndef CLASHQT_APP_RUNTIME_ROUTING_CONTROLLER_H
#define CLASHQT_APP_RUNTIME_ROUTING_CONTROLLER_H

// RoutingController - one owner for "where does traffic go".
//
// Extracted from src/main.cpp, and from the routing intent that was spread
// across four surfaces with no single owner:
//
//   settings  ui/pages/settings/settings_page.cpp:215-251 (the system proxy)
//   toolbar   ui/shell/routing_controls.cpp                (system proxy, TUN)
//             ui/shell/main_window.cpp:219-225             (the mode box)
//   tray      ui/shell/tray_icon.cpp:93-105
//   hotkeys   ui/shell/main_window.cpp:274-289             (toggle, cycle)
//
// Each surface reached a different object and computed "is it on?" its own way,
// so three of them could disagree, and two of them could report a REQUESTED
// state as though it were the confirmed one. Here there is one confirmed
// answer per control and every surface reads it.
//
// CONFIRMED, NOT REQUESTED. systemProxyEnabled(), tunEnabled() and mode() are
// read-backs. A change in flight leaves them at the last confirmed value and
// raises the matching *Pending() flag; the *Confirmed signals fire only when
// the backend or the OS reported the change actually took. A TUN change whose
// read-back disagrees with the request is not a success: TunChangeCompleted's
// `actual` is a read-back of the controller, never an echo of the request.
//
// WHAT THIS DOES NOT OWN
//   * the owned-proxy restore at quit, and SystemProxyService::shutdown().
//     Those belong to app/lifecycle. The restores here are the
//     normal-operation ones, and SystemProxyService::restoreOwned() is already
//     inert once shutdown has begun, so the two cannot race.
//   * reload scheduling and snapshot retention - RuntimeCoordinator.

#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

#include "core/backend/backend.h"
#include "platform/proxy/system_proxy.h"

namespace platform {
class SystemProxyService;
}

namespace app::runtime {

// main.cpp:256 and main.cpp:269. Both restore paths wait, then RE-VALIDATE.
// Injectable only so a test can compress the five-second one; the defaults are
// the shipped values and are asserted as such.
struct RestoreDelays {
    // The 0 ms hop exists so a restart that immediately re-enters Starting does
    // not strip the proxy between the two states.
    int coreStoppedMs = 0;
    // The grace window for a controller blip.
    int disconnectedMs = 5000;
};

class RoutingController : public QObject, public core::backend::BackendObserver {
    Q_OBJECT

  public:
    static constexpr int kCoreStoppedRestoreDelayMs = 0;
    static constexpr int kDisconnectedRestoreDelayMs = 5000;

    // `backend` and `proxy` must outlive this object; neither is owned.
    //
    // LIFETIME. main.cpp:258 and :263 used `coreProcess` and `client` as the
    // single-shot context objects so that destroying the backend cancelled a
    // pending restore. Here the context object is `this`, so destroying the
    // controller cancels it - which means the controller must be destroyed
    // BEFORE the backend, not after. The composition root owns that ordering;
    // it is stated in the wiring request rather than assumed.
    RoutingController(core::backend::MihomoBackend &backend, platform::SystemProxyService *proxy,
                      RestoreDelays delays = {}, QObject *parent = nullptr);
    ~RoutingController() override;

    RoutingController(const RoutingController &) = delete;
    RoutingController &operator=(const RoutingController &) = delete;

    RestoreDelays delays() const;

    // --------------------------------------------------------- system proxy

    // Where the OS proxy would be pointed. Supplied by whoever knows the
    // core's listening port and the user's bypass list; the controller decides
    // whether a change is allowed and performs it.
    void setProxyTarget(const platform::ProxyConfig &target);
    platform::ProxyConfig proxyTarget() const;

    // The single intent entry point for settings, toolbar and tray.
    void requestSystemProxy(bool enabled);
    // The hotkey (`proxy.toggle`). Inert while unavailable, exactly as the
    // toggle it used to drive was.
    void toggleSystemProxy();

    // CONFIRMED: the OS reports a proxy that matches the current target.
    bool systemProxyEnabled() const;
    bool systemProxyAvailable() const;
    bool systemProxyPending() const;
    void refreshSystemProxy();

    // ------------------------------------------------------------------ TUN

    void requestTun(bool enabled);
    // CONFIRMED: the controller's read-back, never the request.
    bool tunEnabled() const;
    bool tunAvailable() const;
    bool tunPending() const;
    void setTunBlockedReason(const QString &reason);
    QString tunBlockedReason() const;

    // ----------------------------------------------------------------- mode

    static QStringList modes();
    void requestMode(const QString &mode);
    // The hotkey (`mode.cycle`).
    void cycleMode();
    // CONFIRMED: what the controller last reported, not what was requested.
    QString mode() const;
    bool modePending() const;

    // ------------------------------------------------------------ group D

    // How many times restoreOwned() has actually been performed. The
    // re-validation is only observable as a restore that did NOT happen, so
    // this has to be countable.
    int restoreCount() const;

    QString lastError() const;

    core::backend::Generation lastObservedGeneration() const;

  Q_SIGNALS:
    // Any change to a confirmed value, a pending flag or an availability flag.
    // Every surface re-reads the controller on this one signal, which is what
    // makes them agree.
    void routingStateChanged();

    // The controller's OWN error channel. main.cpp:257 emitted this through
    // `client->errorOccurred`, i.e. one object emitting another's signal; the
    // UI is re-pointed at this instead.
    void errorOccurred(const QString &error);

    void systemProxyConfirmed(bool enabled);
    void tunConfirmed(bool enabled);
    void modeConfirmed(const QString &mode);
    void proxyRestored();

  public:
    // ---- core::backend::BackendObserver
    void coreStateChanged(core::backend::Generation generation, core::backend::CoreState state,
                          core::backend::Ownership ownership) noexcept override;
    void connectedChanged(core::backend::Generation generation, bool connected) noexcept override;
    void endpointChanged(core::backend::Generation generation,
                         const core::backend::Endpoint &endpoint,
                         core::backend::Ownership ownership) noexcept override;
    void configReceived(const core::backend::Completion &completion,
                        const core::backend::BaseConfig &config) noexcept override;
    void tunChangeCompleted(const core::backend::TunChangeCompleted &result) noexcept override;
    void modeChanged(const core::backend::Completion &completion,
                     const QString &mode) noexcept override;

  private:
    void restoreProxy();
    void clearControllerState();
    bool admit(const core::backend::Completion &completion) noexcept;
    void observe(core::backend::Generation generation) noexcept;

    core::backend::MihomoBackend &backend_;
    platform::SystemProxyService *proxy_;
    RestoreDelays delays_;

    platform::ProxyConfig target_;
    std::optional<bool> proxyRequest_;

    bool configKnown_ = false;
    bool tunEnabled_ = false;
    bool tunPending_ = false;
    QString tunBlockedReason_;

    QString mode_;
    QString requestedMode_;
    bool modePending_ = false;

    QString lastError_;
    int restores_ = 0;
    core::backend::Generation lastObserved_ = core::backend::Generation::Initial;
};

}  // namespace app::runtime

#endif  // CLASHQT_APP_RUNTIME_ROUTING_CONTROLLER_H
