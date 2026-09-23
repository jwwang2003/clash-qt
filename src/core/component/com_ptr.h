#ifndef CLASHQT_CORE_COMPONENT_COM_PTR_H
#define CLASHQT_CORE_COMPONENT_COM_PTR_H

// ComPtr<T>: the intrusive owning pointer for component interfaces.
// Contract: docs/module-api.md.
//
// The two ways to take ownership are named, so they cannot be confused at a call
// site:
//   ComPtr<T>::Adopt(p)   takes an ALREADY-OWNED reference and does not AddRef.
//                         This is what QueryInterface and every factory output
//                         feeds.
//   ComPtr<T>::Retain(p)  takes a BORROWED pointer and calls AddRef.
// There is no constructor from a raw pointer and no implicit conversion back to
// one; Get() is explicit about handing out a borrowed pointer.

#include <cstddef>
#include <utility>

#include "core/component/object.h"
#include "core/component/result.h"

namespace clashqt::com {

template <typename T>
class ComPtr {
  public:
    using element_type = T;

    constexpr ComPtr() noexcept = default;
    constexpr ComPtr(std::nullptr_t) noexcept {}

    // Takes an already-owned reference. No AddRef.
    [[nodiscard]] static ComPtr Adopt(T* pointer) noexcept {
        ComPtr owner;
        owner.pointer_ = pointer;
        return owner;
    }

    // Takes a borrowed pointer and adds a reference of its own.
    [[nodiscard]] static ComPtr Retain(T* pointer) noexcept {
        if (pointer != nullptr) {
            pointer->AddRef();
        }
        ComPtr owner;
        owner.pointer_ = pointer;
        return owner;
    }

    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr& other) noexcept : pointer_(other.pointer_) {
        if (pointer_ != nullptr) {
            pointer_->AddRef();
        }
    }

    ComPtr(ComPtr&& other) noexcept : pointer_(other.pointer_) { other.pointer_ = nullptr; }

    // Copy/move from a ComPtr to a type convertible to T (derived -> base).
    template <typename U, typename = decltype(static_cast<T*>(static_cast<U*>(nullptr)))>
    ComPtr(const ComPtr<U>& other) noexcept : pointer_(other.Get()) {
        if (pointer_ != nullptr) {
            pointer_->AddRef();
        }
    }

    template <typename U, typename = decltype(static_cast<T*>(static_cast<U*>(nullptr)))>
    ComPtr(ComPtr<U>&& other) noexcept : pointer_(other.Detach()) {}

    // Acquires the incoming reference BEFORE releasing the outgoing one, so
    // self-assignment cannot destroy the object it is about to keep.
    ComPtr& operator=(const ComPtr& other) noexcept {
        T* incoming = other.pointer_;
        if (incoming != nullptr) {
            incoming->AddRef();
        }
        T* outgoing = pointer_;
        pointer_ = incoming;
        if (outgoing != nullptr) {
            outgoing->Release();
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            T* outgoing = pointer_;
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
            if (outgoing != nullptr) {
                outgoing->Release();
            }
        }
        return *this;
    }

    ComPtr& operator=(std::nullptr_t) noexcept {
        Reset();
        return *this;
    }

    // Releases the held reference, if any, and becomes empty.
    void Reset() noexcept {
        T* outgoing = pointer_;
        pointer_ = nullptr;
        if (outgoing != nullptr) {
            outgoing->Release();
        }
    }

    // Yields ownership to the caller, which then owns the reference.
    [[nodiscard]] T* Detach() noexcept {
        T* outgoing = pointer_;
        pointer_ = nullptr;
        return outgoing;
    }

    // A borrowed pointer. The caller must not Release it.
    [[nodiscard]] T* Get() const noexcept { return pointer_; }

    T* operator->() const noexcept { return pointer_; }
    T& operator*() const noexcept { return *pointer_; }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }

    // Output parameter. Releases any previously held value first, so a pointer
    // cannot leak by being overwritten.
    [[nodiscard]] T** Put() noexcept {
        Reset();
        return &pointer_;
    }
    [[nodiscard]] T** GetAddressOf() noexcept { return Put(); }

    // The same, shaped for QueryInterface/CreateObject's void** out parameter.
    [[nodiscard]] void** PutVoid() noexcept { return reinterpret_cast<void**>(Put()); }

    void Swap(ComPtr& other) noexcept {
        T* mine = pointer_;
        pointer_ = other.pointer_;
        other.pointer_ = mine;
    }

    // Queries the held object for another interface. Returns kInvalidArgument
    // when this pointer is empty; otherwise returns QueryInterface's own Result
    // unchanged, and out is empty on every failure.
    template <typename U>
    Result As(ComPtr<U>& out) const noexcept {
        void** slot = out.PutVoid();
        if (pointer_ == nullptr) {
            return kInvalidArgument;
        }
        return pointer_->QueryInterface(InterfaceIdOf<U>(), slot);
    }

  private:
    T* pointer_ = nullptr;

    template <typename U>
    friend class ComPtr;
};

template <typename T>
bool operator==(const ComPtr<T>& a, const ComPtr<T>& b) noexcept {
    return a.Get() == b.Get();
}
template <typename T>
bool operator!=(const ComPtr<T>& a, const ComPtr<T>& b) noexcept {
    return !(a == b);
}
template <typename T>
bool operator==(const ComPtr<T>& a, std::nullptr_t) noexcept {
    return a.Get() == nullptr;
}
template <typename T>
bool operator!=(const ComPtr<T>& a, std::nullptr_t) noexcept {
    return a.Get() != nullptr;
}

}  // namespace clashqt::com

#endif  // CLASHQT_CORE_COMPONENT_COM_PTR_H
