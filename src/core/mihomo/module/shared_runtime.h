#pragma once

// Keeping the SHARED RUNTIME mapped so the MODULE can be unmapped.
//
// THE DEFECT THIS EXISTS FOR, AS MEASURED
//   Unmapping a module unmaps every shared library whose only reference was
//   that module. Those libraries are not inert. QtNetwork registers a host
//   lookup manager with QCoreApplication::destroyed the first time anything
//   resolves a name, and that registration is a slot thunk INSIDE QtNetwork's
//   image, held by a QObject that outlives the module. Unmap QtNetwork and the
//   application segfaults when it destroys its QCoreApplication - which is
//   precisely what the first sample consumer did: EXC_BAD_ACCESS at
//   QtPrivate::QCallableObject<void (*)()>::impl in QtNetwork, reached from
//   doActivate inside ~QObject, with QtNetwork no longer in the image list.
//
//   The module's OWN image was innocent. Staged runs that loaded, created a
//   session, installed a host and set a binary path all unmapped cleanly; only
//   the stage that started the engine - the first to make a network request,
//   and so the first to construct that lookup manager - crashed.
//
// THE RULE THIS ENCODES
//   module-r1 assumes host and module share ONE runtime, which the handshake's
//   runtime tag enforces. A shared runtime is process-wide by definition: it is
//   not the module's to unload, whoever happened to map it first. So a module
//   pins what it brought in before it constructs anything, and then answers the
//   unload question about ITS OWN image, which is the only one it owns.
//
//   Pinning a library the host also linked is a no-op. Pinning one the module
//   dragged in leaks a mapping the process was going to keep for its whole life
//   anyway, and buys an image that can actually be replaced. That is the
//   opposite trade from pinning the module itself, which buys nothing.
//
// HEADER-ONLY, DELIBERATELY
//   Every function here is inline so this file adds no translation unit and
//   therefore no build registration. It is included by module_export.h, so both
//   the shipping module and the test double get it through the adapter sources
//   the build already names.
//
// QUALIFICATION
//   Measured on macOS arm64. The Linux path uses the same dlopen semantics and
//   is compiled but unrun; the Windows path is compiled but unrun and pins
//   through module handles, which is a different mechanism with different
//   failure modes.

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <mach-o/loader.h>
#elif defined(__linux__)
#include <link.h>
#endif

namespace core::module {
namespace runtime_detail {

inline std::atomic<std::size_t> &pinnedImages() noexcept {
    static std::atomic<std::size_t> count{0};
    return count;
}

#if defined(__APPLE__) || defined(__linux__)

/// Bumps the reference count of an ALREADY-LOADED image and promotes it to
/// RTLD_NODELETE. Either alone would do; both together mean the mapping
/// survives whether the platform honours the flag or only the count.
///
/// RTLD_NOLOAD is what keeps this from being a load: an image that is not
/// already mapped is not one this module brought in, and pulling it in here
/// would be the opposite of the point.
inline bool pinByName(const char *name) noexcept {
    void *handle = ::dlopen(name, RTLD_NOLOAD | RTLD_NODELETE | RTLD_LAZY);
    // The handle is deliberately never closed. It IS the reference that keeps
    // the mapping, and there is no point in the process's life at which
    // dropping it would be correct.
    return handle != nullptr;
}

#endif

#if defined(__APPLE__)

inline bool pinDependencies(std::size_t *pinned) noexcept {
    Dl_info info{};
    if (::dladdr(reinterpret_cast<const void *>(&pinnedImages), &info) == 0 ||
        info.dli_fbase == nullptr) {
        return false;
    }
    const auto *header = static_cast<const mach_header_64 *>(info.dli_fbase);
    if (header->magic != MH_MAGIC_64) {
        return false;
    }

    bool complete = true;
    const auto *command = reinterpret_cast<const load_command *>(header + 1);
    for (std::uint32_t index = 0; index < header->ncmds; ++index) {
        switch (command->cmd) {
            case LC_LOAD_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB:
            case LC_LOAD_WEAK_DYLIB: {
                const auto *dylib = reinterpret_cast<const dylib_command *>(command);
                const char *name =
                    reinterpret_cast<const char *>(command) + dylib->dylib.name.offset;
                if (pinByName(name)) {
                    ++*pinned;
                } else if (command->cmd != LC_LOAD_WEAK_DYLIB) {
                    // A weak dependency that is absent is normal. A hard one
                    // that cannot be found while we are running out of an image
                    // that links it is not, and it is reported rather than
                    // rounded off: it is the difference between "may unmap" and
                    // "must not".
                    complete = false;
                }
                break;
            }
            default:
                break;
        }
        command = reinterpret_cast<const load_command *>(reinterpret_cast<const char *>(command) +
                                                         command->cmdsize);
    }
    // LC_ID_DYLIB is deliberately not in that list: this module's own image is
    // the one thing here that must stay unpinned.
    return complete;
}

#elif defined(__linux__)

inline bool pinDependencies(std::size_t *pinned) noexcept {
    // _DYNAMIC is this shared object's own dynamic section. DT_NEEDED entries
    // are offsets into DT_STRTAB, which the dynamic loader has already
    // relocated to an absolute address for a shared object.
    extern ElfW(Dyn) _DYNAMIC[];
    const char *strings = nullptr;
    for (const ElfW(Dyn) *entry = _DYNAMIC; entry->d_tag != DT_NULL; ++entry) {
        if (entry->d_tag == DT_STRTAB) {
            strings = reinterpret_cast<const char *>(entry->d_un.d_ptr);
            break;
        }
    }
    if (strings == nullptr) {
        return false;
    }
    bool complete = true;
    for (const ElfW(Dyn) *entry = _DYNAMIC; entry->d_tag != DT_NULL; ++entry) {
        if (entry->d_tag != DT_NEEDED) {
            continue;
        }
        if (pinByName(strings + entry->d_un.d_val)) {
            ++*pinned;
        } else {
            complete = false;
        }
    }
    return complete;
}

#elif defined(_WIN32)

inline bool pinDependencies(std::size_t *pinned) noexcept {
    // GET_MODULE_HANDLE_EX_FLAG_PIN is the Windows spelling of "this library
    // stays for the life of the process". Applied to each IMPORT, never to this
    // module itself - the UNCHANGED_REFCOUNT flag on the self lookup is what
    // keeps this from pinning the very image it is trying to make unloadable.
    HMODULE self = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&pinnedImages), &self) == 0 ||
        self == nullptr) {
        return false;
    }
    const auto *base = reinterpret_cast<const unsigned char *>(self);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_DATA_DIRECTORY &directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return true;  // nothing imported, nothing to pin
    }
    bool complete = true;
    for (const auto *descriptor =
             reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
         descriptor->Name != 0; ++descriptor) {
        const char *name = reinterpret_cast<const char *>(base + descriptor->Name);
        HMODULE handle = nullptr;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &handle) != 0) {
            ++*pinned;
        } else {
            complete = false;
        }
    }
    return complete;
}

#else

inline bool pinDependencies(std::size_t *) noexcept { return false; }

#endif

inline bool runOnce() noexcept {
    std::size_t pinned = 0;
    const bool complete = pinDependencies(&pinned);
    pinnedImages().store(pinned, std::memory_order_release);
    return complete;
}

}  // namespace runtime_detail

/// Pins every shared library this module links against, so unmapping this
/// module cannot unmap them. Returns true when every non-weak dependency was
/// pinned - which is the only condition under which this image may be unmapped.
///
/// Idempotent, thread-safe, and safe to call before a QCoreApplication exists:
/// it allocates nothing beyond what the platform loader does, touches no Qt
/// type, and deliberately never releases what it takes.
inline bool pinSharedRuntime() noexcept {
    // A function-local static of trivial type: initialised exactly once,
    // thread-safely, with no atexit registration to be run out of an image that
    // is about to be unmapped.
    static const bool pinned = runtime_detail::runOnce();
    return pinned;
}

/// How many images the pinning pass took a reference on. Diagnostic only: the
/// dependency list is a property of the build, not of the contract, so a suite
/// reports this rather than asserting a number.
inline std::size_t pinnedImageCount() noexcept {
    return runtime_detail::pinnedImages().load(std::memory_order_acquire);
}

}  // namespace core::module
