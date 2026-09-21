#ifndef CLASHQT_CORE_BACKEND_TELEMETRY_H
#define CLASHQT_CORE_BACKEND_TELEMETRY_H

// BackendTelemetry: snapshots, streams and providers.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r1, sections 2, 7, 9.

#include <QString>

#include "core/backend/types.h"

namespace core::backend {

// Thread affinity: the owning thread, as BackendLifecycle. Stream samples are
// delivered on the owning thread too, however the transport is threaded.
class BackendTelemetry {
  public:
    // ---- snapshots. Completion: the matching observer callback, or
    //      observer.errorOccurred() carrying the same RequestId.
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
    // request's id rather than a new one. See the report's answer on
    // ProviderClient::pending_ - this is the behaviour that set implements, and
    // it is not expressible with a per-submission RequestId alone.
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
