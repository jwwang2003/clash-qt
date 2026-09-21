#ifndef CLASHQT_CORE_BACKEND_OBSERVER_H
#define CLASHQT_CORE_BACKEND_OBSERVER_H

// BackendObserver: the event sink, published instead of Qt signals.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r1, sections 2, 7, 9.
//
// DELIVERY RULES - all four are contract obligations on the BACKEND.
//
// 1. No re-entrant delivery. The backend must not invoke an observer from
//    inside a mutating call. Every event produced by a mutating call is
//    delivered AFTER that call has returned, on the owning thread.
//
//    This is a deliberate CHANGE from current behaviour, not a transcription of
//    it: MihomoClient::clearLiveState() emits five signals synchronously from
//    within setEndpoint/setConnected, so today a handler can re-enter the object
//    mid-mutation. A sink interface would inherit that hazard unchanged.
//
// 2. Order is preserved. Events are delivered in the order they were produced.
//    That is what makes the section 2 ordering rule observable: the generation
//    bump reaches the consumer before the completions the abort produced.
//
// 3. Removal during delivery is safe. An observer may remove itself, or any
//    other observer, from inside a callback. A removed observer receives no
//    further callbacks, including ones already produced and still queued.
//    Adding an observer during delivery is also safe; it sees only events
//    produced after it was added.
//
// 4. Nothing unwinds. Every callback is noexcept. An observer that lets an
//    exception escape terminates the process - it does not corrupt the
//    backend's queue.
//
// THREAD AFFINITY
//   Every callback is invoked on the backend's owning thread - the thread that
//   created it - whatever thread the underlying transport used.
//
// LIFETIME
//   The backend does not own an observer and never extends its lifetime. An
//   observer must be removed before it is destroyed.
//   Every reference and Span parameter is BORROWED for the duration of the
//   call. An observer that needs the data afterwards copies it.
//
// CONSUMER OBLIGATION
//   Reject any event whose generation is older than the newest generation
//   already observed (contract section 2). Completions carry the generation
//   their request was submitted under, so a completion for work a newer
//   generation invalidated compares older and is dropped.
//
// Every method has an empty default body so a consumer overrides only what it
// uses. A completion callback whose `completion.status` is not Ok carries no
// meaningful payload; the payload parameters are then empty, not stale.

#include <QString>

#include "core/backend/capabilities.h"
#include "core/backend/lifecycle.h"
#include "core/backend/types.h"

namespace core::backend {

class BackendObserver {
  public:
    virtual ~BackendObserver() = default;

    // ------------------------------------------------------- managed core

    // `ownership` is reported on every state event (contract section 1).
    virtual void coreStateChanged(Generation generation, CoreState state,
                                  Ownership ownership) noexcept {
        (void)generation;
        (void)state;
        (void)ownership;
    }

    // The managed core answered GET /version with 200 and a string `version`.
    // Not "the process started".
    virtual void coreReady(const Completion &completion, const Endpoint &endpoint) noexcept {
        (void)completion;
        (void)endpoint;
    }

    // One line of the managed core's output. Refreshes the idle deadline while
    // the core is Starting - that is why a core that keeps logging keeps its
    // deadline alive.
    virtual void coreLogLine(Generation generation, const QString &line) noexcept {
        (void)generation;
        (void)line;
    }

    // The managed core failed. completion.request is the start request that
    // failed, or RequestId::Invalid when the core died unprompted or a
    // validation failed with no launch in progress. A validation failure while
    // a core is Running arrives here WITHOUT a state change: the running
    // configuration stays intact (contract section 3).
    virtual void coreFailed(const Completion &completion) noexcept { (void)completion; }

    // The managed core exited cleanly. Emitted alongside stopCompleted() on the
    // clean path; consumers that only care about the terminal answer to stop()
    // should use stopCompleted().
    virtual void coreStopped(Generation generation) noexcept { (void)generation; }

    // Terminal response to stop(). confirmed == false is NOT success.
    virtual void stopCompleted(const StopCompleted &result) noexcept { (void)result; }

    // ------------------------------------------------------- attachment

    virtual void endpointChanged(Generation generation, const Endpoint &endpoint,
                                 Ownership ownership) noexcept {
        (void)generation;
        (void)endpoint;
        (void)ownership;
    }
    virtual void connectedChanged(Generation generation, bool connected) noexcept {
        (void)generation;
        (void)connected;
    }
    virtual void configReceived(const Completion &completion, const BaseConfig &config) noexcept {
        (void)completion;
        (void)config;
    }

    // ------------------------------------------------------- control

    virtual void modeChanged(const Completion &completion, const QString &mode) noexcept {
        (void)completion;
        (void)mode;
    }
    virtual void tunChangeCompleted(const TunChangeCompleted &result) noexcept { (void)result; }
    virtual void nodeSelected(const Completion &completion, const QString &group,
                              const QString &node) noexcept {
        (void)completion;
        (void)group;
        (void)node;
    }
    virtual void geoDatabasesUpdated(const Completion &completion) noexcept { (void)completion; }
    // `resultJson` is the controller's response body as text.
    virtual void dnsQueryFinished(const Completion &completion, const QString &name,
                                  const QString &resultJson) noexcept {
        (void)completion;
        (void)name;
        (void)resultJson;
    }
    virtual void dnsCacheFlushed(const Completion &completion, bool fakeIp) noexcept {
        (void)completion;
        (void)fakeIp;
    }

    // ------------------------------------------------------- telemetry

    virtual void versionReceived(const Completion &completion, const QString &version) noexcept {
        (void)completion;
        (void)version;
    }

    // `nodes` is keyed by ProxyNode::name; it is a span rather than a hash so
    // no container crosses the boundary. Both spans are borrowed for the
    // duration of this call.
    //
    // Also published with two empty spans and completion.request ==
    // RequestId::Invalid when live state is cleared by an endpoint change or a
    // disconnect - the replacement for clearLiveState()'s synchronous emission.
    virtual void proxiesUpdated(const Completion &completion, Span<ProxyGroup> groups,
                                Span<ProxyNode> nodes) noexcept {
        (void)completion;
        (void)groups;
        (void)nodes;
    }
    virtual void rulesUpdated(const Completion &completion, Span<Rule> rules) noexcept {
        (void)completion;
        (void)rules;
    }
    virtual void trafficSample(Generation generation, quint64 up, quint64 down) noexcept {
        (void)generation;
        (void)up;
        (void)down;
    }
    virtual void memorySample(Generation generation, quint64 inuse, quint64 oslimit) noexcept {
        (void)generation;
        (void)inuse;
        (void)oslimit;
    }
    virtual void connectionsUpdated(Generation generation, Span<Connection> connections,
                                    quint64 uploadTotal, quint64 downloadTotal) noexcept {
        (void)generation;
        (void)connections;
        (void)uploadTotal;
        (void)downloadTotal;
    }
    virtual void logReceived(Generation generation, const LogEntry &entry) noexcept {
        (void)generation;
        (void)entry;
    }

    // ------------------------------------------------------- providers

    virtual void providersReceived(const Completion &completion, bool rules,
                                   Span<Provider> providers) noexcept {
        (void)completion;
        (void)rules;
        (void)providers;
    }
    virtual void providerBusyChanged(Generation generation, bool busy) noexcept {
        (void)generation;
        (void)busy;
    }
    virtual void providerOperationFinished(const Completion &completion,
                                           const QString &message) noexcept {
        (void)completion;
        (void)message;
    }

    // ------------------------------------------------------- capabilities

    virtual void privilegedServiceStatus(const Completion &completion,
                                         const PrivilegedServiceStatus &status) noexcept {
        (void)completion;
        (void)status;
    }

    // ------------------------------------------------------- errors

    // The global error channel. completion.request names the operation that
    // failed, or RequestId::Invalid for a failure that belongs to no request.
    virtual void errorOccurred(const Completion &completion) noexcept { (void)completion; }
};

}  // namespace core::backend

#endif  // CLASHQT_CORE_BACKEND_OBSERVER_H
