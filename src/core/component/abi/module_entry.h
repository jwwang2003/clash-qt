#ifndef CLASHQT_CORE_COMPONENT_ABI_MODULE_ENTRY_H
#define CLASHQT_CORE_COMPONENT_ABI_MODULE_ENTRY_H

// The one exported C symbol of a backend module, and the handshake it performs
// before anything else happens.
// Contract: .refactor/P4_ABI_CONTRACT.md revision module-r1.
//
// WHY THE HANDSHAKE IS A C FUNCTION WITH POD ARGUMENTS
//   It is the only thing in the boundary that runs before the two sides have
//   agreed they are compatible. A C++ vtable, an IObject, a QString or an
//   exception at this point would already be the undefined behaviour the
//   handshake exists to prevent. So: extern "C", fixed-width fields, a
//   size-prefixed struct in each direction, and noexcept.
//
// WHAT IS CHECKED, AND WHY EACH ONE
//   structSize          the caller and the module agree about the shape of the
//                       structs themselves, before any field is read
//   abiVersion          the module ABI revision: the version of THIS handshake
//                       and of wire.h's protocol. Distinct from any interface
//                       id's revision (component-r1) and from backend-r4's
//                       BackendIdentity::interfaceRevision
//   targetAbiTag        compiler, architecture, pointer width, endianness and
//                       C++ ABI flavour, from target_abi.h. A mismatch is not a
//                       version problem, it is a different binary contract
//   runtimeTag          the shared runtime the two sides must agree on and this
//                       header cannot see, because no published ABI header
//                       includes a Qt header. The host computes it from its own
//                       Qt build and the module from its own; module-r1
//                       requires a matching shared Qt, and this is where that
//                       requirement is enforced instead of hoped for
//   interfaceRevision   backend-r4's semantic interface revision
//   moduleId            which module this is. A loader that asked for the
//                       shipping module never accepts the test double
//
// ON FAILURE THE MODULE WRITES NULL AND RETURNS A FAILURE CODE. It does not
// construct anything, and it does not "try to work anyway": there is no
// fallback in production (module-r1).

#include <cstdint>

#include "core/component/abi/target_abi.h"
#include "core/component/abi/wire.h"
#include "core/component/component_module.h"
#include "core/component/interface_id.h"
#include "core/component/result.h"

namespace clashqt::com::abi {

// The module ABI revision. Bumped by any change to the handshake structs, to
// IBackendSession/IBackendHost, or to the MEANING of a wire.h code.
inline constexpr std::uint32_t kModuleAbiVersion = 1;

// The exported symbol. Deliberately verbose and project-scoped: a module is
// found by dlopen/LoadLibrary on a path the loader chose, and the symbol name
// is the second half of "this file is one of ours".
#define CLASHQT_COM_MODULE_ENTRY_NAME "clashqt_component_module_entry"

// What the host tells the module about itself.
struct ModuleHandshakeRequest {
    std::uint32_t structSize;  // sizeof(ModuleHandshakeRequest)
    std::uint32_t abiVersion;
    std::uint64_t targetAbiTag;
    std::uint64_t runtimeTag;
    std::uint32_t interfaceRevision;
    std::uint32_t wireRevision;
    // The module the host intends to load, or the all-zero id for "any backend
    // module". The all-zero id is deliberately NOT a wildcard the shipping
    // loader uses - component-r1 records an all-zero id as a defect of the
    // design reference - it is what a test harness passes when it is loading a
    // module in order to ask what it is.
    ModuleId expectedModuleId;
};

// What the module answers, whether or not it accepts.
struct ModuleHandshakeResponse {
    std::uint32_t structSize;  // sizeof(ModuleHandshakeResponse)
    std::uint32_t abiVersion;
    std::uint64_t targetAbiTag;
    std::uint64_t runtimeTag;
    std::uint32_t interfaceRevision;
    std::uint32_t wireRevision;
    ModuleId moduleId;
    // The first mismatch found, as a Result. kOk when the module accepted.
    Result status;
    std::uint32_t reserved;
};

static_assert(sizeof(ModuleHandshakeRequest) == 48, "handshake request layout is fixed");
static_assert(sizeof(ModuleHandshakeResponse) == 56, "handshake response layout is fixed");

// The exported entry.
//
//   request   what the host is. Null, or a structSize this module does not
//             recognise, is kInvalidArgument.
//   response  filled in ALWAYS when non-null, including on refusal, so the
//             loader can report what the module actually was rather than
//             "incompatible". The CALLER sets response->structSize before the
//             call and zeroes the rest: the module refuses a size it does not
//             recognise instead of writing past the end of a smaller struct
//             allocated by an older host.
//   out       receives one owned reference to the module root on success;
//             nullptr is written FIRST on every failure path.
//
// noexcept in the strongest sense available: the module must not let anything
// unwind out of this function, whatever the C++ runtimes on the two sides are.
using ModuleEntryFn = Result (*)(const ModuleHandshakeRequest* request,
                                 ModuleHandshakeResponse* response,
                                 IComponentModule** out) noexcept;

// Fills a request with everything this translation unit knows about itself.
// `runtimeTag` is supplied because no ABI header may include a Qt header; the
// host and the module each compute it with their own private code.
inline ModuleHandshakeRequest MakeHandshakeRequest(std::uint64_t runtimeTag,
                                                   const ModuleId& expectedModuleId) noexcept {
    ModuleHandshakeRequest request{};
    request.structSize = static_cast<std::uint32_t>(sizeof(ModuleHandshakeRequest));
    request.abiVersion = kModuleAbiVersion;
    request.targetAbiTag = kTargetAbiTag;
    request.runtimeTag = runtimeTag;
    request.interfaceRevision = kBackendInterfaceRevision;
    request.wireRevision = kWireRevision;
    request.expectedModuleId = expectedModuleId;
    return request;
}

// The all-zero id, named rather than spelled out at call sites. It is the
// "ask what you are" value for a harness, never a production wildcard.
inline constexpr ModuleId kAnyModuleId = MakeInterfaceId("00000000-0000-0000-0000-000000000000");

// The single place the compatibility rules are evaluated, so the module and any
// loader that pre-checks apply exactly the same ones in the same order.
// Returns kOk when compatible.
inline Result CheckHandshake(const ModuleHandshakeRequest& request, std::uint64_t moduleRuntimeTag,
                             const ModuleId& moduleId) noexcept {
    if (request.structSize != static_cast<std::uint32_t>(sizeof(ModuleHandshakeRequest))) {
        return kInvalidArgument;
    }
    if (request.abiVersion != kModuleAbiVersion) {
        return kUnsupportedVersion;
    }
    if (request.wireRevision != kWireRevision) {
        return kUnsupportedVersion;
    }
    if (request.interfaceRevision != kBackendInterfaceRevision) {
        return kUnsupportedVersion;
    }
    // A target mismatch is reported as an unsupported version too, but it is a
    // different thing: the two binaries do not share a calling convention or a
    // vtable layout, and no version of either would help.
    if (request.targetAbiTag != kTargetAbiTag) {
        return kUnsupportedVersion;
    }
    if (request.runtimeTag != moduleRuntimeTag) {
        return kUnsupportedVersion;
    }
    if (request.expectedModuleId != kAnyModuleId && request.expectedModuleId != moduleId) {
        return kNotFound;
    }
    return kOk;
}

}  // namespace clashqt::com::abi

#endif  // CLASHQT_CORE_COMPONENT_ABI_MODULE_ENTRY_H
