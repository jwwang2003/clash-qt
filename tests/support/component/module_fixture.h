#pragma once

// Loading a module and driving the double behind it, for suites.
//
// A fixture rather than a helper function, because the acceptance question for
// P4 is "do the in-process backend and the module-backed one satisfy the SAME
// contract tests" - and that is only answerable if a suite can obtain a
// MihomoBackend without knowing which it is. ModuleFixture::backend() returns
// one; everything module-specific is on the fixture, not on the backend.
//
// WHERE THE ARTIFACT COMES FROM
//   The environment variable first, then the path the build baked in. Both are
//   EXPLICIT: neither consults PATH, a source tree or a build directory, which
//   is the loader rule this fixture must not quietly break.

#include <memory>

#include <QByteArray>
#include <QString>

#include "core/component/abi/backend_abi.h"
#include "core/component/com_ptr.h"
#include "integrations/component/marshal/backend_marshal.h"
#include "integrations/component/marshal/codec.h"
#include "integrations/component/marshal/com_objects.h"
#include "integrations/component/module_backend.h"
#include "integrations/component/module_loader.h"
#include "support/component/fake_module_protocol.h"

namespace testsupport::component {

namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;
namespace cb = ::core::backend;
namespace marshal = ::clashqt::integration::marshal;

/// The test double's artifact.
inline QString fakeModulePath() {
    const QByteArray fromEnvironment = qgetenv("CLASH_QT_FAKE_MODULE");
    if (!fromEnvironment.isEmpty()) {
        return QString::fromLocal8Bit(fromEnvironment);
    }
#ifdef CLASHQT_FAKE_MODULE_PATH
    return QStringLiteral(CLASHQT_FAKE_MODULE_PATH);
#else
    return {};
#endif
}

/// The shipping artifact, so a suite can assert what the REAL module refuses.
inline QString realModulePath() {
    const QByteArray fromEnvironment = qgetenv("CLASH_QT_BACKEND_MODULE");
    if (!fromEnvironment.isEmpty()) {
        return QString::fromLocal8Bit(fromEnvironment);
    }
#ifdef CLASHQT_BACKEND_MODULE_PATH
    return QStringLiteral(CLASHQT_BACKEND_MODULE_PATH);
#else
    return {};
#endif
}

class ModuleFixture {
  public:
    ModuleFixture(QString artifactPath, const com::ModuleId &expectedModuleId)
        : loader_(std::move(artifactPath)), expected_(expectedModuleId) {}

    ~ModuleFixture() { unload(); }

    ModuleFixture(const ModuleFixture &) = delete;
    ModuleFixture &operator=(const ModuleFixture &) = delete;

    bool load(core::PrivilegedCoreService *service = nullptr) {
        if (!loader_.load(expected_)) {
            return false;
        }
        com::ComPtr<abi::IBackendSession> session;
        if (!loader_.createSession(session)) {
            return false;
        }
        // The fixture keeps a second reference to the SAME session object, so
        // a control command can reach the module without going through the
        // contract surface. It is one object with two owners, not two
        // sessions: the module's live-object count stays at one, and the ABI
        // suite asserts that too.
        session_ = session;
        backend_ = std::make_unique<::clashqt::integration::ModuleBackend>(std::move(session),
                                                                          service);
        return backend_->isValid();
    }

    /// Tears down in the order the ABI requires: the backend closes its
    /// session and releases the module's objects, and only then may the
    /// library be unmapped.
    bool unload() {
        backend_.reset();
        session_.Reset();
        return loader_.isLoaded() ? loader_.unload() : true;
    }

    ::clashqt::integration::ModuleLoader &loader() noexcept { return loader_; }
    ::clashqt::integration::ModuleBackend *backend() const noexcept { return backend_.get(); }
    cb::MihomoBackend *contractBackend() const noexcept { return backend_.get(); }

    // ---------------------------------------------- the double's controls
    //
    // Every one of these is a command in the reserved range, so the same call
    // against the SHIPPING module is kNotImplemented - which the suite
    // asserts rather than assumes.

    com::Result control(std::uint32_t command, const marshal::ByteWriter &args = {}) {
        return invoke(command, args, nullptr);
    }

    bool controlFlag(std::uint32_t command, const marshal::ByteWriter &args) {
        std::vector<std::uint8_t> reply;
        if (com::IsFailure(invoke(command, args, &reply))) {
            return false;
        }
        marshal::ByteReader in(reply.data(), reply.size());
        const bool value = in.boolean();
        return in.finished() && value;
    }

    void stageVersion(const QString &version) {
        marshal::ByteWriter args;
        args.text(version);
        control(kFakeStageVersion, args);
    }

    void setRequestGate(bool held) {
        marshal::ByteWriter args;
        args.u8(held ? 1 : 0);
        control(kFakeSetRequestGate, args);
    }

    bool releaseRequest(cb::RequestId id, std::uint8_t outcome = 0,
                        const cb::ErrorInfo &error = {}) {
        marshal::ByteWriter args;
        args.u64(cb::number(id));
        args.u8(outcome);
        marshal::writeErrorInfo(args, error);
        return controlFlag(kFakeReleaseRequest, args);
    }

    bool isPending(cb::RequestId id) {
        marshal::ByteWriter args;
        args.u64(cb::number(id));
        return controlFlag(kFakeIsPending, args);
    }

    void addExternalController(const cb::Endpoint &endpoint) {
        marshal::ByteWriter args;
        marshal::writeEndpoint(args, endpoint);
        control(kFakeAddExternalController, args);
    }

  private:
    com::Result invoke(std::uint32_t command, const marshal::ByteWriter &args,
                       std::vector<std::uint8_t> *reply) {
        // Deliberately reaches the session directly rather than through
        // ModuleBackend: a control command is not part of backend-r4, and
        // routing it through the contract surface would put a test hook there.
        if (!session_) {
            return com::kInvalidState;
        }
        com::IBuffer *buffer = nullptr;
        const com::Result status =
            session_->Invoke(command, args.isEmpty() ? nullptr : args.data().data(), args.size(),
                             reply == nullptr ? nullptr : &buffer);
        if (reply != nullptr && buffer != nullptr) {
            *reply = ::clashqt::integration::marshal::ReadBuffer(buffer);
        }
        if (buffer != nullptr) {
            buffer->Release();
        }
        return status;
    }

    ::clashqt::integration::ModuleLoader loader_;
    com::ModuleId expected_;
    com::ComPtr<abi::IBackendSession> session_;
    std::unique_ptr<::clashqt::integration::ModuleBackend> backend_;
};

}  // namespace testsupport::component
