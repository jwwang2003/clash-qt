#ifndef CLASHQT_CORE_BACKEND_CONTROL_H
#define CLASHQT_CORE_BACKEND_CONTROL_H

// BackendControl: the mutating operations the application performs against the
// attached controller.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r1, section 2.
//
// Every method here is a mutating call and returns a RequestId synchronously.
// Every one of them can fail; none of them reports failure by throwing.
// MihomoClient::patchConfig(QJsonObject) is deliberately absent: it is public
// today but has no consumer outside mihomo_client.cpp, and the two typed
// operations below replace it.

#include <QString>

#include "core/backend/types.h"

namespace core::backend {

// Thread affinity: the owning thread, as BackendLifecycle.
class BackendControl {
  public:
    // "rule" / "global" / "direct". A string rather than an enum because the
    // controller reports its own mode as a string in BaseConfig::mode and a
    // newer engine may know modes this revision does not.
    // Completion: observer.modeChanged().
    virtual RequestId setMode(const QString &mode) noexcept = 0;

    // CONFIRMED asynchronous change: the completion's `actual` is a read-back
    // of the controller's state, never an echo of `requested`.
    // Completion: observer.tunChangeCompleted().
    // Further calls are rejected (RequestId::Invalid) while one is pending.
    virtual RequestId setTunEnabled(bool enabled) noexcept = 0;
    virtual bool isTunChangePending() const noexcept = 0;

    // Completion: observer.nodeSelected().
    virtual RequestId selectNode(const QString &group, const QString &node) noexcept = 0;
    virtual RequestId resetGroupSelection(const QString &group) noexcept = 0;

    // Completion: observer.proxiesUpdated() with the re-measured delays.
    virtual RequestId testGroupDelay(const QString &group) noexcept = 0;
    virtual RequestId testNodeDelay(const QString &node) noexcept = 0;

    // Observed through the connections stream; no completion event of its own
    // beyond a failure on observer.errorOccurred().
    virtual RequestId closeConnection(const QString &id) noexcept = 0;
    virtual RequestId closeAllConnections() noexcept = 0;

    // Single in-flight. A second call while one is outstanding is rejected.
    // Completion: observer.geoDatabasesUpdated().
    virtual RequestId updateGeoDatabases() noexcept = 0;

    // Completion: observer.dnsQueryFinished(). The result is delivered as the
    // controller's JSON response text, not a QJsonObject: a parsed document
    // cannot cross a module boundary, and the one consumer displays it.
    virtual RequestId queryDns(const QString &name, const QString &type) noexcept = 0;

    // Completion: observer.dnsCacheFlushed().
    virtual RequestId flushDnsCache(bool fakeIp) noexcept = 0;

  protected:
    ~BackendControl() = default;
};

}  // namespace core::backend

#endif  // CLASHQT_CORE_BACKEND_CONTROL_H
