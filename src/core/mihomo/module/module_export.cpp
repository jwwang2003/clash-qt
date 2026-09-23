#include "core/mihomo/module/module_export.h"

#include <utility>

#include "core/mihomo/module/backend_module.h"
#include "core/mihomo/module/shared_runtime.h"
#include "integrations/component/marshal/runtime_tag.h"

namespace core::module {
namespace {

/// Whether `response` is a struct this module knows the shape of. Checked
/// BEFORE anything is written into it, on every path including the ones that
/// fail on a null argument: a host built against an older, smaller struct
/// passes a smaller allocation, and filling ours into it writes past the end.
/// A null response is not a size mismatch - it is a caller that wants no
/// diagnostic, which the ABI allows.
bool responseIsWritable(const abi::ModuleHandshakeResponse *response) noexcept {
    return response == nullptr ||
           response->structSize == static_cast<std::uint32_t>(sizeof(abi::ModuleHandshakeResponse));
}

void fillResponse(abi::ModuleHandshakeResponse *response, const com::ModuleId &moduleId,
                  com::Result status) noexcept {
    if (response == nullptr) {
        return;
    }
    response->structSize = static_cast<std::uint32_t>(sizeof(abi::ModuleHandshakeResponse));
    response->abiVersion = abi::kModuleAbiVersion;
    response->targetAbiTag = abi::kTargetAbiTag;
    response->runtimeTag = ::clashqt::integration::marshal::runtimeTag();
    response->interfaceRevision = abi::kBackendInterfaceRevision;
    response->wireRevision = abi::kWireRevision;
    response->moduleId = moduleId;
    response->status = status;
    response->reserved = 0;
}

}  // namespace

com::Result runModuleEntry(const abi::ModuleHandshakeRequest *request,
                           abi::ModuleHandshakeResponse *response, com::IComponentModule **out,
                           const com::ModuleId &moduleId, const char *description,
                           BackendFactory factory, CommandExtension extension) noexcept {
    // Before anything is constructed: keep the shared libraries this module
    // brought into the process mapped, so unmapping this module later unmaps
    // only this module. The answer is recorded, not enforced - a module that
    // could not pin its runtime is still perfectly usable, it just says so when
    // it is asked whether its image may go.
    return runModuleEntry(request, response, out, moduleId, description, std::move(factory),
                          std::move(extension), pinSharedRuntime());
}

com::Result runModuleEntry(const abi::ModuleHandshakeRequest *request,
                           abi::ModuleHandshakeResponse *response, com::IComponentModule **out,
                           const com::ModuleId &moduleId, const char *description,
                           BackendFactory factory, CommandExtension extension,
                           bool imageMayUnmap) noexcept {
    // Null FIRST, on every path, so a caller that ignores the code cannot read
    // an uninitialised pointer - the same rule component-r1 puts on
    // QueryInterface, for the same reason.
    if (out != nullptr) {
        *out = nullptr;
    }

    // The size check comes before EVERY write, including the null-argument
    // refusals below. Writing a diagnostic into a struct we do not know the
    // shape of is the one failure mode a handshake exists to prevent.
    if (!responseIsWritable(response)) {
        return com::kInvalidArgument;
    }

    if (request == nullptr || out == nullptr) {
        fillResponse(response, moduleId, com::kInvalidArgument);
        return com::kInvalidArgument;
    }

    const std::uint64_t moduleRuntimeTag = ::clashqt::integration::marshal::runtimeTag();
    const com::Result compatibility = abi::CheckHandshake(*request, moduleRuntimeTag, moduleId);
    fillResponse(response, moduleId, compatibility);
    if (com::IsFailure(compatibility)) {
        return compatibility;
    }

    BackendModule *module = nullptr;
    try {
        // The factory and the extension are std::functions; copying one can
        // throw, and nothing may unwind out of an extern "C" entry point whose
        // caller was compiled by a different runtime.
        module = new BackendModule(moduleId, description, std::move(factory), std::move(extension),
                                   imageMayUnmap);
    } catch (...) {
        fillResponse(response, moduleId, com::kFail);
        return com::kFail;
    }
    *out = static_cast<com::IComponentModule *>(module);
    return com::kOk;
}

}  // namespace core::module
