// The test module: the deterministic FakeBackend behind the SAME export, the
// same handshake and the same marshalling as the shipping module.
//
// D8 left one question open for P4's first worker: "whether the fake backend
// also becomes module-backed. If it does not, the module path has exactly one
// implementation and the common-contract claim is weaker than it sounds."
// This file is the answer. Because the double goes through the production
// adapter rather than around it, a single suite can hold the in-process fake,
// the in-process real backend, the module-backed fake and the module-backed
// real backend to the same statements - which is what backend-r4 section 10
// asks for and what G2 calls "fake and production modules satisfy common
// contracts".
//
// It is built for tests only. Its module id is different, so a loader that
// asked for the shipping module refuses it - and the ABI suite proves that.

#include <memory>
#include <type_traits>

#include "core/backend/backend.h"
#include "core/backend/privileged_core_service.h"
#include "core/component/abi/backend_abi.h"
#include "core/component/abi/module_entry.h"
#include "core/mihomo/module/backend_session.h"
#include "core/mihomo/module/module_export.h"
#include "integrations/component/marshal/backend_marshal.h"
#include "integrations/component/marshal/codec.h"
#include "integrations/component/marshal/com_objects.h"
#include "support/backend/fake_backend.h"
#include "support/component/fake_module_protocol.h"

namespace {

namespace com = ::clashqt::com;
namespace abi = ::clashqt::com::abi;
namespace cb = ::core::backend;
namespace marshal = ::clashqt::integration::marshal;
namespace fakes = ::testsupport::backend;
namespace proto = ::testsupport::component;

fakes::FakeBackend *fakeOf(core::module::BackendSession &session) {
    return static_cast<fakes::FakeBackend *>(session.backend());
}

core::module::WrappedBackend makeFakeBackend(core::PrivilegedCoreService *service) {
    // The fake has no privileged-service seam of its own; it models the
    // service through its own knobs. Accepting and ignoring the argument keeps
    // the factory signature identical to the shipping one.
    (void)service;
    auto backend = std::make_unique<fakes::FakeBackend>();
    fakes::FakeBackend *dispatcher = backend.get();
    // The same two accessors the shipping factory supplies, so the double's
    // events carry real production sequences and the host's admission rule is
    // exercised by the deterministic module rather than only by the real one.
    return core::module::WrappedBackend{
        std::move(backend),
        [dispatcher] { return dispatcher->producedSequence(); },
        [dispatcher] { return dispatcher->deliverySequence(); },
    };
}

/// Test-only: presents a module that is quiescent but asks not to be unmapped,
/// so a suite can prove the loader HONOURS that answer rather than assuming it.
/// Read from the environment because the answer has to be fixed before the root
/// exists, which is before any command could set it.
bool unpinnableRequested() {
    return !qEnvironmentVariableIsEmpty("CLASHQT_FAKE_MODULE_UNPINNABLE");
}

com::Result reply(core::module::BackendSession &session, com::IBuffer **out,
                  const marshal::ByteWriter &payload) {
    if (out == nullptr || payload.isEmpty()) {
        return com::kOk;
    }
    // Anchored on the module exactly as a shipping reply is. A control command
    // that produced an uncounted buffer would let the double be unmapped with
    // one of its own objects still in the caller's hands - and the suite would
    // be proving the accounting against a module that opts out of it.
    auto *buffer =
        marshal::ByteBuffer::Create(payload.data().data(), payload.size(), session.anchor());
    if (buffer == nullptr) {
        return com::kFail;
    }
    *out = static_cast<com::IBuffer *>(buffer);
    return com::kOk;
}

com::Result handleControl(core::module::BackendSession &session, std::uint32_t command,
                          const void *args, std::size_t size, com::IBuffer **out) {
    fakes::FakeBackend *fake = fakeOf(session);
    if (fake == nullptr) {
        return com::kInvalidState;
    }
    marshal::ByteReader in(args, size);
    marshal::ByteWriter answer;

    switch (command) {
        case proto::kFakeStageVersion:
            fake->staged().version = in.text();
            break;
        case proto::kFakeStageMode:
            fake->staged().mode = in.text();
            break;
        case proto::kFakeStageProxies:
            fake->staged().groups = marshal::readVector<cb::ProxyGroup>(
                in, marshal::readProxyGroup, marshal::kMinProxyGroupBytes);
            fake->staged().nodes = marshal::readVector<cb::ProxyNode>(
                in, marshal::readProxyNode, marshal::kMinProxyNodeBytes);
            break;
        case proto::kFakeStageRules:
            fake->staged().rules =
                marshal::readVector<cb::Rule>(in, marshal::readRule, marshal::kMinRuleBytes);
            break;
        case proto::kFakeStageProviders:
            fake->staged().providers = marshal::readVector<cb::Provider>(
                in, marshal::readProvider, marshal::kMinProviderBytes);
            break;
        case proto::kFakeStageConfig:
            fake->staged().config = marshal::readBaseConfig(in);
            break;
        case proto::kFakeStageTunActual:
            fake->staged().tunActual = in.boolean();
            break;
        case proto::kFakeStageDnsResult:
            fake->staged().dnsResultJson = in.text();
            break;

        case proto::kFakeSetRequestGate:
            fake->setRequestGate(static_cast<fakes::Gate>(in.u8()));
            break;
        case proto::kFakeReleaseRequest: {
            const auto id = static_cast<cb::RequestId>(in.u64());
            const auto outcome = static_cast<fakes::RequestOutcome>(in.u8());
            const cb::ErrorInfo error = marshal::readErrorInfo(in);
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            answer.boolean(fake->releaseRequest(id, outcome, error));
            return reply(session, out, answer);
        }
        case proto::kFakeReleaseAllRequests:
            fake->releaseAllRequests(static_cast<fakes::RequestOutcome>(in.u8()));
            break;
        case proto::kFakeIsPending: {
            const auto id = static_cast<cb::RequestId>(in.u64());
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            answer.boolean(fake->isPending(id));
            return reply(session, out, answer);
        }

        case proto::kFakeAddExternalController:
            fake->addExternalController(marshal::readEndpoint(in));
            break;
        case proto::kFakeDisconnectController:
            fake->disconnectController();
            break;
        case proto::kFakeEmitCoreLogLine:
            fake->emitCoreLogLine(in.text());
            break;
        case proto::kFakeSetValidationGate:
            fake->setValidationGate(static_cast<fakes::Gate>(in.u8()));
            break;
        case proto::kFakeCompleteValidation: {
            const bool valid = in.boolean();
            const QString reason = in.text();
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            answer.boolean(fake->completeValidation(valid, reason));
            return reply(session, out, answer);
        }
        case proto::kFakeSetChildExitGate:
            fake->setChildExitGate(static_cast<fakes::Gate>(in.u8()));
            break;
        case proto::kFakeReleaseChildExit: {
            const std::int32_t code = in.i32();
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            answer.boolean(fake->releaseChildExit(code));
            return reply(session, out, answer);
        }
        case proto::kFakeAdvanceTime:
            fake->advanceTime(in.i64());
            break;
        case proto::kFakeSetDiscoverableBinary:
            fake->setDiscoverableBinary(in.text());
            break;

        case proto::kFakeSetServiceStatus:
            fake->setServiceStatus(marshal::readPrivilegedServiceStatus(in));
            break;
        case proto::kFakeSetServiceSupported:
            fake->setServiceSupported(in.boolean());
            break;
        case proto::kFakeSetServiceAvailable:
            fake->setServiceAvailable(in.boolean());
            break;

        case proto::kFakeEmitConnections: {
            const cb::Generation generation = marshal::readGeneration(in);
            const QVector<cb::Connection> connections = marshal::readVector<cb::Connection>(
                in, marshal::readConnection, marshal::kMinConnectionBytes);
            const quint64 uploadTotal = in.u64();
            const quint64 downloadTotal = in.u64();
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            // Through the session's observer face, so the sample is packed by
            // the production snapshot codec on its way out.
            session.sink().connectionsUpdated(generation, cb::makeSpan(connections),
                                                 uploadTotal, downloadTotal);
            return com::kOk;
        }
        case proto::kFakeEmitLogEntry: {
            const cb::Generation generation = marshal::readGeneration(in);
            const cb::LogEntry entry = marshal::readLogEntry(in);
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            session.sink().logReceived(generation, entry);
            return com::kOk;
        }
        case proto::kFakeEmitTrafficSample: {
            const cb::Generation generation = marshal::readGeneration(in);
            const quint64 up = in.u64();
            const quint64 down = in.u64();
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            session.sink().trafficSample(generation, up, down);
            return com::kOk;
        }
        case proto::kFakeEmitMemorySample: {
            const cb::Generation generation = marshal::readGeneration(in);
            const quint64 inuse = in.u64();
            const quint64 oslimit = in.u64();
            if (!in.finished()) {
                return com::kInvalidArgument;
            }
            session.sink().memorySample(generation, inuse, oslimit);
            return com::kOk;
        }

        default:
            return com::kNotImplemented;
    }

    return in.finished() ? com::kOk : com::kInvalidArgument;
}

}  // namespace

extern "C" CLASHQT_MODULE_EXPORT com::Result clashqt_component_module_entry(
    const abi::ModuleHandshakeRequest *request, abi::ModuleHandshakeResponse *response,
    com::IComponentModule **out) noexcept {
    return core::module::runModuleEntry(
        request, response, out, abi::kFakeModuleId,
        "clash-qt fake backend module (tests only; module-r1, backend-r4)", makeFakeBackend,
        [](core::module::BackendSession &session, std::uint32_t command, const void *args,
           std::size_t size, com::IBuffer **reply) {
            return handleControl(session, command, args, size, reply);
        },
        // Measured like the shipping module's, and then overridable DOWNWARDS
        // only by the environment: a double that says "do not unmap me" is how
        // the suite proves the loader honours a refusal. It can never claim to
        // be unmappable when the pinning pass said otherwise.
        core::module::pinSharedRuntime() && !unpinnableRequested());
}

static_assert(std::is_same_v<decltype(&clashqt_component_module_entry), abi::ModuleEntryFn>,
              "the test double must export exactly the published signature");
