// The shipping backend module's one exported symbol.
//
// This translation unit is what turns the existing MihomoBackendImpl into a
// separately built, separately packaged shared library, which is G2's
// requirement and decision D8's option A. It adds no behaviour: the engine is
// still a child process reached over a loopback controller, the module is a
// thin supervisor over that process edge, and no proxy traffic crosses the
// boundary.
//
// It also has NO test-control extension, which is what makes the test-control
// command range answer kNotImplemented in a shipping build.

#include <memory>
#include <type_traits>

#include <utility>

#include "core/backend/backend.h"
#include "core/backend/privileged_core_service.h"
#include "core/component/abi/backend_abi.h"
#include "core/component/abi/module_entry.h"
#include "core/mihomo/mihomo_backend.h"
#include "core/mihomo/module/backend_session.h"
#include "core/mihomo/module/module_export.h"

namespace {

namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;
namespace cb = ::core::backend;

core::module::WrappedBackend makeRealBackend(core::PrivilegedCoreService *service) {
    // The service is the host's, marshalled through the reverse seam. A null
    // one means the host injected none, and MihomoBackendImpl already reports
    // "no privileged service" for that case rather than pretending.
    auto backend = std::make_unique<core::MihomoBackendImpl>(service);
    // Borrowed: the two accessors are only ever called from inside a delivery
    // or a command of the session that owns this unique_ptr, so the raw pointer
    // cannot outlive it.
    core::MihomoBackendImpl *dispatcher = backend.get();
    return core::module::WrappedBackend{
        std::move(backend),
        [dispatcher] { return dispatcher->producedSequence(); },
        [dispatcher] { return dispatcher->deliverySequence(); },
        // The one accessor that is read from OUTSIDE a delivery: a host asks it
        // to decide whether this image may be unmapped, and the session's Close
        // drains against it. Still borrowed from the unique_ptr above, and still
        // never called after that pointer is reset.
        [dispatcher] { return dispatcher->pendingNativeWork(); },
    };
}

}  // namespace

extern "C" CLASHQT_MODULE_EXPORT com::Result clashqt_component_module_entry(
    const abi::ModuleHandshakeRequest *request, abi::ModuleHandshakeResponse *response,
    com::IComponentModule **out) noexcept {
    // No unmap policy is passed, because there is none to choose. runModuleEntry
    // pins the shared libraries this module links - QtCore, QtNetwork,
    // QtWebSockets, QtConcurrent, yaml-cpp - so that unmapping this image later
    // unmaps only this image, and records whether that succeeded. See
    // shared_runtime.h for the crash that made this necessary.
    return core::module::runModuleEntry(request, response, out, abi::kMihomoModuleId,
                                        "clash-qt mihomo backend module (module-r1, backend-r4)",
                                        makeRealBackend, {});
}

// A compile-time check that the exported function matches the published type.
// A signature drift here would be found by a loader at run time, as a crash.
static_assert(std::is_same_v<decltype(&clashqt_component_module_entry), abi::ModuleEntryFn>,
              "the exported entry must have exactly the published signature");
