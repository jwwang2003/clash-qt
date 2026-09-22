#ifndef CLASHQT_CORE_BACKEND_OBSERVER_H
#define CLASHQT_CORE_BACKEND_OBSERVER_H

// BackendObserver: the event sink, published instead of Qt signals.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r4, sections 2, 7, 9,
// with amendment A2, which is the reason the consumer obligation below is not
// stated the way r1 stated it.
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
//
//    Note what that means for an abort, because r1 got it backwards and A2
//    corrected it: the completions an abort produces are queued BEFORE the
//    event that announces the bump, so they reach the consumer FIRST, while its
//    last-observed generation is still the pre-bump value. Section 2's ordering
//    rule (bump, THEN abort) is still observable - but through the completion's
//    status, not through its arrival order. A backend that bumps first marks
//    each abandoned completion Superseded; one that aborts first delivers the
//    stale reply as live Ok data. That is the difference the contract test
//    asserts.
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
// CONSUMER OBLIGATION - TWO CHECKS, NOT ONE (amendment A2)
//   1. Drop any completion whose `status` is CompletionStatus::Superseded. The
//      BACKEND marks abandoned work; the consumer does not have to deduce it.
//   2. Reject any event whose generation is older than the newest generation
//      already observed (contract section 2).
//
//   Check 2 alone is NOT sufficient, and r1 wrongly implied it was. Completions
//   carry the generation their request was submitted under, but under rule 2
//   above they are delivered before the event that carries the newer
//   generation, so at that moment `lastObserved` is still the older value and
//   the comparison says "current". Check 1 is what catches them; check 2 is the
//   second line of defence, for an event that arrives after a bump the consumer
//   has already seen.
//
//   One exemption, and it is deliberate: the managed core's terminal outcomes
//   (coreReady, coreFailed, stopCompleted) carry the POST-bump generation
//   (amendment A1, restated by B1), so check 2 never fires on them. Uniform
//   filtering would drop the unconfirmed stop that blocks quit.
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
