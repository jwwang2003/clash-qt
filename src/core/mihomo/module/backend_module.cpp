#include "core/mihomo/module/backend_module.h"

#include <new>
#include <utility>

namespace core::module {

BackendModule::BackendModule(const com::ModuleId &id, const char *description,
                             BackendFactory factory, CommandExtension extension,
                             bool imageMayUnmap) noexcept
    : id_(id),
      description_(description == nullptr ? "" : description),
      factory_(std::move(factory)),
      extension_(std::move(extension)),
      imageMayUnmap_(imageMayUnmap) {}

com::Result BackendModule::QueryInterface(const com::InterfaceId &id, void **out) noexcept {
    const com::InterfaceEntry entries[] = {
        com::MakeInterfaceEntry(static_cast<com::IComponentModule *>(this)),
        com::MakeInterfaceEntry(static_cast<abi::IModuleLifetime *>(this)),
        // Multiple inheritance means the two interface pointers differ; the
        // IObject pointer is the identity, and component-r1 requires it to be
        // the same value however it is reached. The canonical branch is the
        // module interface, which is also what the reference count belongs to.
        com::InterfaceEntry{&com::kIObjectId,
                            static_cast<void *>(static_cast<com::IObject *>(
                                static_cast<com::IComponentModule *>(this)))},
    };
    return com::ResolveInterface(id, out, entries, sizeof(entries) / sizeof(entries[0]),
                                 static_cast<com::IComponentModule *>(this));
}

std::int32_t BackendModule::AddRef() noexcept { return references_.Increment(); }

std::int32_t BackendModule::Release() noexcept {
    const std::int32_t remaining = references_.Decrement();
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

std::uint32_t BackendModule::AbiVersion() noexcept { return abi::kModuleAbiVersion; }

com::Result BackendModule::GetModuleId(com::ModuleId *out) noexcept {
    if (out == nullptr) {
        return com::kInvalidArgument;
    }
    *out = id_;
    return com::kOk;
}

const char *BackendModule::Description() noexcept { return description_; }

com::Result BackendModule::CreateObject(const com::ClassId &classId,
                                        const com::InterfaceId &interfaceId, void **out) noexcept {
    if (out == nullptr) {
        return com::kInvalidArgument;
    }
    *out = nullptr;
    if (classId != abi::kBackendSessionClassId) {
        return com::kNotFound;
    }

    // The admission and the increment are ONE step. Checking a separate
    // "unloading" flag and then incrementing a separate counter leaves a window
    // in which PrepareUnload sees zero, answers kOk, and this call hands out an
    // object into an image the loader is already unmapping.
    std::uint64_t state = state_.load(std::memory_order_acquire);
    do {
        if ((state & kUnloadingBit) != 0) {
            // PrepareUnload has been answered. Handing out one more object here
            // is how a loader ends up unmapping a library that is still in use.
            return com::kInvalidState;
        }
    } while (!state_.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel,
                                           std::memory_order_acquire));
    // The counter is claimed; the module reference that goes with it is taken
    // here so the pair is never half-held.
    AddRef();

    BackendSession *session = nullptr;
    try {
        // Not new(std::nothrow): the constructor copies a std::function, and a
        // throwing copy escaping a COM entry point is undefined behaviour
        // across the boundary. Allocation failure and copy failure land in the
        // same handler and produce the same answer - a failure code and a null
        // output - which is what component-r1 asks of every entry point.
        session = new BackendSession(this, factory_);
    } catch (...) {
        // Nothing was constructed, so nothing will report its own death:
        // unwind exactly what was claimed above, by hand.
        state_.fetch_sub(1, std::memory_order_acq_rel);
        Release();
        return com::kFail;
    }
    try {
        session->setCommandExtension(extension_);
    } catch (...) {
        // The session exists, so its own destructor does the unwinding through
        // objectDestroyed - the same path a normal release takes.
        session->Release();
        return com::kFail;
    }

    const com::Result status = session->QueryInterface(interfaceId, out);
    if (com::IsFailure(status)) {
        // Releases the construction reference, which destroys the session and
        // runs objectDestroyed - so the counter and the module reference are
        // both unwound by the same path that unwinds them normally.
        session->Release();
        return status;
    }
    // QueryInterface took a second reference; drop the construction one so the
    // caller owns exactly one, as component-r1 requires.
    session->Release();
    return com::kOk;
}

std::int32_t BackendModule::LiveObjectCount() noexcept {
    return static_cast<std::int32_t>(state_.load(std::memory_order_acquire) & kCountMask);
}

com::Result BackendModule::PrepareUnload() noexcept {
    std::uint64_t state = state_.load(std::memory_order_acquire);
    for (;;) {
        if ((state & kCountMask) != 0) {
            // The module stays usable AND stays open: the consumer still has to
            // release those objects, and it may legitimately create more before
            // it gets round to unloading. Latching the refusal here would turn
            // one "not yet" into a module that can never create another object
            // - a refusal that outlives its own reason.
            return com::kInvalidState;
        }
        if ((state & kUnloadingBit) != 0) {
            return com::kAlreadyClosed;  // prepared before, and still idempotent
        }
        if (state_.compare_exchange_weak(state, state | kUnloadingBit, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            break;
        }
        // The word moved: re-read and decide again. This is the loop that makes
        // "nothing is alive" and "no more objects" a single observation.
    }
    // Quiescent. Whether the IMAGE may go is a separate, measured question:
    // this module's own code is safe to unmap, but only if the shared runtime
    // it dragged into the process was pinned first.
    return imageMayUnmap_ ? com::kOk : com::kFalse;
}

void BackendModule::objectCreated() noexcept {
    // Used by objects the module produces AFTER admission has been decided -
    // reply buffers and diagnostics, which are created while a session is alive
    // and therefore while the latch is necessarily clear. They increment
    // unconditionally: refusing one here would fail a command for a reason the
    // caller cannot act on, and the latch cannot be set while their session
    // still holds its own count.
    state_.fetch_add(1, std::memory_order_acq_rel);
    AddRef();
}

void BackendModule::objectDestroyed() noexcept {
    state_.fetch_sub(1, std::memory_order_acq_rel);
    Release();
}

}  // namespace core::module
