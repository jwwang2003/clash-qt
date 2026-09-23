#pragma once

// Loads a backend module and validates that it is one of ours before anything
// calls into it.
//
// THE RULES, FROM module-r1, EACH OF WHICH IS A REFUSAL
//   * The artifact is either an EXPLICIT path the caller chose or the
//     installation-relative one. There is no source-tree fallback, no build
//     directory fallback and no PATH search: a loader that searches can load a
//     module the user did not install, and the failure mode of that is silent.
//   * Identity is validated. A loader that asked for the shipping module never
//     accepts the test double, even though both export the same symbol.
//   * Target and version are validated by the handshake before the module is
//     asked for a single object.
//   * The library is NOT unmapped while any object it created is alive, while
//     anyone but this loader still holds a reference to its ROOT, or when the
//     module answers that its image must stay. Three separate questions, all
//     of them accounting rather than a blanket refusal - and the shipping
//     module, driven through a real engine and back, IS unmapped.
//
// This class deliberately knows nothing about backend-r4. It hands back an
// IBackendSession; ModuleBackend is what turns that into a MihomoBackend.

#include <cstdint>

#include <QString>

#include "core/component/abi/backend_abi.h"
#include "core/component/abi/module_entry.h"
#include "core/component/com_ptr.h"
#include "core/component/component_module.h"

namespace clashqt::integration {

namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;

class ModuleLoader {
  public:
    /// The explicit-path constructor. The path is taken as given - it is not
    /// searched for, completed or guessed at.
    explicit ModuleLoader(QString artifactPath);
    ~ModuleLoader();

    ModuleLoader(const ModuleLoader &) = delete;
    ModuleLoader &operator=(const ModuleLoader &) = delete;

    /// The installation-relative artifact, for the common case. On macOS that
    /// is the bundle's Frameworks directory; elsewhere it is beside the
    /// executable. Returns the first candidate that EXISTS, or the primary
    /// candidate when none does - so a failure names a real path rather than an
    /// empty string. Never consults PATH or a build tree.
    static QString installedModulePath();

    /// The platform file name of a backend module.
    static QString moduleFileName();

    /// Loads, resolves the one exported symbol, performs the handshake and
    /// takes a reference on the module root. `expectedModuleId` is what the
    /// module must report; the shipping default refuses the test double.
    bool load(const com::ModuleId &expectedModuleId = abi::kMihomoModuleId);

    /// Creates one session. Fails without loading anything first: this class
    /// never loads implicitly.
    bool createSession(com::ComPtr<abi::IBackendSession> &out);

    /// Releases the module root and, when that is safe, unmaps the library.
    ///
    /// THE CALLER PROTOCOL, AND WHY IT IS SHAPED LIKE THIS
    /// A loader that simply dropped every reference and then looked at the
    /// remainder learned "somebody else is holding this" at the exact moment it
    /// had nothing left to hold, so it could neither retry nor recover: the
    /// module was latched shut and its image leaked for the life of the
    /// process. This implementation instead:
    ///   1. releases its ROOT interface while keeping its IModuleLifetime one,
    ///      so the object is still pinned and the question is answerable;
    ///   2. calls PrepareUnload(), which requires that the asking caller be the
    ///      ONLY holder and zero objects be alive, and which refuses WITHOUT
    ///      latching anything;
    ///   3. on refusal, restores the root through QueryInterface - legal
    ///      because the lifetime reference never let the object die - and
    ///      returns false with a diagnostic;
    ///   4. on success, drops the last reference and unmaps.
    /// A refused unload therefore changes NOTHING: isLoaded() is still true,
    /// createSession() still works, and the same call succeeds once the other
    /// holder releases. That retry is the behaviour this class promises.
    ///
    /// Returns true when the module was released, false when it refused and
    /// nothing changed. Ask isMapped() for whether the image went with a
    /// successful release; it stays when the module answered kFalse, because it
    /// could not pin the shared runtime it brought into the process and
    /// unmapping it would take QtNetwork and friends with it (see
    /// IModuleLifetime::PrepareUnload). lastError() says which case it was.
    bool unload();

    /// Whether this loader still holds the image mapped. False after a
    /// successful unload that got as far as unmapping.
    bool isMapped() const noexcept { return handle_ != nullptr; }

    /// Whether the artifact is still mapped into this PROCESS, asked of the
    /// platform loader rather than of our own bookkeeping.
    ///
    /// isMapped() says what this loader did; this says what actually happened.
    /// They differ exactly when something else in the process also holds the
    /// library, which is the case a suite has to be able to tell apart from a
    /// unload that silently left the mapping behind.
    bool isImageStillMappedInProcess() const;

    /// A module root is held. Distinct from isMapped(): a pinned module is
    /// no longer loaded once released, and its image is still mapped.
    bool isLoaded() const noexcept { return static_cast<bool>(module_); }
    /// Borrowed; valid while the loader holds the module.
    com::IComponentModule *module() const noexcept { return module_.Get(); }
    /// Objects the module has handed out and nobody has released, or -1 when
    /// the module does not answer the lifetime interface.
    std::int32_t liveObjectCount() const;

    QString artifactPath() const { return artifactPath_; }
    /// What the module answered, valid after a load attempt that got far
    /// enough to call the entry point.
    const abi::ModuleHandshakeResponse &handshake() const noexcept { return handshake_; }
    bool handshakeAnswered() const noexcept { return handshakeAnswered_; }

    com::Result lastErrorCode() const noexcept { return lastErrorCode_; }
    QString lastError() const { return lastError_; }

  private:
    bool fail(com::Result code, const QString &message);
    void closeHandle();
    /// Step 3 of the protocol above: takes the root interface back while the
    /// lifetime reference still pins the object. False only if the module
    /// refuses its own module id, which would be a module bug.
    bool restoreRoot();

    QString artifactPath_;
    void *handle_ = nullptr;
    com::ComPtr<com::IComponentModule> module_;
    com::ComPtr<abi::IModuleLifetime> lifetime_;
    abi::ModuleHandshakeResponse handshake_{};
    bool handshakeAnswered_ = false;
    com::Result lastErrorCode_ = com::kOk;
    QString lastError_;
};

}  // namespace clashqt::integration
