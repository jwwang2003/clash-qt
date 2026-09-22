#ifndef CLASHQT_CORE_COMPONENT_ABI_BACKEND_ABI_H
#define CLASHQT_CORE_COMPONENT_ABI_BACKEND_ABI_H

// The two vtables that carry backend-r4 across a module boundary, plus the
// lifetime interface that makes unloading a module safe rather than forbidden.
// Contract: .refactor/P4_ABI_CONTRACT.md revision module-r1, decision D8.
//
// No Qt type, no standard-library container and no exception crosses anything
// declared here. Payloads are raw bytes in the encoding of wire.h; results the
// module produces are owned IBuffers (component-r1: the PRODUCING module owns
// and frees the storage).
//
// WHAT CROSSES, AND WHAT DELIBERATELY DOES NOT
//   D8's first binding constraint: the module is a thin supervisor over the
//   existing process edge and must NEVER marshal proxy traffic. What crosses is
//   lifecycle (a few calls per session), control (human-rate) and telemetry
//   (bounded by mihomo's emission rate). The data plane stays where it already
//   is - between the engine child process and the network.
//
// THREAD AFFINITY
//   IBackendSession and IBackendHost belong to the thread that created the
//   session, which is the host's Qt thread. backend-r4 gives every one of its
//   methods that affinity and delivers every observer callback on it, so the
//   module runs its wrapped backend on the SAME event loop rather than
//   inventing a second one. AddRef/Release remain callable from any thread,
//   because component-r1 says so of every IObject.
//
// RE-ENTRANCY
//   backend-r4 section 7 forbids an observer callback from inside a mutating
//   call, and D8 records that this guarantee is currently owned by the Qt event
//   loop. It survives here because it is not re-implemented: the module's
//   wrapped backend still queues its events, and the host shim additionally
//   defers any Notify that arrives while it is inside a command. Both halves
//   are asserted by the contract suite.

#include <cstddef>
#include <cstdint>

#include "core/component/abi/wire.h"
#include "core/component/buffer.h"
#include "core/component/error_info.h"
#include "core/component/interface_id.h"
#include "core/component/object.h"
#include "core/component/result.h"

namespace clashqt::com::abi {

// The host's side of the boundary: an event sink and the reverse seam for
// privileged execution. Implemented by the HOST, called by the module.
//
// Lifetime: the module holds a strong reference for as long as the session can
// produce an event. IBackendSession::Close releases it, and the host does not
// destroy its implementation until Close has returned - "Host and callbacks
// outlive pending work" (module-r1).
struct IBackendHost : IObject {
    // One event of wire.h's Event enumeration, behind wire.h's u64 production
    // envelope. `data` is borrowed for the duration of the call; a host that
    // needs it afterwards copies it.
    //   kOk                the event was accepted
    //   kNotImplemented    the host does not know this event code. NORMAL when
    //                      a newer module talks to an older host; never fatal.
    //   kInvalidArgument   the payload did not decode
    virtual Result Notify(std::uint32_t event, const void* data, std::size_t size) noexcept = 0;

    // One operation of wire.h's HostCommand enumeration - the privileged
    // service the host owns and injected. `reply` may be null when the caller
    // wants no reply; otherwise it receives an owned buffer allocated by the
    // HOST, which the module releases.
    //   kNotImplemented    unknown command code
    //   kInvalidState      no privileged service is injected. Distinct from a
    //                      service that answers "unsupported": one is an absent
    //                      collaborator, the other is a platform answer.
    virtual Result Invoke(std::uint32_t command, const void* data, std::size_t size,
                          IBuffer** reply) noexcept = 0;

  protected:
    ~IBackendHost() = default;
};

// The module's side: every operation of backend-r4, as data.
//
// Lifetime and ownership
//   The session is created through IComponentModule::CreateObject and owns
//   whatever backend it wraps. Releasing the last reference destroys both,
//   inside the module. A session that is still open when its last reference
//   goes is closed first: the destructor must not leave a managed core running
//   behind a released object.
struct IBackendSession : IObject {
    // Installs the host. Exactly one host per session:
    //   kOk              accepted; the session took a strong reference
    //   kInvalidArgument host is null
    //   kInvalidState    a host is already installed
    //   kAlreadyClosed   the session is closed
    virtual Result SetHost(IBackendHost* host) noexcept = 0;

    // Performs one command of wire.h's Command enumeration.
    //   `args` is borrowed for the duration of the call.
    //   `reply`, when non-null, receives an owned buffer allocated by the
    //   MODULE; the host releases it. A command with no reply payload writes
    //   nullptr and returns kOk - an absent reply is not a failure.
    //   kNotImplemented  unknown command code, including the test-control range
    //                    in a shipping module
    //   kInvalidArgument the arguments did not decode
    //   kAlreadyClosed   the session is closed; nothing was performed
    // A failure may be accompanied by a diagnostic; see LastError.
    virtual Result Invoke(std::uint32_t command, const void* args, std::size_t size,
                          IBuffer** reply) noexcept = 0;

    // The diagnostic for this session's most recent failure, or kNotFound when
    // there is none. component-r1 forbids thread-local or process-global error
    // state, so the diagnostic is retrieved from the failing object; this is
    // that retrieval for an object whose failures are reported as Results from
    // Invoke rather than through a failed QueryInterface.
    virtual Result LastError(IErrorInfo** out) noexcept = 0;

    // Closes the session: refuse further commands, cancel what is cancellable,
    // stop the managed core when kCloseStopManagedCore is set, and release the
    // host reference once nothing can call back.
    //
    // Returns kOk when the session is quiescent and kTimeout when work is still
    // outstanding after `timeoutMs`. An UNCONFIRMED stop is not success - it is
    // reported through the ordinary stopCompleted event, exactly as backend-r4
    // section 6 requires, and Close does not paper over it.
    //   kAlreadyClosed   already closed. Idempotent, and NOT an error the
    //                    caller has to guard against.
    virtual Result Close(std::uint32_t flags, std::uint32_t timeoutMs) noexcept = 0;

    // Work the session has not finished: pending commands, queued events and
    // buffers the host still holds. Zero means unloading the module cannot
    // strand a callback. Published because "no unload, ever" is not an
    // acceptable substitute for accounting (module-r1).
    virtual std::int32_t OutstandingWork() noexcept = 0;

  protected:
    ~IBackendSession() = default;
};

// Queryable from the module root. The loader uses it to decide whether
// unloading is safe, which is the difference between a module that can be
// replaced and one that can only be leaked.
struct IModuleLifetime : IObject {
    // Objects this module has constructed and the consumer has not yet
    // released, EXCLUDING the module root itself. Unloading with a non-zero
    // count would leave vtable pointers into unmapped memory.
    //
    // It counts EVERY object the module produced, not just sessions: a reply
    // IBuffer or an IErrorInfo the consumer is still holding has its vtable in
    // this image exactly as a session does, and a session-only count would read
    // zero while those were outstanding. The module root itself is excluded
    // because the loader observes it directly - the final Release() of the root
    // tells it whether anyone else still holds one.
    virtual std::int32_t LiveObjectCount() noexcept = 0;

    // Refuses new object creation and reports whether unloading is safe now.
    // The refusal and the answer are ONE atomic transition: a module that
    // checked its count and then latched the refusal separately would still
    // hand out an object between the two, and this interface is free-threaded.
    //   kOk            nothing is alive; the loader may release the root AND
    //                  unmap the image
    //   kFalse         nothing is alive and the root may be released, but the
    //                  image must STAY MAPPED. component-r1 defines kFalse as
    //                  "succeeded, and the answer is negative". It is the
    //                  answer of a module that could not pin the shared
    //                  runtime it brought into the process - see below - and
    //                  it is a MEASURED answer, never a policy
    //   kInvalidState  objects are still alive; the loader must NOT unmap, and
    //                  the module stays usable for their release. NOT latched:
    //                  a consumer may legitimately create more before it gets
    //                  round to unloading
    //   kAlreadyClosed prepared before; still idempotent
    //
    // WHAT ACTUALLY MAKES AN UNMAP UNSAFE, MEASURED RATHER THAN ASSUMED.
    // Unmapping a module unmaps every shared library whose only reference was
    // that module. Those libraries are not inert: QtNetwork, for one, registers
    // a lookup manager with QCoreApplication::destroyed the first time anything
    // resolves a host name, and that registration outlives the image it points
    // into. The first sample consumer segfaulted for exactly that reason - the
    // faulting address was a slot thunk in QtNetwork, which the module's own
    // dlclose had just unmapped, reached from ~QCoreApplication - and NOT
    // because the module had registered meta-objects of its own.
    //
    // So a module pins the shared runtime it loaded BEFORE it constructs
    // anything, and then its own image is genuinely unmappable. kFalse is what
    // it answers when that pinning failed, not a standing policy: "the module
    // may never be unloaded" is exactly the substitute module-r1 rules out.
    virtual Result PrepareUnload() noexcept = 0;

  protected:
    ~IModuleLifetime() = default;
};

// The one class a backend module creates. A module that does not know this id
// answers CreateObject with kNotFound, which is how a loader distinguishes
// "some other component module" from "the backend module".
inline constexpr ClassId kBackendSessionClassId =
    MakeInterfaceId("c131b23b-9d15-4a5d-83bf-a5aefba239a8");

// The shipping module's identity, and the test double's. Both are checked by
// the loader against what the caller asked for, so a fake can never be loaded
// in place of the real module by accident - "no fallback in production".
inline constexpr ModuleId kMihomoModuleId = MakeInterfaceId("75838d74-f2b7-4030-96ad-26d8ff05919d");
inline constexpr ModuleId kFakeModuleId = MakeInterfaceId("fb6a96db-6c97-4aa7-9092-b11b82f0f96c");

}  // namespace clashqt::com::abi

CLASHQT_COM_DECLARE_INTERFACE_ID(::clashqt::com::abi::IBackendHost,
                                 "4b6abab7-235e-4876-9d53-5a96686b91c5")
CLASHQT_COM_DECLARE_INTERFACE_ID(::clashqt::com::abi::IBackendSession,
                                 "296b1610-e991-4136-adbf-4a25c4136ef8")
CLASHQT_COM_DECLARE_INTERFACE_ID(::clashqt::com::abi::IModuleLifetime,
                                 "ddf41729-c7b1-44c7-bf96-e45d0e3f6802")

namespace clashqt::com::abi {
inline constexpr const InterfaceId& kIBackendHostId = InterfaceTraits<IBackendHost>::kId;
inline constexpr const InterfaceId& kIBackendSessionId = InterfaceTraits<IBackendSession>::kId;
inline constexpr const InterfaceId& kIModuleLifetimeId = InterfaceTraits<IModuleLifetime>::kId;
}  // namespace clashqt::com::abi

#endif  // CLASHQT_CORE_COMPONENT_ABI_BACKEND_ABI_H
