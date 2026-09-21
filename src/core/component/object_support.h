#ifndef CLASHQT_CORE_COMPONENT_OBJECT_SUPPORT_H
#define CLASHQT_CORE_COMPONENT_OBJECT_SUPPORT_H

// Implementation aids for writing an IObject. Nothing here is required to CALL a
// component; it exists so that the QueryInterface rules of
// .refactor/COMPONENT_CONTRACT.md (component-r1) live in one place instead of
// being re-derived, and re-broken, in every implementing class.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/component/interface_id.h"
#include "core/component/object.h"
#include "core/component/result.h"

namespace clashqt::com {

// An atomic strong-reference count that starts at 1, because a freshly
// constructed object is already owned by whoever constructed it.
//
// Atomicity here is about the COUNT only; it confers no thread-safety on any
// method of the object that holds it.
class ReferenceCount {
  public:
    ReferenceCount() noexcept = default;
    explicit ReferenceCount(std::int32_t initial) noexcept : count_(initial) {}

    ReferenceCount(const ReferenceCount&) = delete;
    ReferenceCount& operator=(const ReferenceCount&) = delete;

    std::int32_t Increment() noexcept {
        return count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    // Returns the count after the decrement. A returned 0 means the caller now
    // owns the destruction and must run it; the acq_rel ordering makes every
    // write by every previous owner visible to that destructor.
    std::int32_t Decrement() noexcept {
        return count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    }

    std::int32_t Value() const noexcept { return count_.load(std::memory_order_relaxed); }

  private:
    std::atomic<std::int32_t> count_{1};
};

// One row of an object's interface table: a published id and the pointer that id
// resolves to, already adjusted to the right base under multiple inheritance.
struct InterfaceEntry {
    const InterfaceId* id;
    void* pointer;
};

// Builds a row. Pass an already-cast pointer so the adjustment is explicit and
// visible at the call site:
//   MakeInterfaceEntry(static_cast<IBuffer*>(this))
template <typename T>
constexpr InterfaceEntry MakeInterfaceEntry(T* pointer) noexcept {
    return InterfaceEntry{&InterfaceIdOf<T>(), static_cast<void*>(pointer)};
}

// The whole of the QueryInterface contract, in one implementation:
//   out == nullptr      -> kInvalidArgument, nothing written, count untouched
//   no matching row     -> *out = nullptr FIRST, then kNoInterface, count
//                          untouched
//   matching row        -> *out = the row's pointer, exactly one AddRef on
//                          `identity`, then kOk
// `identity` is the object whose reference count is being taken; under multiple
// inheritance it must be the same canonical branch the kIObjectId row points at,
// so that one object has exactly one count.
Result ResolveInterface(const InterfaceId& id, void** out, const InterfaceEntry* entries,
                        std::size_t count, IObject* identity) noexcept;

}  // namespace clashqt::com

#endif  // CLASHQT_CORE_COMPONENT_OBJECT_SUPPORT_H
