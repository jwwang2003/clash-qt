#ifndef CLASHQT_CORE_COMPONENT_COMPONENT_MODULE_H
#define CLASHQT_CORE_COMPONENT_COMPONENT_MODULE_H

// IComponentModule: the queryable root a loaded module hands back.
// Contract: docs/module-api.md revision component-r1.
//
// The exported C factory entry and the host/module handshake belong to the
// module ABI layer. component-r1 defines only the interface that entry will
// return; nothing here loads, unloads or looks up a shared library.

#include <cstdint>

#include "core/component/interface_id.h"
#include "core/component/object.h"
#include "core/component/result.h"

namespace clashqt::com {

// Thread affinity
//   Free-threaded. A module root is reached by a loader and by consumers on
//   whatever thread needs an object, so an implementation must permit concurrent
//   calls to every method below. Objects it CREATES carry their own affinity,
//   which is declared by their own interfaces, not by this one.
struct IComponentModule : IObject {
    // The module ABI revision, which is the version of the host/module
    // handshake and is distinct from the revision of any individual interface.
    // An interface is versioned by its id; this number is not.
    virtual std::uint32_t AbiVersion() noexcept = 0;

    // Copies out the module's own identity. kInvalidArgument when out is null.
    virtual Result GetModuleId(ModuleId* out) noexcept = 0;

    // A human-readable UTF-8, NUL-terminated description. Owned by the module
    // and valid for as long as the module stays loaded; never freed by the
    // caller. Never null; an empty description is the empty string.
    virtual const char* Description() noexcept = 0;

    // Creates an instance of classId and returns interfaceId on it.
    //   out == nullptr                       kInvalidArgument, nothing written
    //   classId is unknown                   *out = nullptr, then kNotFound
    //   class exists, interfaceId does not   *out = nullptr, then kNoInterface
    //   construction failed                  *out = nullptr, then kFail or a
    //                                        more specific failure code
    //   success                              *out is non-null, holds exactly one
    //                                        strong reference owned by the
    //                                        caller, and kOk is returned
    // The object is destroyed inside this module when its count reaches zero.
    virtual Result CreateObject(const ClassId& classId, const InterfaceId& interfaceId,
                                void** out) noexcept = 0;

  protected:
    ~IComponentModule() = default;
};

}  // namespace clashqt::com

CLASHQT_COM_DECLARE_INTERFACE_ID(::clashqt::com::IComponentModule,
                                 "66fdef80-2cd3-4808-ac33-547172c2952f")

namespace clashqt::com {
inline constexpr const InterfaceId& kIComponentModuleId =
    InterfaceTraits<IComponentModule>::kId;
}

#endif  // CLASHQT_CORE_COMPONENT_COMPONENT_MODULE_H
