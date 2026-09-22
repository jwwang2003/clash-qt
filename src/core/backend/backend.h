#ifndef CLASHQT_CORE_BACKEND_BACKEND_H
#define CLASHQT_CORE_BACKEND_BACKEND_H

// MihomoBackend: the asynchronous facade the application talks to.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r4.
//
// One object exposing five facets. A consumer that needs only part of the
// surface takes a reference to the facet it needs - MOD-RUNTIME can hold a
// BackendControl &, MOD-LIFECYCLE a BackendLifecycle & - and cannot reach the
// rest. The facets are separate base classes rather than accessor methods
// returning pointers, so nothing hands out a reference into the component and
// no facet can be deleted (each has a protected, non-virtual destructor).
//
// LIFETIME
//   The facade's destructor is public and virtual: the host creates and deletes
//   this in-process facade. Per D8, the module-backed implementation is a host
//   shim that holds COM references; module allocations are released inside the
//   module. This Qt interface itself never crosses the binary boundary.
//
// THREAD AFFINITY
//   Every method of every facet, and addObserver/removeObserver here, belongs
//   to the thread that created the backend. Observer callbacks are delivered on
//   that same thread, and never from inside a mutating call.
//
// EXCEPTIONS
//   None crosses this interface.

#include "core/backend/attachment.h"
#include "core/backend/capabilities.h"
#include "core/backend/control.h"
#include "core/backend/lifecycle.h"
#include "core/backend/observer.h"
#include "core/backend/telemetry.h"
#include "core/backend/types.h"

namespace core::backend {

class MihomoBackend : public BackendLifecycle,
                      public BackendAttachment,
                      public BackendControl,
                      public BackendTelemetry,
                      public BackendCapabilities {
  public:
    virtual ~MihomoBackend() = default;

    // The generation current right now. Bumped by any event that invalidates
    // outstanding work: endpoint change, disconnect, managed start, managed
    // stop, failure. Monotonic for the life of the backend.
    //
    // A consumer does not need to poll this: every event carries the value it
    // needs. It is published for assertions and for a consumer rebuilding its
    // own state after an out-of-band change.
    virtual Generation generation() const noexcept = 0;

    // Registration is explicit; the backend does not own the observer.
    // Returns false for a null pointer or one already registered. An observer
    // added during delivery sees only events produced after it was added.
    virtual bool addObserver(BackendObserver *observer) noexcept = 0;

    // Returns false when the observer was not registered. Safe to call from
    // inside a callback, including on the observer being invoked: no further
    // callback reaches it, including events already produced and still queued.
    virtual bool removeObserver(BackendObserver *observer) noexcept = 0;
};

}  // namespace core::backend

#endif  // CLASHQT_CORE_BACKEND_BACKEND_H
