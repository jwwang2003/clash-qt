#ifndef CLASHQT_CORE_COMPONENT_BUFFER_H
#define CLASHQT_CORE_COMPONENT_BUFFER_H

// IBuffer: the owned byte range used wherever data crosses a module boundary.
// Contract: docs/module-api.md revision component-r1.

#include <cstddef>

#include "core/component/object.h"
#include "core/component/result.h"

namespace clashqt::com {

// A reference-counted, resizable byte range.
//
// The PRODUCING module owns and frees the storage; a consumer holds a reference
// and releases it. That is why no std::string, std::vector, QByteArray or
// QString ever appears in a published signature: a container allocated by one
// module and freed by another is a crash waiting for a different compiler,
// runtime or allocator.
//
// Thread affinity
//   Not free-threaded. AddRef and Release are atomic (IObject says so), but the
//   bytes are not: Size, Data and MutableData may be called concurrently only
//   while no thread is inside Resize, and a writer through MutableData must be
//   synchronised with every reader by the caller. Calls otherwise belong to the
//   thread that obtained the buffer.
//
// Pointer validity
//   Pointers returned by Data and MutableData are invalidated by Resize and by
//   releasing the last reference. They are never invalidated by an unrelated
//   call on another object.
struct IBuffer : IObject {
    // Bytes currently addressable through Data / MutableData.
    virtual std::size_t Size() noexcept = 0;

    // Read pointer. May be nullptr when Size() is 0; never nullptr otherwise.
    virtual const void* Data() noexcept = 0;

    // Writable pointer, or nullptr when the buffer is read-only. A read-only
    // buffer is a normal thing, not a failure.
    virtual void* MutableData() noexcept = 0;

    // Changes the size, preserving min(old, new) leading bytes.
    //   kOk             the buffer now holds exactly newSize bytes
    //   kFail           the allocation failed; the buffer is unchanged
    //   kInvalidState   the buffer is read-only or fixed-size; unchanged
    // Any success invalidates previously returned pointers.
    virtual Result Resize(std::size_t newSize) noexcept = 0;

  protected:
    ~IBuffer() = default;
};

}  // namespace clashqt::com

CLASHQT_COM_DECLARE_INTERFACE_ID(::clashqt::com::IBuffer,
                                 "8fdacf5e-be9a-47e0-bb57-1375a2322e54")

namespace clashqt::com {
inline constexpr const InterfaceId& kIBufferId = InterfaceTraits<IBuffer>::kId;
}

#endif  // CLASHQT_CORE_COMPONENT_BUFFER_H
