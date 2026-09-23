#include "integrations/component/module_loader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include "integrations/component/marshal/runtime_tag.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace clashqt::integration {
namespace {

QString platformError() {
#if defined(_WIN32)
    const DWORD code = ::GetLastError();
    return QStringLiteral("system error %1").arg(code);
#else
    const char *message = ::dlerror();
    return message == nullptr ? QString() : QString::fromLocal8Bit(message);
#endif
}

void *openLibrary(const QString &path) {
#if defined(_WIN32)
    return static_cast<void *>(::LoadLibraryW(reinterpret_cast<const wchar_t *>(path.utf16())));
#else
    // RTLD_LOCAL: the module's symbols do not leak into the global namespace,
    // so two modules cannot resolve each other's internals by accident.
    // RTLD_NOW: an unresolved symbol is found here, not at the first call.
    return ::dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void closeLibrary(void *handle) {
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

void *findSymbol(void *handle, const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
}

QString describeStatus(com::Result status) {
    switch (status) {
        case com::kUnsupportedVersion:
            return QStringLiteral("the module was built for a different ABI, target or runtime");
        case com::kNotFound:
            return QStringLiteral("the module is not the one that was asked for");
        case com::kInvalidArgument:
            return QStringLiteral("the module did not recognise the handshake");
        default:
            return QStringLiteral("the module refused to initialise (%1)").arg(status);
    }
}

}  // namespace

ModuleLoader::ModuleLoader(QString artifactPath) : artifactPath_(std::move(artifactPath)) {}

ModuleLoader::~ModuleLoader() {
    // A loader destroyed while objects are alive must NOT unmap: the consumer
    // still holds vtable pointers into this image. Release what we own and
    // leave the mapping in place; unload() is the checked path.
    if (module_ && liveObjectCount() == 0 && unload()) {
        return;
    }
    // Either objects were alive, or unload() refused because somebody else
    // still holds the root. Both mean the image stays; drop only what this
    // loader owns.
    lifetime_.Reset();
    module_.Reset();
    // Deliberately leaks the mapping rather than unmapping an image something
    // else may still be pointing into. A leaked mapping is recoverable; a
    // dangling vtable pointer is not.
    handle_ = nullptr;
}

QString ModuleLoader::moduleFileName() {
#if defined(_WIN32)
    return QStringLiteral("clash_qt_backend_module.dll");
#elif defined(__APPLE__)
    return QStringLiteral("libclash_qt_backend_module.dylib");
#else
    return QStringLiteral("libclash_qt_backend_module.so");
#endif
}

QString ModuleLoader::installedModulePath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString name = moduleFileName();
    QStringList candidates;
#if defined(__APPLE__)
    // <bundle>/Contents/MacOS/<app> -> <bundle>/Contents/Frameworks/<module>
    candidates << QDir::cleanPath(appDir + QStringLiteral("/../Frameworks/") + name);
#endif
    candidates << QDir::cleanPath(appDir + QDir::separator() + name);
#if !defined(_WIN32) && !defined(__APPLE__)
    candidates << QDir::cleanPath(appDir + QStringLiteral("/../lib/") + name);
#endif
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return candidates.first();
}

bool ModuleLoader::fail(com::Result code, const QString &message) {
    lastErrorCode_ = code;
    lastError_ = message;
    return false;
}

void ModuleLoader::closeHandle() {
    if (handle_ != nullptr) {
        closeLibrary(handle_);
        handle_ = nullptr;
    }
}

bool ModuleLoader::load(const com::ModuleId &expectedModuleId) {
    if (isLoaded() || handle_ != nullptr) {
        return fail(com::kInvalidState, QStringLiteral("a module is already loaded"));
    }
    if (artifactPath_.isEmpty()) {
        return fail(com::kInvalidArgument, QStringLiteral("no module path was given"));
    }
    const QFileInfo artifact(artifactPath_);
    if (!artifact.isFile()) {
        // Checked before dlopen so a relative or missing path can never fall
        // through to the platform loader's own search rules.
        return fail(com::kNotFound,
                    QStringLiteral("no module at %1").arg(artifact.absoluteFilePath()));
    }

    void *handle = openLibrary(artifact.absoluteFilePath());
    if (handle == nullptr) {
        return fail(com::kFail, QStringLiteral("could not load %1: %2")
                                    .arg(artifact.absoluteFilePath(), platformError()));
    }

    auto entry = reinterpret_cast<abi::ModuleEntryFn>(
        findSymbol(handle, CLASHQT_COM_MODULE_ENTRY_NAME));
    if (entry == nullptr) {
        closeLibrary(handle);
        return fail(com::kNotFound,
                    QStringLiteral("%1 exports no %2")
                        .arg(artifact.absoluteFilePath(),
                             QStringLiteral(CLASHQT_COM_MODULE_ENTRY_NAME)));
    }

    const abi::ModuleHandshakeRequest request =
        abi::MakeHandshakeRequest(marshal::runtimeTag(), expectedModuleId);
    abi::ModuleHandshakeResponse response{};
    response.structSize = static_cast<std::uint32_t>(sizeof(response));

    com::IComponentModule *root = nullptr;
    const com::Result status = entry(&request, &response, &root);
    handshake_ = response;
    handshakeAnswered_ = response.structSize == sizeof(response);

    if (com::IsFailure(status) || root == nullptr) {
        closeLibrary(handle);
        return fail(status, describeStatus(status));
    }

    handle_ = handle;
    module_ = com::ComPtr<com::IComponentModule>::Adopt(root);
    // The lifetime interface is optional in the ABI but required of a module
    // this loader will ever unload; its absence is recorded, not fatal.
    module_.As(lifetime_);

    lastErrorCode_ = com::kOk;
    lastError_.clear();
    return true;
}

bool ModuleLoader::createSession(com::ComPtr<abi::IBackendSession> &out) {
    out.Reset();
    if (!module_) {
        return fail(com::kInvalidState, QStringLiteral("no module is loaded"));
    }
    const com::Result status = module_->CreateObject(abi::kBackendSessionClassId,
                                                     abi::kIBackendSessionId, out.PutVoid());
    if (com::IsFailure(status)) {
        return fail(status, QStringLiteral("the module would not create a backend session (%1)")
                                .arg(status));
    }
    return true;
}

std::int32_t ModuleLoader::liveObjectCount() const {
    if (!lifetime_) {
        return -1;
    }
    return lifetime_->LiveObjectCount();
}

bool ModuleLoader::isImageStillMappedInProcess() const {
#if defined(_WIN32)
    HMODULE handle = nullptr;
    const QString path = QFileInfo(artifactPath_).absoluteFilePath();
    if (::GetModuleHandleExW(0, reinterpret_cast<const wchar_t *>(path.utf16()), &handle) == 0) {
        return false;
    }
    ::FreeLibrary(handle);
    return true;
#else
    // RTLD_NOLOAD returns a handle only for an image that is ALREADY mapped, so
    // this is a question and not a load. The reference it takes is given back
    // immediately; the image survives that dlclose if anything else holds it,
    // which is precisely what is being asked.
    void *probe = ::dlopen(QFileInfo(artifactPath_).absoluteFilePath().toLocal8Bit().constData(),
                           RTLD_NOLOAD | RTLD_LAZY);
    if (probe == nullptr) {
        return false;
    }
    ::dlclose(probe);
    return true;
#endif
}

bool ModuleLoader::restoreRoot() {
    // The lifetime reference still pins the object, so this is a live
    // QueryInterface and not a resurrection: nothing was destroyed, and the
    // root pointer we get back is the same one we gave up a moment ago.
    if (!lifetime_ || module_) {
        return static_cast<bool>(module_);
    }
    return com::IsSuccess(
        lifetime_->QueryInterface(com::kIComponentModuleId, module_.PutVoid()));
}

bool ModuleLoader::unload() {
    if (!isLoaded()) {
        return fail(com::kInvalidState, QStringLiteral("no module is loaded"));
    }
    if (!lifetime_) {
        // A module that answers no lifetime interface cannot be asked, so the
        // only available evidence is the remainder of our own release. Kept for
        // modules older than IModuleLifetime; the shipping one takes the path
        // below.
        bool mayUnmap = true;
        if (com::IComponentModule *root = module_.Detach(); root != nullptr) {
            mayUnmap = root->Release() == 0;
        }
        if (!mayUnmap) {
            lastErrorCode_ = com::kFalse;
            lastError_ = QStringLiteral("another reference to the module root is still held");
            return true;
        }
        closeHandle();
        lastErrorCode_ = com::kOk;
        lastError_.clear();
        return true;
    }

    // THE RECOVERABLE PROTOCOL, step by step. Each step is here because the
    // obvious shortcut for it is what made a retained root a permanent leak.
    //
    // 1. Give up the ROOT interface but keep the LIFETIME one. The module is
    //    still pinned by that reference - nothing can be destroyed underneath
    //    us - and the question we are about to ask becomes answerable: "is the
    //    asking caller the only holder left".
    module_.Reset();

    // 2. Ask. The module answers without latching anything, so a refusal costs
    //    nothing and the module is exactly as usable afterwards as before.
    const com::Result status = lifetime_->PrepareUnload();

    if (com::IsFailure(status) && status != com::kAlreadyClosed) {
        // 3a. Refused. Put the root back - the lifetime reference still pins
        //     the object, so this is a plain QueryInterface - and report why.
        //     isLoaded() is true again, createSession() works again, and the
        //     SAME unload() succeeds once the obstruction is released. Nothing
        //     was released, so this is false: the caller has work to do.
        const std::int32_t live = lifetime_->LiveObjectCount();
        if (!restoreRoot()) {
            // Cannot happen while lifetime_ holds the object; if it somehow
            // did, the mapping is kept rather than dropped on a half state.
            return fail(com::kFail,
                        QStringLiteral("the module refused to unload and its root could not be "
                                       "recovered; the image is kept"));
        }
        return fail(status,
                    live > 0 ? QStringLiteral("%1 object(s) from this module are still alive")
                                   .arg(live)
                             : QStringLiteral("another reference to the module root is still "
                                              "held; release it and retry"));
    }

    // 3b. Prepared. kFalse is a SUCCESSFUL "no": the module is released, but it
    //     could not pin the shared runtime it brought in, so the IMAGE stays.
    const bool mayUnmap = status != com::kFalse;

    // 4. Drop the last reference. PrepareUnload just certified that this was
    //    the only one, so the remainder is read rather than assumed: a non-zero
    //    answer means the certification was wrong, and the mapping is kept
    //    instead of pulled from under whatever is still pointing at it.
    bool soleHolder = true;
    if (abi::IModuleLifetime *lifetime = lifetime_.Detach(); lifetime != nullptr) {
        soleHolder = lifetime->Release() == 0;
    }

    if (mayUnmap && soleHolder) {
        closeHandle();
        lastErrorCode_ = com::kOk;
        lastError_.clear();
        return true;
    }
    // Deliberately leaks the mapping rather than unmapping an image something
    // else may still be pointing into. The release SUCCEEDED, so this returns
    // true; lastError() carries why the image stayed, and isMapped() says so.
    lastErrorCode_ = com::kFalse;
    lastError_ = soleHolder
                     ? QStringLiteral("the module could not pin the runtime it loaded")
                     : QStringLiteral("a reference to the module root outlived PrepareUnload");
    return true;
}

}  // namespace clashqt::integration
