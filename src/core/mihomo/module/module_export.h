#pragma once

// The handshake half of a module's exported entry, written once.
//
// Both the shipping module and the test double export the SAME symbol with the
// same semantics; only the backend factory differs. module-r1 requires the fake
// to go through the same export and codec - "Fake module uses the same
// adapter/marshalling around FakeBackend" - because a double that takes a
// shortcut proves nothing about the path the application uses.

#include "core/component/abi/module_entry.h"
#include "core/component/component_module.h"
#include "core/mihomo/module/backend_module.h"
#include "core/mihomo/module/backend_session.h"
#include "core/mihomo/module/shared_runtime.h"

// Default visibility even under -fvisibility=hidden, which a shared module
// should be built with: exactly one symbol leaves the library.
#if defined(_WIN32)
#define CLASHQT_MODULE_EXPORT __declspec(dllexport)
#else
#define CLASHQT_MODULE_EXPORT __attribute__((visibility("default")))
#endif

namespace core::module {

namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;

/// Performs the handshake and, on success, pins the shared runtime and
/// constructs the module root.
///
/// `response` is filled in on every path that HAS a response to fill - which
/// means every path except one whose response struct is a size this module does
/// not recognise. That size is checked before a single byte is written, on the
/// null-argument paths too: a response smaller than ours belongs to an older
/// host, and writing our shape into it is a buffer overrun dressed up as a
/// diagnostic. `*out` is set to nullptr first on every failure path, and
/// nothing is constructed.
///
/// There is no unmap POLICY parameter. Whether the image may be unmapped is
/// measured here, by pinSharedRuntime(), and handed to the root as a fact.
com::Result runModuleEntry(const abi::ModuleHandshakeRequest *request,
                           abi::ModuleHandshakeResponse *response, com::IComponentModule **out,
                           const com::ModuleId &moduleId, const char *description,
                           BackendFactory factory, CommandExtension extension) noexcept;

/// The same, for a module that has a reason of its own to answer the image
/// question differently from what the pinning pass measured.
///
/// It exists for ONE caller: the test double, which needs to be able to present
/// a module that answers "quiescent, but do not unmap me" so a suite can prove
/// the loader honours that answer instead of assuming it. The shipping entry
/// point calls the overload above and has no way to reach this one - which is
/// the same rule as the test-control command range, for the same reason.
com::Result runModuleEntry(const abi::ModuleHandshakeRequest *request,
                           abi::ModuleHandshakeResponse *response, com::IComponentModule **out,
                           const com::ModuleId &moduleId, const char *description,
                           BackendFactory factory, CommandExtension extension,
                           bool imageMayUnmap) noexcept;

}  // namespace core::module
