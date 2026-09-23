#ifndef CLASHQT_CORE_BACKEND_TELEMETRY_H
#define CLASHQT_CORE_BACKEND_TELEMETRY_H

// BackendTelemetry: snapshots, streams and providers.
// Contract: docs/module-api.md, sections 2, 7, 9.
// One global Generation, with the snapshot re-issue obligation that comes with
// it. The fake backend owes that obligation exactly as the real one does, or
// the two stop satisfying the same contract tests.

#include <QString>

#include "core/backend/types.h"

namespace core::backend {

// Thread affinity: the owning thread, as BackendLifecycle. Stream samples are
// delivered on the owning thread too, however the transport is threaded.
class BackendTelemetry {
  public:
    // ---- snapshots. Completion: the matching observer callback, or
    //      observer.errorOccurred() carrying the same RequestId.
    //
    // THE RE-ISSUE OBLIGATION. ONE global Generation suffices,
    // in place of the four counters the pre-contract code carried, but a single
    // counter means a managed start, stop or failure also invalidates in-flight
    // CONTROLLER replies - which the pre-contract code did not do. /version and
    // /proxies recover through the composition root's 5 s poll; /rules and
    // /configs are issued only from a refresh, so a discarded reply would leave
    // the rules list and BaseConfig stale until the next endpoint change.
    //
    // In place of a second counter: any generation bump that is NOT an endpoint
    // change must re-issue this snapshot set. An endpoint change discharges it
    // by re-fetching on attach; a disconnect, a managed start, a managed stop
    // and a managed failure all owe it. It binds the fake as well as the real
    // backend, because section 10 requires both to satisfy the same contract
    // tests.
    virtual RequestId refreshVersion() noexcept = 0;
    virtual RequestId refreshProxies() noexcept = 0;
    virtual RequestId refreshRules() noexcept = 0;

    // ---- streams.
    //
    // A stream is a subscription, not a request: opening one produces no
    // completion event. Its samples arrive on the observer stamped with the
    // generation current when each sample was produced, and the loss of a
    // stream surfaces as observer.connectedChanged(false), not as a failure of
    // the open. Opening an already-open stream is idempotent.
    //
    // The RequestId identifies the open or close operation itself, so a
    // consumer can correlate a rejection (RequestId::Invalid) with its call.
    virtual RequestId openTrafficStream() noexcept = 0;
    virtual RequestId closeTrafficStream() noexcept = 0;
    virtual RequestId openConnectionsStream() noexcept = 0;
    virtual RequestId closeConnectionsStream() noexcept = 0;
    virtual RequestId openLogStream(const QString &level) noexcept = 0;
    virtual RequestId closeLogStream() noexcept = 0;
    virtual RequestId openMemoryStream() noexcept = 0;
    virtual RequestId closeMemoryStream() noexcept = 0;

    // ---- providers.
    //
    // Deduplicated: a fetch or operation identical to one already outstanding
    // under the current generation is COALESCED onto it and returns that
    // request's id rather than a new one. This is r2's published answer to the
    // first open question: ProviderClient::pending_ does NOT reduce to a
    // RequestId, because it is keyed by (Generation, operation identity) and a
    // duplicate submission issues nothing at all. A per-submission id would
    // mint two ids and issue both.
    // Completion: observer.providersReceived() / observer.providerOperationFinished().
    virtual RequestId fetchProviders(bool rules) noexcept = 0;
    virtual RequestId updateProvider(bool rules, const QString &name) noexcept = 0;
    virtual RequestId healthCheckProvider(const QString &name) noexcept = 0;
    // True while any provider request is outstanding; changes are published on
    // observer.providerBusyChanged().
    virtual bool isProviderBusy() const noexcept = 0;

  protected:
    ~BackendTelemetry() = default;
};

}  // namespace core::backend

#endif  // CLASHQT_CORE_BACKEND_TELEMETRY_H
