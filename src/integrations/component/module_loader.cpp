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
    if (module_ && liveObjectCount() == 0) {
        unload();
        return;
    }
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

bool ModuleLoader::unload() {
    if (!isLoaded()) {
        return fail(com::kInvalidState, QStringLiteral("no module is loaded"));
    }
    bool mayUnmap = true;
    QString keptBecause;
    if (lifetime_) {
        // kAlreadyClosed means a previous attempt already prepared it and
        // nothing has been created since. kFalse is a SUCCESSFUL "no": release
        // the root, keep the image.
        const com::Result status = lifetime_->PrepareUnload();
        if (status == com::kFalse) {
            mayUnmap = false;
            keptBecause = QStringLiteral("the module could not pin the runtime it loaded");
        }
        if (com::IsFailure(status) && status != com::kAlreadyClosed) {
            // The module stays loaded and stays usable. This is the honest
            // answer, and it is why the count is published: a consumer can see
            // what it still holds.
            return fail(status, QStringLiteral("%1 object(s) from this module are still alive")
                                    .arg(lifetime_->LiveObjectCount()));
        }
    }
    lifetime_.Reset();

    // The final release is performed by hand rather than through ComPtr::Reset
    // so its ANSWER can be read. A non-zero remainder means someone else still
    // holds a reference to the root - a QueryInterface result a consumer kept,
    // most likely - and that reference is a vtable in this image. Unmapping it
    // would be exactly the failure the count exists to prevent, and the count
    // cannot see it because the root is deliberately not in it.
    if (com::IComponentModule *root = module_.Detach(); root != nullptr) {
        if (root->Release() != 0) {
            mayUnmap = false;
            keptBecause = QStringLiteral("another reference to the module root is still held");
        }
    }

    if (mayUnmap) {
        closeHandle();
        lastErrorCode_ = com::kOk;
        lastError_.clear();
        return true;
    }
    // Deliberately leaks the mapping rather than unmapping an image something
    // else may still be pointing into. The release SUCCEEDED, so this returns
    // true; lastError() carries why the image stayed, and isMapped() says so.
    lastErrorCode_ = com::kFalse;
    lastError_ = keptBecause;
    return true;
}

}  // namespace clashqt::integration
