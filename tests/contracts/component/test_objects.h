#ifndef CLASHQT_TESTS_CONTRACTS_COMPONENT_TEST_OBJECTS_H
#define CLASHQT_TESTS_CONTRACTS_COMPONENT_TEST_OBJECTS_H

// Minimal component implementations for the component-r1 contract suite.
//
// Deliberately hand-written rather than generated: these are the objects whose
// behaviour the contract describes, so the boilerplate a real consumer will
// write is exactly what gets tested. Each one counts with clashqt::com::
// ReferenceCount, answers QueryInterface through ResolveInterface, and destroys
// itself - inside this module - when its count reaches zero.
//
// No Qt appears here either, although the suite that drives these objects is a
// QtTest suite: the objects are what crosses a module boundary.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

#include "core/component/component.h"

namespace componenttest {

using clashqt::com::ClassId;
using clashqt::com::InterfaceEntry;
using clashqt::com::InterfaceId;
using clashqt::com::IBuffer;
using clashqt::com::IComponentModule;
using clashqt::com::IErrorInfo;
using clashqt::com::IObject;
using clashqt::com::MakeInterfaceEntry;
using clashqt::com::ReferenceCount;
using clashqt::com::ResolveInterface;
using clashqt::com::Result;

// Counts destructor runs, so a test can prove an object died exactly once.
struct DestructionWitness {
    int destroyed = 0;
};

// Supports IObject and nothing else.
class PlainObject final : public IObject {
  public:
    explicit PlainObject(DestructionWitness* witness = nullptr) noexcept : witness_(witness) {}

    Result QueryInterface(const InterfaceId& id, void** out) noexcept override {
        const InterfaceEntry table[] = {MakeInterfaceEntry(static_cast<IObject*>(this))};
        return ResolveInterface(id, out, table, std::size(table), this);
    }

    std::int32_t AddRef() noexcept override { return count_.Increment(); }

    std::int32_t Release() noexcept override {
        const std::int32_t after = count_.Decrement();
        if (after == 0) {
            delete this;  // destruction happens in the allocating module
        }
        return after;
    }

    // Test-only: observes the count without perturbing it, which is what lets a
    // test assert "the count did not change".
    std::int32_t PeekCount() const noexcept { return count_.Value(); }

  private:
    ~PlainObject() {
        if (witness_ != nullptr) {
            ++witness_->destroyed;
        }
    }

    ReferenceCount count_;
    DestructionWitness* witness_ = nullptr;
};

// A resizable byte range. std::vector is used INSIDE the implementation; it
// never appears in a published signature.
class ByteBuffer final : public IBuffer {
  public:
    ByteBuffer() noexcept = default;

    explicit ByteBuffer(const char* text) {
        const std::size_t length = text != nullptr ? std::strlen(text) : 0;
        bytes_.assign(text, text + length);
    }

    Result QueryInterface(const InterfaceId& id, void** out) noexcept override {
        const InterfaceEntry table[] = {
            MakeInterfaceEntry(static_cast<IObject*>(static_cast<IBuffer*>(this))),
            MakeInterfaceEntry(static_cast<IBuffer*>(this)),
        };
        return ResolveInterface(id, out, table, std::size(table),
                                static_cast<IObject*>(static_cast<IBuffer*>(this)));
    }

    std::int32_t AddRef() noexcept override { return count_.Increment(); }

    std::int32_t Release() noexcept override {
        const std::int32_t after = count_.Decrement();
        if (after == 0) {
            delete this;
        }
        return after;
    }

    std::size_t Size() noexcept override { return bytes_.size(); }
    const void* Data() noexcept override { return bytes_.empty() ? nullptr : bytes_.data(); }
    void* MutableData() noexcept override { return bytes_.empty() ? nullptr : bytes_.data(); }

    Result Resize(std::size_t newSize) noexcept override {
        if (newSize > kMaximumSize) {
            return clashqt::com::kFail;  // a resize that can fail, and does
        }
        bytes_.resize(newSize);
        return clashqt::com::kOk;
    }

    std::int32_t PeekCount() const noexcept { return count_.Value(); }

    static constexpr std::size_t kMaximumSize = 1u << 20;

  private:
    ~ByteBuffer() = default;

    ReferenceCount count_;
    std::vector<char> bytes_;
};

// Multiple inheritance, which is the case that makes interface identity worth
// asserting: the IBuffer and IErrorInfo pointers legitimately differ, while the
// IObject pointer is the one identity of the object. The IBuffer branch is the
// canonical one.
class DualObject final : public IBuffer, public IErrorInfo {
  public:
    explicit DualObject(DestructionWitness* witness = nullptr) noexcept : witness_(witness) {}

    Result QueryInterface(const InterfaceId& id, void** out) noexcept override {
        const InterfaceEntry table[] = {
            MakeInterfaceEntry(Identity()),
            MakeInterfaceEntry(static_cast<IBuffer*>(this)),
            MakeInterfaceEntry(static_cast<IErrorInfo*>(this)),
        };
        return ResolveInterface(id, out, table, std::size(table), Identity());
    }

    std::int32_t AddRef() noexcept override { return count_.Increment(); }

    std::int32_t Release() noexcept override {
        const std::int32_t after = count_.Decrement();
        if (after == 0) {
            delete this;
        }
        return after;
    }

    // IBuffer
    std::size_t Size() noexcept override { return bytes_.size(); }
    const void* Data() noexcept override { return bytes_.empty() ? nullptr : bytes_.data(); }
    void* MutableData() noexcept override { return nullptr; }  // read-only
    Result Resize(std::size_t) noexcept override { return clashqt::com::kInvalidState; }

    // IErrorInfo
    Result GetCode(Result* out) noexcept override {
        if (out == nullptr) {
            return clashqt::com::kInvalidArgument;
        }
        *out = clashqt::com::kTimeout;
        return clashqt::com::kOk;
    }

    Result GetMessage(IBuffer** out) noexcept override {
        if (out == nullptr) {
            return clashqt::com::kInvalidArgument;
        }
        *out = new ByteBuffer("bounded wait expired");  // one reference, caller owns it
        return clashqt::com::kOk;
    }

    Result GetSource(IBuffer** out) noexcept override {
        if (out == nullptr) {
            return clashqt::com::kInvalidArgument;
        }
        *out = nullptr;  // no source tag; absence is normal
        return clashqt::com::kNotFound;
    }

    std::int32_t PeekCount() const noexcept { return count_.Value(); }

    // The canonical branch: one object, one identity, one count.
    IObject* Identity() noexcept { return static_cast<IObject*>(static_cast<IBuffer*>(this)); }

  private:
    ~DualObject() {
        if (witness_ != nullptr) {
            ++witness_->destroyed;
        }
    }

    ReferenceCount count_;
    std::vector<char> bytes_;
    DestructionWitness* witness_ = nullptr;
};

// Test-local class ids. Freshly generated for this suite; they name classes, not
// interfaces, and are not part of the published contract table.
inline constexpr ClassId kPlainObjectClassId =
    clashqt::com::MakeInterfaceId("06763cd2-8102-4ab6-82f1-0227061a6fc6");
inline constexpr ClassId kByteBufferClassId =
    clashqt::com::MakeInterfaceId("e7d62bad-9bbc-4ac5-a7ec-6219860761d5");
inline constexpr ClassId kUnknownClassId =
    clashqt::com::MakeInterfaceId("6d79d048-a6d4-45a3-9d39-0cf46abe0654");

inline constexpr clashqt::com::ModuleId kTestModuleId =
    clashqt::com::MakeInterfaceId("b0f4a7d1-3c2e-4f58-9a6b-7e5d1c8f2a34");

// A module root. r1 defines the interface a loader will later return; this
// object is that interface, reached directly, with no exported C entry and no
// library loading anywhere in sight.
class TestModule final : public IComponentModule {
  public:
    Result QueryInterface(const InterfaceId& id, void** out) noexcept override {
        const InterfaceEntry table[] = {
            MakeInterfaceEntry(static_cast<IObject*>(this)),
            MakeInterfaceEntry(static_cast<IComponentModule*>(this)),
        };
        return ResolveInterface(id, out, table, std::size(table), this);
    }

    std::int32_t AddRef() noexcept override { return count_.Increment(); }

    std::int32_t Release() noexcept override {
        const std::int32_t after = count_.Decrement();
        if (after == 0) {
            delete this;
        }
        return after;
    }

    std::uint32_t AbiVersion() noexcept override { return 1; }

    Result GetModuleId(clashqt::com::ModuleId* out) noexcept override {
        if (out == nullptr) {
            return clashqt::com::kInvalidArgument;
        }
        *out = kTestModuleId;
        return clashqt::com::kOk;
    }

    const char* Description() noexcept override { return "clash-qt component contract test module"; }

    Result CreateObject(const ClassId& classId, const InterfaceId& interfaceId,
                        void** out) noexcept override {
        if (out == nullptr) {
            return clashqt::com::kInvalidArgument;
        }
        *out = nullptr;
        IObject* created = nullptr;
        if (classId == kPlainObjectClassId) {
            created = new PlainObject();
        } else if (classId == kByteBufferClassId) {
            created = static_cast<IObject*>(static_cast<IBuffer*>(new ByteBuffer()));
        } else {
            return clashqt::com::kNotFound;  // the class, not the interface, is missing
        }
        const Result queried = created->QueryInterface(interfaceId, out);
        created->Release();  // the construction reference; the query took its own
        return queried;
    }

  private:
    ~TestModule() = default;

    ReferenceCount count_;
};

}  // namespace componenttest

#endif  // CLASHQT_TESTS_CONTRACTS_COMPONENT_TEST_OBJECTS_H
