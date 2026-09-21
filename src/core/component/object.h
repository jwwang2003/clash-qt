#ifndef CLASHQT_CORE_COMPONENT_OBJECT_H
#define CLASHQT_CORE_COMPONENT_OBJECT_H

// IObject, the base of every component interface, and the id lookup that binds a
// C++ interface type to its published identifier.
// Contract: .refactor/COMPONENT_CONTRACT.md revision component-r1.

#include <cstdint>

#include "core/component/interface_id.h"
#include "core/component/result.h"

namespace clashqt::com {

// Specialised for every interface by CLASHQT_COM_DECLARE_INTERFACE_ID. The
// primary template is deliberately incomplete: querying for a type that has no
// published id is a compile error, not a zero id.
template <typename T>
struct InterfaceTraits;

template <typename T>
constexpr const InterfaceId& InterfaceIdOf() noexcept {
    return InterfaceTraits<T>::kId;
}

// The base every component interface derives from.
//
// Thread affinity
//   AddRef and Release are atomic and need no external synchronisation; they may
//   be called from any thread. That is a statement about the reference count
//   ONLY. It says nothing about QueryInterface or about any method of a derived
//   interface: each interface documents its own affinity separately, and where
//   an interface says nothing, its calls belong to the thread that created the
//   object.
//
// Lifetime
//   Release reaching zero destroys the object and frees its storage inside the
//   module that constructed it. A consumer never deletes an interface pointer
//   and never frees the storage with its own allocator. The destructor here is
//   protected and non-virtual for that reason: `delete` on an IObject* does not
//   compile. The vtable is exactly the three methods below.
struct IObject {
    // Retrieves another interface on this object.
    //   out == nullptr            -> kInvalidArgument. Nothing is written and the
    //                                reference count is not touched.
    //   id is not supported       -> *out is set to nullptr FIRST, then
    //                                kNoInterface is returned, so a caller that
    //                                ignores the code cannot read an
    //                                uninitialised pointer. The count is
    //                                unchanged.
    //   id is supported           -> *out receives a non-null pointer, exactly
    //                                one new strong reference is taken and kOk is
    //                                returned. The caller owns that reference and
    //                                must Release it.
    // Querying kIObjectId on any interface of one object yields the same pointer
    // value: that pointer is the object's identity. Queries are reflexive,
    // symmetric and stable for the object's lifetime - an id that succeeds once
    // succeeds while the object lives, and one that fails always fails.
    virtual Result QueryInterface(const InterfaceId& id, void** out) noexcept = 0;

    // Atomic. Returns the count after the increment; the value is a debugging
    // aid, not a synchronisation primitive.
    virtual std::int32_t AddRef() noexcept = 0;

    // Atomic. Returns the count after the decrement. Only a returned 0 is
    // reliable, and it means the object was destroyed; calling any method on the
    // pointer afterwards is undefined.
    virtual std::int32_t Release() noexcept = 0;

  protected:
    ~IObject() = default;
};

}  // namespace clashqt::com

// Binds an interface type to its published id. Use at global scope with a fully
// qualified type name:
//   CLASHQT_COM_DECLARE_INTERFACE_ID(my::IThing, "....-....-....-....-............")
#define CLASHQT_COM_DECLARE_INTERFACE_ID(Type, Literal)                     \
    namespace clashqt::com {                                                \
    template <>                                                             \
    struct InterfaceTraits<Type> {                                          \
        static constexpr ::clashqt::com::InterfaceId kId =                  \
            ::clashqt::com::MakeInterfaceId(Literal);                       \
    };                                                                      \
    }

CLASHQT_COM_DECLARE_INTERFACE_ID(::clashqt::com::IObject,
                                 "247a1b90-ece9-43a9-ab82-5739bdff6445")

namespace clashqt::com {

// Convenience for the id of the base interface.
inline constexpr const InterfaceId& kIObjectId = InterfaceTraits<IObject>::kId;

// Ids 5fd359f7-579a-4bcf-9743-cbbb8c5da432 (IWeakReference) and
// 558f7604-facb-4122-86a8-16f66d58eac2 (IWeakSource) are reserved by the
// contract and deliberately NOT declared here: r1 does not build weak
// references, and reserving an id promises nothing.

}  // namespace clashqt::com

#endif  // CLASHQT_CORE_COMPONENT_OBJECT_H
