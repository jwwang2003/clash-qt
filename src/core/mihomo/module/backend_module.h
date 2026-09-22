#pragma once

// The module root: IComponentModule plus the lifetime accounting a loader
// needs before it may unmap the library.
//
// WHY IModuleLifetime EXISTS. component-r1 froze IComponentModule's vtable, and
// an id names an immutable vtable - so the unload question gets its own
// interface with its own id rather than a method appended to that one. The
// alternative the industry usually settles for is "never unload", and module-r1
// rules it out explicitly: "Actual safe close/drain/lifetime accounting, not
// permanent no-unload as substitute."
//
// THE ACCOUNTING IS EXACT, AND IT COVERS EVERY PRODUCED OBJECT
// Every object this module constructs - sessions, reply buffers, diagnostics,
// and the buffers a diagnostic produces for its own message - takes a reference
// on the module AND increments a counter; releasing it decrements both. A
// loader that sees zero knows no vtable pointer into this library is still
// reachable from the host. A session-only count would read zero while a buffer
// the consumer kept was still pointing into the image.
//
// THE CREATE/UNLOAD TRANSITION IS ONE ATOMIC STEP
// This interface is free-threaded (component-r1), so "read the count, then
// latch the refusal" is a check-to-act race: another thread creates an object
// between the two and the loader unmaps an image that is in use. The count and
// the refusal therefore live in ONE atomic word and move together.
//
// A REFUSAL IS RECOVERABLE, BECAUSE IT DOES NOT LATCH
// PrepareUnload answers kInvalidState in two cases - objects are still alive,
// or somebody other than the asking caller still holds a reference to this root
// - and in NEITHER does it set the unloading bit. The module remains completely
// usable, and the same call succeeds once the obstruction goes. The alternative,
// which this code had and which an audit reproduced, is a module that refuses
// to create objects AND can never be unmapped: a permanent leak dressed as
// safety. The caller protocol that makes the second case answerable - release
// the root interface, keep the lifetime reference, restore the root on refusal
// - is documented in integrations/component/module_loader.h.

#include <atomic>
#include <cstdint>

#include "core/component/abi/backend_abi.h"
#include "core/component/abi/module_entry.h"
#include "core/component/component_module.h"
#include "core/component/object_support.h"
#include "core/mihomo/module/backend_session.h"
#include "integrations/component/marshal/com_objects.h"

namespace core::module {

namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;

class BackendModule final : public com::IComponentModule,
                            public abi::IModuleLifetime,
                            public ::clashqt::integration::marshal::ObjectAnchor {
  public:
    /// `description` must outlive the module: IComponentModule::Description()
    /// returns storage the module owns, and a string literal is what every
    /// entry point passes.
    ///
    /// `imageMayUnmap` is the MEASURED answer to "did this module manage to pin
    /// the shared runtime it brought into the process", not a policy switch.
    /// False makes PrepareUnload answer kFalse; see shared_runtime.h.
    BackendModule(const com::ModuleId &id, const char *description, BackendFactory factory,
                  CommandExtension extension, bool imageMayUnmap) noexcept;

    BackendModule(const BackendModule &) = delete;
    BackendModule &operator=(const BackendModule &) = delete;

    // ---- clashqt::com::IObject
    com::Result QueryInterface(const com::InterfaceId &id, void **out) noexcept override;
    std::int32_t AddRef() noexcept override;
    std::int32_t Release() noexcept override;

    // ---- clashqt::com::IComponentModule
    std::uint32_t AbiVersion() noexcept override;
    com::Result GetModuleId(com::ModuleId *out) noexcept override;
    const char *Description() noexcept override;
    com::Result CreateObject(const com::ClassId &classId, const com::InterfaceId &interfaceId,
                             void **out) noexcept override;

    // ---- clashqt::com::abi::IModuleLifetime
    std::int32_t LiveObjectCount() noexcept override;
    com::Result PrepareUnload() noexcept override;

    // ---- clashqt::integration::marshal::ObjectAnchor
    //
    // The seam every produced object reports through. Sessions use it too, so
    // there is exactly one accounting path and not two that can disagree.
    void objectCreated() noexcept override;
    void objectDestroyed() noexcept override;

  private:
    ~BackendModule() = default;

    /// The live-object count and the "no more objects" latch, in one word.
    /// Bit 63 is the latch; the rest is the count. Both move under one CAS.
    static constexpr std::uint64_t kUnloadingBit = 1ull << 63;
    static constexpr std::uint64_t kCountMask = kUnloadingBit - 1;

    com::ReferenceCount references_;
    com::ModuleId id_;
    const char *description_ = "";
    BackendFactory factory_;
    CommandExtension extension_;
    bool imageMayUnmap_ = false;
    std::atomic<std::uint64_t> state_{0};
};

}  // namespace core::module
