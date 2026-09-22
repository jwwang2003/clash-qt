// The module ABI contract suite, revision module-r1.
//
// WHAT THIS SUITE IS FOR, AND WHAT IT IS NOT FOR
//   It asserts the BOUNDARY: the handshake, identity, lifetime accounting,
//   lossless marshalling of every backend-r4 value, the delivery rules the Qt
//   event loop used to provide for free, and the privileged reverse seam. It
//   does NOT re-assert backend-r4's semantics - that is what running the
//   existing contract suite against a module-backed fixture is for, and it is
//   the next round's work. A case here fails when the boundary is broken, not
//   when the backend is.
//
// THE REAL MODULE IS DRIVEN, AND IS INERT WHILE IT IS
//   The shipping module is loaded and asked two questions that are answered
//   before any backend is constructed: what it is, and whether it implements
//   the test-control range. No host is installed on it, so MihomoBackendImpl
//   is never built, no engine is launched, no controller is contacted and the
//   privileged helper is never touched.

#include <QtTest>

#include <dlfcn.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QTemporaryDir>

#include "core/backend/backend.h"
#include "core/component/abi/abi.h"
#include "integrations/component/marshal/backend_marshal.h"
#include "integrations/component/marshal/connection_snapshot.h"
#include "integrations/component/marshal/runtime_tag.h"
#include "support/backend/fake_backend.h"
#include "support/component/fake_module_protocol.h"
#include "support/component/module_fixture.h"

namespace com = clashqt::com;
namespace abi = clashqt::com::abi;
namespace cb = core::backend;
namespace marshal = clashqt::integration::marshal;
namespace fixtures = testsupport::component;

namespace {

/// Records everything the boundary delivers, including the three callbacks the
/// shared RecordingObserver does not cover, and witnesses the delivery rules.
class BoundaryObserver final : public cb::BackendObserver {
  public:
    explicit BoundaryObserver(const clashqt::integration::ModuleBackend *backend = nullptr)
        : backend_(backend) {}

    struct ConnectionsEvent {
        cb::Generation generation = cb::Generation::Initial;
        QVector<cb::Connection> connections;
        quint64 uploadTotal = 0;
        quint64 downloadTotal = 0;
    };

    QStringList order;
    std::vector<cb::Completion> versions;
    QStringList versionTexts;
    std::vector<ConnectionsEvent> connectionSets;
    std::vector<cb::LogEntry> logs;
    std::vector<cb::PrivilegedServiceStatus> serviceStatuses;
    std::vector<cb::Completion> errors;
    QVector<cb::ProxyGroup> groups;
    QVector<cb::ProxyNode> nodes;
    QVector<cb::Rule> rules;
    QVector<cb::Provider> providers;
    cb::BaseConfig config;
    cb::TunChangeCompleted tunChange;
    std::vector<std::pair<quint64, quint64>> traffic;
    bool sawReentrantDelivery = false;
    int callbacks = 0;
    std::function<void()> onNextEvent;

    void witness(const char *channel) noexcept {
        ++callbacks;
        order.append(QString::fromLatin1(channel));
        if (backend_ != nullptr && backend_->isInsideCommand()) {
            sawReentrantDelivery = true;
        }
        if (onNextEvent) {
            auto once = onNextEvent;
            onNextEvent = nullptr;
            once();
        }
    }

    void versionReceived(const cb::Completion &completion, const QString &version) noexcept override {
        witness("versionReceived");
        versions.push_back(completion);
        versionTexts.append(version);
    }
    void connectionsUpdated(cb::Generation generation, cb::Span<cb::Connection> connections,
                            quint64 uploadTotal, quint64 downloadTotal) noexcept override {
        witness("connectionsUpdated");
        ConnectionsEvent event;
        event.generation = generation;
        for (std::size_t index = 0; index < connections.size(); ++index) {
            event.connections.append(connections[index]);
        }
        event.uploadTotal = uploadTotal;
        event.downloadTotal = downloadTotal;
        connectionSets.push_back(event);
    }
    void logReceived(cb::Generation generation, const cb::LogEntry &entry) noexcept override {
        witness("logReceived");
        (void)generation;
        logs.push_back(entry);
    }
    void trafficSample(cb::Generation generation, quint64 up, quint64 down) noexcept override {
        witness("trafficSample");
        (void)generation;
        traffic.emplace_back(up, down);
    }
    void privilegedServiceStatus(const cb::Completion &completion,
                                 const cb::PrivilegedServiceStatus &status) noexcept override {
        witness("privilegedServiceStatus");
        (void)completion;
        serviceStatuses.push_back(status);
    }
    void proxiesUpdated(const cb::Completion &completion, cb::Span<cb::ProxyGroup> incomingGroups,
                        cb::Span<cb::ProxyNode> incomingNodes) noexcept override {
        witness("proxiesUpdated");
        (void)completion;
        groups.clear();
        nodes.clear();
        for (std::size_t index = 0; index < incomingGroups.size(); ++index) {
            groups.append(incomingGroups[index]);
        }
        for (std::size_t index = 0; index < incomingNodes.size(); ++index) {
            nodes.append(incomingNodes[index]);
        }
    }
    void rulesUpdated(const cb::Completion &completion,
                      cb::Span<cb::Rule> incoming) noexcept override {
        witness("rulesUpdated");
        (void)completion;
        rules.clear();
        for (std::size_t index = 0; index < incoming.size(); ++index) {
            rules.append(incoming[index]);
        }
    }
    void providersReceived(const cb::Completion &completion, bool isRules,
                           cb::Span<cb::Provider> incoming) noexcept override {
        witness("providersReceived");
        (void)completion;
        (void)isRules;
        providers.clear();
        for (std::size_t index = 0; index < incoming.size(); ++index) {
            providers.append(incoming[index]);
        }
    }
    void configReceived(const cb::Completion &completion,
                        const cb::BaseConfig &incoming) noexcept override {
        witness("configReceived");
        (void)completion;
        config = incoming;
    }
    void tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept override {
        witness("tunChangeCompleted");
        tunChange = result;
    }
    void endpointChanged(cb::Generation generation, const cb::Endpoint &endpoint,
                         cb::Ownership ownership) noexcept override {
        witness("endpointChanged");
        (void)generation;
        (void)endpoint;
        (void)ownership;
    }
    void connectedChanged(cb::Generation generation, bool connected) noexcept override {
        witness("connectedChanged");
        (void)generation;
        (void)connected;
    }
    void errorOccurred(const cb::Completion &completion) noexcept override {
        witness("errorOccurred");
        errors.push_back(completion);
    }

  private:
    const clashqt::integration::ModuleBackend *backend_;
};

/// A privileged service the HOST owns, so the reverse seam has something real
/// to reach. It never talks to the installed helper: it records and answers.
class FakePrivilegedService final : public core::PrivilegedCoreService {
  public:
    void setListener(core::PrivilegedCoreServiceListener *listener) override {
        listener_ = listener;
        ++listenerChanges;
    }
    bool isSupported() const override { return supported; }
    bool isAvailable() const override { return available; }
    bool isConnected() const override { return connected; }
    bool isBusy() const override { return busy; }
    QString connectionError() const override { return error; }
    void requestStatus() override { ++statusRequests; }
    void requestLogs() override { ++logRequests; }
    void startCore(const QJsonObject &config) override { started.append(config); }
    void stopCore() override { ++stopRequests; }
    void close() override { ++closeRequests; }

    core::PrivilegedCoreServiceListener *listener() const noexcept { return listener_; }

    bool supported = true;
    bool available = true;
    bool connected = true;
    bool busy = false;
    QString error = QStringLiteral("no error");
    int listenerChanges = 0;
    int statusRequests = 0;
    int logRequests = 0;
    int stopRequests = 0;
    int closeRequests = 0;
    QList<QJsonObject> started;

  private:
    core::PrivilegedCoreServiceListener *listener_ = nullptr;
};

/// The smallest host a session will accept: it exists so a case can drive the
/// SESSION directly, without ModuleBackend in the way, and still get a module
/// that has built its backend. Events are counted and discarded.
class CountingHost final : public abi::IBackendHost {
  public:
    com::Result QueryInterface(const com::InterfaceId &id, void **out) noexcept override {
        if (out == nullptr) {
            return com::kInvalidArgument;
        }
        if (id == abi::kIBackendHostId || id == com::kIObjectId) {
            *out = static_cast<void *>(this);
            AddRef();
            return com::kOk;
        }
        *out = nullptr;
        return com::kNoInterface;
    }
    std::int32_t AddRef() noexcept override { return ++references_; }
    std::int32_t Release() noexcept override {
        const std::int32_t remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }
    com::Result Notify(std::uint32_t, const void *, std::size_t) noexcept override {
        ++events;
        return com::kOk;
    }
    com::Result Invoke(std::uint32_t, const void *, std::size_t,
                       com::IBuffer **reply) noexcept override {
        if (reply != nullptr) {
            *reply = nullptr;
        }
        return com::kInvalidState;  // this host injects no privileged service
    }

    int events = 0;

  private:
    ~CountingHost() = default;
    std::int32_t references_ = 1;
};

/// Gives back a mapping a case deliberately left behind, so no case is testing
/// against an image an earlier one leaked.
///
/// Only a test may do this, and only to the DOUBLE: every refusal reproduced
/// here is simulated, and the fake is as unmappable as the rest of this suite
/// proves. The loader's own dlopen reference is what is being dropped -
/// RTLD_NOLOAD adds a second, and two closes clear both.
void reclaimLeakedMapping(const QString &path) {
    void *leaked = dlopen(QFileInfo(path).absoluteFilePath().toLocal8Bit().constData(),
                          RTLD_NOLOAD | RTLD_LAZY);
    if (leaked == nullptr) {
        return;
    }
    dlclose(leaked);
    dlclose(leaked);
}

cb::Connection makeConnection(const QString &id, const QStringList &chains) {
    cb::Connection connection;
    connection.id = id;
    connection.host = QStringLiteral("example.test");
    connection.network = QStringLiteral("tcp");
    connection.connectionType = QStringLiteral("HTTPS");
    connection.chains = chains;
    connection.rule = QStringLiteral("DomainSuffix");
    connection.rulePayload = QStringLiteral("example.test");
    connection.sourceIp = QStringLiteral("192.168.1.2");
    connection.sourcePort = QStringLiteral("51234");
    connection.destinationIp = QStringLiteral("93.184.216.34");
    connection.destinationPort = QStringLiteral("443");
    connection.process = QStringLiteral("curl");
    connection.processPath = QStringLiteral("/usr/bin/curl");
    connection.upload = 0x0000'0001'0000'0007ull;
    connection.download = 0xFFFF'FFFF'0000'0001ull;
    connection.uploadRate = 0.1;
    connection.downloadRate = 1234.5678;
    connection.start = QDateTime::fromMSecsSinceEpoch(1'700'000'000'123LL);
    // Deliberately invalid: every LIVE connection has one, and an encoding
    // that collapses it to the epoch renders a connection that closed in 1970.
    connection.end = QDateTime();
    return connection;
}

}  // namespace

class AbiContractTest : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();

    // ---- handshake
    void theHandshakeAcceptsAHostBuiltFromTheSameHeaders();
    void theHandshakeRefusesAnIncompatibleModuleAbiVersion();
    void theHandshakeRefusesAForeignTargetAbi();
    void theHandshakeRefusesAMismatchedRuntime();
    void theHandshakeRefusesAStructSizeItDoesNotKnow();
    void theLoaderRefusesAModuleThatIsNotTheOneItAskedFor();
    void theLoaderRefusesAnArtifactItCannotSeeWithoutSearching();

    // ---- module identity and lifetime
    void theModuleRootAnswersItsIdentityAndRefusesAnUnknownClass();
    void aLiveObjectKeepsTheLibraryMapped();
    void anActivatedShippingModuleIsActuallyUnmapped();
    void aModuleThatRefusesTheUnmapKeepsItsImageMapped();
    void aRetainedRootReferenceKeepsTheImageMapped();
    void aRetainedReplyBufferKeepsTheModuleCountedAndMapped();
    void aRetainedDiagnosticAndItsMessageBufferEachKeepTheModule();
    void creatingAndPreparingInParallelNeverHandsOutAnObjectAfterPrepare();
    void aPreparedModuleCreatesNothingFurther();
    void theReaderLatchesAnOverrunInsteadOfContinuing();
    void closingIsIdempotentAndAClosedSessionRefusesCommands();
    void outstandingWorkCountsHeldRequestsAndCloseReportsTheTimeout();

    // ---- marshalling
    void sixtyFourBitIdentityAndCountersSurviveTheBoundary();
    void everyQueryOfTheContractCrossesTheBoundary();
    void stagedTelemetryRoundTripsThroughAReleasedRequest();
    void aPackedConnectionSnapshotRoundTripsExactly();
    void aMalformedConnectionSnapshotIsRefusedWithAReason();
    void thePrivilegedStatusCarriesCoreRunning();

    // ---- delivery rules
    void noObserverIsInvokedFromInsideACommand();
    void removingAnObserverDuringDeliveryStopsFurtherCallbacks();
    void anObserverAddedAfterProductionIsRefusedTheSameEventsAsDirect();
    void anObserverAddedDuringDeliveryIsAdmittedTheSameAsDirect();

    // ---- the reverse seam
    void thePrivilegedSeamReachesTheHostsServiceAndAllowsOneListener();

    // ---- refusals
    void aMalformedArgumentBlockIsRefusedWithADiagnostic();
    void anUnknownCommandIsNotImplementedRatherThanIgnored();
    void theShippingModuleDoesNotImplementTheTestControlRange();

  private:
    QString fakePath_;
    QString realPath_;
};

void AbiContractTest::initTestCase() {
    fakePath_ = fixtures::fakeModulePath();
    realPath_ = fixtures::realModulePath();
    QVERIFY2(!fakePath_.isEmpty(), "the test module path must be supplied by the build");
    QVERIFY2(QFileInfo::exists(fakePath_), qPrintable(QStringLiteral("no module at %1").arg(fakePath_)));
}

// ------------------------------------------------------------- handshake

namespace {

// Loads a library and resolves the published entry WITHOUT the loader, so a
// case can drive the handshake with fields the loader would never send.
struct RawModule {
    explicit RawModule(const QString &path) {
        handle = dlopen(QFileInfo(path).absoluteFilePath().toLocal8Bit().constData(),
                        RTLD_NOW | RTLD_LOCAL);
        if (handle != nullptr) {
            entry = reinterpret_cast<abi::ModuleEntryFn>(
                dlsym(handle, CLASHQT_COM_MODULE_ENTRY_NAME));
        }
    }
    ~RawModule() {
        if (handle != nullptr) {
            dlclose(handle);
        }
    }
    void *handle = nullptr;
    abi::ModuleEntryFn entry = nullptr;
};

abi::ModuleHandshakeRequest goodRequest(const com::ModuleId &expected) {
    return abi::MakeHandshakeRequest(marshal::runtimeTag(), expected);
}

abi::ModuleHandshakeResponse emptyResponse() {
    abi::ModuleHandshakeResponse response{};
    response.structSize = static_cast<std::uint32_t>(sizeof(response));
    return response;
}

}  // namespace

void AbiContractTest::theHandshakeAcceptsAHostBuiltFromTheSameHeaders() {
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());

    const abi::ModuleHandshakeResponse &response = fixture.loader().handshake();
    QVERIFY(fixture.loader().handshakeAnswered());
    QCOMPARE(response.status, com::kOk);
    QCOMPARE(response.abiVersion, abi::kModuleAbiVersion);
    QCOMPARE(response.wireRevision, abi::kWireRevision);
    QCOMPARE(response.interfaceRevision, abi::kBackendInterfaceRevision);
    QCOMPARE(response.targetAbiTag, abi::kTargetAbiTag);
    QCOMPARE(response.runtimeTag, marshal::runtimeTag());
    QVERIFY(response.moduleId == abi::kFakeModuleId);
    QVERIFY(fixture.loader().module() != nullptr);
    QCOMPARE(fixture.loader().module()->AbiVersion(), abi::kModuleAbiVersion);
}

void AbiContractTest::theHandshakeRefusesAnIncompatibleModuleAbiVersion() {
    RawModule module(fakePath_);
    QVERIFY(module.entry != nullptr);

    abi::ModuleHandshakeRequest request = goodRequest(abi::kFakeModuleId);
    request.abiVersion = abi::kModuleAbiVersion + 1;
    abi::ModuleHandshakeResponse response = emptyResponse();
    com::IComponentModule *root = reinterpret_cast<com::IComponentModule *>(0x1);

    const com::Result status = module.entry(&request, &response, &root);

    QCOMPARE(status, com::kUnsupportedVersion);
    QCOMPARE(root, nullptr);  // null is written FIRST, on every failure path
    QCOMPARE(response.status, com::kUnsupportedVersion);
    // The module still says what it is, so a loader reports a version rather
    // than "incompatible".
    QCOMPARE(response.abiVersion, abi::kModuleAbiVersion);
    QVERIFY(response.moduleId == abi::kFakeModuleId);
}

void AbiContractTest::theHandshakeRefusesAForeignTargetAbi() {
    RawModule module(fakePath_);
    QVERIFY(module.entry != nullptr);

    abi::ModuleHandshakeRequest request = goodRequest(abi::kFakeModuleId);
    // One bit is enough: a different compiler, architecture or C++ ABI does not
    // share a vtable layout, and no version of either side would help.
    request.targetAbiTag ^= 1ull;
    abi::ModuleHandshakeResponse response = emptyResponse();
    com::IComponentModule *root = nullptr;

    QCOMPARE(module.entry(&request, &response, &root), com::kUnsupportedVersion);
    QCOMPARE(root, nullptr);
    QCOMPARE(response.targetAbiTag, abi::kTargetAbiTag);
}

void AbiContractTest::theHandshakeRefusesAMismatchedRuntime() {
    RawModule module(fakePath_);
    QVERIFY(module.entry != nullptr);

    abi::ModuleHandshakeRequest request = goodRequest(abi::kFakeModuleId);
    request.runtimeTag ^= 1ull;  // a different Qt build on the two sides
    abi::ModuleHandshakeResponse response = emptyResponse();
    com::IComponentModule *root = nullptr;

    QCOMPARE(module.entry(&request, &response, &root), com::kUnsupportedVersion);
    QCOMPARE(root, nullptr);
    QCOMPARE(response.runtimeTag, marshal::runtimeTag());
}

void AbiContractTest::theHandshakeRefusesAStructSizeItDoesNotKnow() {
    RawModule module(fakePath_);
    QVERIFY(module.entry != nullptr);

    abi::ModuleHandshakeRequest request = goodRequest(abi::kFakeModuleId);
    request.structSize = static_cast<std::uint32_t>(sizeof(request)) - 8;
    abi::ModuleHandshakeResponse response = emptyResponse();
    com::IComponentModule *root = nullptr;

    QCOMPARE(module.entry(&request, &response, &root), com::kInvalidArgument);
    QCOMPARE(root, nullptr);

    // And the other direction: a response struct the module does not know is
    // refused rather than written past.
    abi::ModuleHandshakeRequest good = goodRequest(abi::kFakeModuleId);
    abi::ModuleHandshakeResponse shortResponse{};
    shortResponse.structSize = 8;
    QCOMPARE(module.entry(&good, &shortResponse, &root), com::kInvalidArgument);
    QCOMPARE(root, nullptr);
    QCOMPARE(shortResponse.structSize, 8u);  // untouched

    // EVERY error path, not just the compatible one. A null request and a null
    // output are refusals the module reaches before it has looked at anything,
    // and each of them used to write a full-size diagnostic into whatever it
    // was handed. A host built against a smaller struct would have had the
    // bytes after it overwritten - by the very call that exists to prevent
    // exactly that class of mistake.
    //
    // The sentinel is what makes the overrun visible: the trailing bytes are
    // ours, and they are checked, so "refused" and "refused without writing"
    // are two different observations.
    struct GuardedResponse {
        abi::ModuleHandshakeResponse response;
        std::uint64_t sentinel;
    };
    const std::uint64_t kSentinel = 0xA5A5'5A5A'C3C3'3C3Cull;

    GuardedResponse guarded{};
    guarded.response.structSize = 8;
    guarded.sentinel = kSentinel;
    QCOMPARE(module.entry(nullptr, &guarded.response, &root), com::kInvalidArgument);
    QCOMPARE(root, nullptr);
    QCOMPARE(guarded.response.structSize, 8u);
    QCOMPARE(guarded.sentinel, kSentinel);

    guarded = {};
    guarded.response.structSize = 8;
    guarded.sentinel = kSentinel;
    QCOMPARE(module.entry(&good, &guarded.response, nullptr), com::kInvalidArgument);
    QCOMPARE(guarded.response.structSize, 8u);
    QCOMPARE(guarded.sentinel, kSentinel);

    // A null request with a response the module DOES know is still answered,
    // because then there is somewhere safe to answer into. The refusal is
    // about the size, not about declining to diagnose.
    abi::ModuleHandshakeResponse wellSized = emptyResponse();
    QCOMPARE(module.entry(nullptr, &wellSized, &root), com::kInvalidArgument);
    QCOMPARE(root, nullptr);
    QCOMPARE(wellSized.status, com::kInvalidArgument);
    QCOMPARE(wellSized.moduleId, abi::kFakeModuleId);
}

void AbiContractTest::theLoaderRefusesAModuleThatIsNotTheOneItAskedFor() {
    // The test double exports the same symbol and passes every compatibility
    // check. Only its identity differs - and identity is what "no fallback in
    // production" rests on.
    clashqt::integration::ModuleLoader loader(fakePath_);
    QVERIFY(!loader.load(abi::kMihomoModuleId));
    QCOMPARE(loader.lastErrorCode(), com::kNotFound);
    QVERIFY(!loader.isLoaded());
    QVERIFY(loader.handshakeAnswered());
    QVERIFY(loader.handshake().moduleId == abi::kFakeModuleId);
}

void AbiContractTest::theLoaderRefusesAnArtifactItCannotSeeWithoutSearching() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    clashqt::integration::ModuleLoader missing(dir.filePath(QStringLiteral("absent.dylib")));
    QVERIFY(!missing.load(abi::kFakeModuleId));
    QCOMPARE(missing.lastErrorCode(), com::kNotFound);

    // A bare file name is NOT completed against PATH, the working directory's
    // neighbours or a build tree. The artifact exists - under another path -
    // and the loader still refuses.
    clashqt::integration::ModuleLoader bare(clashqt::integration::ModuleLoader::moduleFileName());
    QVERIFY(!bare.load(abi::kFakeModuleId));
    QCOMPARE(bare.lastErrorCode(), com::kNotFound);

    clashqt::integration::ModuleLoader empty(QString{});
    QVERIFY(!empty.load(abi::kFakeModuleId));
    QCOMPARE(empty.lastErrorCode(), com::kInvalidArgument);
}

// ------------------------------------------------- identity and lifetime

void AbiContractTest::theModuleRootAnswersItsIdentityAndRefusesAnUnknownClass() {
    clashqt::integration::ModuleLoader loader(fakePath_);
    QVERIFY(loader.load(abi::kFakeModuleId));

    com::IComponentModule *root = loader.module();
    QVERIFY(root != nullptr);

    com::ModuleId id{};
    QCOMPARE(root->GetModuleId(&id), com::kOk);
    QVERIFY(id == abi::kFakeModuleId);
    QCOMPARE(root->GetModuleId(nullptr), com::kInvalidArgument);
    QVERIFY(root->Description() != nullptr);
    QVERIFY(QString::fromUtf8(root->Description()).contains(QStringLiteral("fake")));

    // component-r1's CreateObject rules, on this module.
    QCOMPARE(root->CreateObject(abi::kBackendSessionClassId, abi::kIBackendSessionId, nullptr),
             com::kInvalidArgument);

    void *object = reinterpret_cast<void *>(0x1);
    QCOMPARE(root->CreateObject(abi::kFakeModuleId, abi::kIBackendSessionId, &object),
             com::kNotFound);
    QCOMPARE(object, nullptr);

    object = reinterpret_cast<void *>(0x1);
    QCOMPARE(root->CreateObject(abi::kBackendSessionClassId, com::kIErrorInfoId, &object),
             com::kNoInterface);
    QCOMPARE(object, nullptr);

    QVERIFY(loader.unload());
}

void AbiContractTest::aLiveObjectKeepsTheLibraryMapped() {
    clashqt::integration::ModuleLoader loader(fakePath_);
    QVERIFY(loader.load(abi::kFakeModuleId));
    QCOMPARE(loader.liveObjectCount(), 0);

    com::ComPtr<abi::IBackendSession> session;
    QVERIFY(loader.createSession(session));
    QCOMPARE(loader.liveObjectCount(), 1);

    // Unloading now would leave a vtable pointer into unmapped memory. The
    // loader refuses and says what it is still holding - which is the whole
    // difference between accounting and "never unload".
    QVERIFY(!loader.unload());
    QCOMPARE(loader.lastErrorCode(), com::kInvalidState);
    QVERIFY(loader.isLoaded());
    QVERIFY(loader.lastError().contains(QStringLiteral("still alive")));

    // The module stays usable while the object lives: refusing to unload is
    // not the same as shutting down.
    QCOMPARE(session->Close(abi::kCloseDefault, 0), com::kOk);
    session.Reset();

    QCOMPARE(loader.liveObjectCount(), 0);
    QVERIFY(loader.unload());
    QVERIFY(!loader.isLoaded());
}

void AbiContractTest::anActivatedShippingModuleIsActuallyUnmapped() {
    // The gate this case guards is "can the SHIPPING module be unloaded after
    // it has been used", and nothing weaker counts: an inert module unmapping
    // proves nothing about an activated one.
    //
    // It used to crash, and the crash was diagnosed rather than worked around.
    // The faulting address was a slot thunk in QtNetwork - not in the module -
    // reached from doActivate inside ~QCoreApplication, with QtNetwork no
    // longer in the image list. Unmapping the module had unmapped the shared
    // runtime the module brought in, and QtNetwork's lookup manager holds a
    // registration on QCoreApplication::destroyed that outlives its own image.
    // The module now pins that runtime, and its own image goes.
    {
        fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
        QVERIFY(fixture.load());
        QVERIFY(fixture.loader().isMapped());
        QVERIFY(fixture.unload());
        QVERIFY(!fixture.loader().isMapped());
        QVERIFY(!fixture.loader().isLoaded());
        // Asked of the platform loader, not of our own bookkeeping: isMapped()
        // says what this loader did, and this says what actually happened.
        QVERIFY2(!fixture.loader().isImageStillMappedInProcess(),
                 "the fake module's image is still mapped after a successful unload");
    }

    if (realPath_.isEmpty() || !QFileInfo::exists(realPath_)) {
        QSKIP("the shipping module artifact was not supplied to this suite");
    }

    // Loaded and never activated.
    {
        clashqt::integration::ModuleLoader idle(realPath_);
        QVERIFY(idle.load(abi::kMihomoModuleId));
        QVERIFY(idle.unload());
        QVERIFY(!idle.isMapped());
        QVERIFY(!idle.isImageStillMappedInProcess());
    }

    // ACTIVATED: a host is installed, so MihomoBackendImpl is constructed with
    // all of its QObject-derived collaborators. No engine is started here -
    // that is the sample consumer's job, and it drives a real one - but the
    // module has done everything that used to make the unmap fatal.
    {
        FakePrivilegedService service;
        fixtures::ModuleFixture fixture(realPath_, abi::kMihomoModuleId);
        QVERIFY(fixture.load(&service));
        QVERIFY(fixture.backend() != nullptr);
        QVERIFY(fixture.unload());
        QVERIFY(!fixture.loader().isLoaded());
        QVERIFY2(!fixture.loader().isMapped(),
                 "an activated shipping module must be unmapped, not pinned forever");
        QVERIFY2(!fixture.loader().isImageStillMappedInProcess(),
                 "the shipping module's image survived a successful unload");
    }
}

void AbiContractTest::aModuleThatRefusesTheUnmapKeepsItsImageMapped() {
    // The other arm: a module CAN still answer "quiescent, but do not unmap
    // me", and the loader has to honour it rather than treat kFalse as a
    // rounding error. The double is asked to present that answer, because the
    // shipping module no longer does - which is the point.
    qputenv("CLASHQT_FAKE_MODULE_UNPINNABLE", "1");
    {
        fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
        QVERIFY(fixture.load());
        QVERIFY(fixture.unload());
        // The release succeeded - the caller's question is "am I done with this
        // module", and it is - but the image stayed.
        QVERIFY(!fixture.loader().isLoaded());
        QVERIFY(fixture.loader().isMapped());
        QVERIFY(fixture.loader().isImageStillMappedInProcess());
        QVERIFY(fixture.loader().lastError().contains(QStringLiteral("pin")));
    }
    qunsetenv("CLASHQT_FAKE_MODULE_UNPINNABLE");
    reclaimLeakedMapping(fakePath_);

    // And with the variable gone the same artifact unmaps again, so the case
    // above is testing the answer and not some permanent property of the file.
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());
    QVERIFY(fixture.unload());
    QVERIFY(!fixture.loader().isMapped());
}

void AbiContractTest::aRetainedRootReferenceKeepsTheImageMapped() {
    // LiveObjectCount deliberately excludes the root, so the count cannot be
    // what protects it. A consumer that kept an IModuleLifetime from
    // QueryInterface holds a vtable in this image exactly as a session would,
    // and the loader finds out because its own final Release does not reach
    // zero.
    clashqt::integration::ModuleLoader loader(fakePath_);
    QVERIFY(loader.load(abi::kFakeModuleId));

    com::ComPtr<abi::IModuleLifetime> retained;
    QCOMPARE(loader.module()->QueryInterface(abi::kIModuleLifetimeId, retained.PutVoid()), com::kOk);
    QCOMPARE(retained->LiveObjectCount(), 0);

    QVERIFY(loader.unload());
    QVERIFY(!loader.isLoaded());
    QVERIFY2(loader.isMapped(), "a root reference someone else holds must keep the image");
    QVERIFY(loader.lastError().contains(QStringLiteral("module root")));
    // The retained interface is still usable, which is the whole reason the
    // image had to stay.
    QCOMPARE(retained->LiveObjectCount(), 0);
    retained.Reset();
    reclaimLeakedMapping(fakePath_);
}

void AbiContractTest::aRetainedReplyBufferKeepsTheModuleCountedAndMapped() {
    // A reply buffer is produced BY the module and freed through the module's
    // vtable. Releasing the session it came from does not make it safe to
    // unmap the image, and a session-only count would say it did.
    clashqt::integration::ModuleLoader loader(fakePath_);
    QVERIFY(loader.load(abi::kFakeModuleId));

    com::ComPtr<abi::IBackendSession> session;
    QVERIFY(loader.createSession(session));
    QCOMPARE(loader.liveObjectCount(), 1);
    auto *host = new CountingHost();
    QCOMPARE(session->SetHost(host), com::kOk);

    com::IBuffer *reply = nullptr;
    QCOMPARE(session->Invoke(abi::kCmdBinaryPath, nullptr, 0, &reply), com::kOk);
    QVERIFY(reply != nullptr);
    QCOMPARE(loader.liveObjectCount(), 2);  // the session AND its reply

    // A reply from the TEST-CONTROL range is a module-produced object too. The
    // double allocating one that nobody counted would let it be unmapped with
    // one of its own buffers still in a caller's hands - and then this suite
    // would be proving the accounting against a module exempt from it.
    marshal::ByteWriter pendingArgs;
    pendingArgs.u64(0);
    com::IBuffer *controlReply = nullptr;
    QCOMPARE(session->Invoke(fixtures::kFakeIsPending, pendingArgs.data().data(),
                             pendingArgs.size(), &controlReply),
             com::kOk);
    QVERIFY(controlReply != nullptr);
    QCOMPARE(loader.liveObjectCount(), 3);

    QCOMPARE(session->Close(abi::kCloseDefault, 0), com::kOk);
    session.Reset();
    host->Release();
    QCOMPARE(loader.liveObjectCount(), 2);

    QVERIFY(!loader.unload());
    controlReply->Release();
    QCOMPARE(loader.liveObjectCount(), 1);  // the ordinary reply buffer, alone

    QVERIFY(!loader.unload());
    QCOMPARE(loader.lastErrorCode(), com::kInvalidState);
    QVERIFY(loader.isMapped());

    // Still readable: the object outlived its session exactly as the ABI
    // permits, because the module is still there to serve it.
    QVERIFY(reply->Size() >= 4);
    reply->Release();

    QCOMPARE(loader.liveObjectCount(), 0);
    QVERIFY(loader.unload());
    QVERIFY(!loader.isMapped());
    QVERIFY(!loader.isImageStillMappedInProcess());
}

void AbiContractTest::aRetainedDiagnosticAndItsMessageBufferEachKeepTheModule() {
    clashqt::integration::ModuleLoader loader(fakePath_);
    QVERIFY(loader.load(abi::kFakeModuleId));

    com::ComPtr<abi::IBackendSession> session;
    QVERIFY(loader.createSession(session));
    auto *host = new CountingHost();
    QCOMPARE(session->SetHost(host), com::kOk);

    // Provoke a diagnostic: an argument block longer than the command's own
    // arguments, which is the failure the session records a message for.
    // setBinaryPath rather than setMode, because setMode submits a request
    // before the trailing bytes are noticed and Close would then report the
    // timeout instead of the value under test.
    marshal::ByteWriter overlong;
    overlong.text(QStringLiteral("/usr/local/bin/mihomo"));
    overlong.u32(0xDEADBEEF);
    QCOMPARE(
        session->Invoke(abi::kCmdSetBinaryPath, overlong.data().data(), overlong.size(), nullptr),
        com::kInvalidArgument);

    com::IErrorInfo *error = nullptr;
    QCOMPARE(session->LastError(&error), com::kOk);
    QVERIFY(error != nullptr);
    QCOMPARE(loader.liveObjectCount(), 2);  // session + diagnostic

    // The NESTED buffer is a separate module-owned object and counts in its own
    // right - it can outlive the diagnostic that produced it.
    com::IBuffer *message = nullptr;
    QCOMPARE(error->GetMessage(&message), com::kOk);
    QVERIFY(message != nullptr);
    QCOMPARE(loader.liveObjectCount(), 3);

    QCOMPARE(session->Close(abi::kCloseDefault, 0), com::kOk);
    session.Reset();
    host->Release();
    QCOMPARE(loader.liveObjectCount(), 2);

    error->Release();
    QCOMPARE(loader.liveObjectCount(), 1);  // the message buffer, alone
    QVERIFY2(!loader.unload(), "a message buffer outliving its diagnostic must block the unload");
    QVERIFY(loader.isMapped());

    QVERIFY(message->Size() > 0);
    message->Release();
    QCOMPARE(loader.liveObjectCount(), 0);
    QVERIFY(loader.unload());
    QVERIFY(!loader.isMapped());
}

void AbiContractTest::creatingAndPreparingInParallelNeverHandsOutAnObjectAfterPrepare() {
    // component-r1 makes the module root free-threaded, so "read the count,
    // then latch the refusal" is a check-to-act race: a creation that slipped
    // between the two would hand out an object into an image the loader is
    // already unmapping. The two must move under one atomic transition, and
    // this case is what makes the difference observable.
    for (int attempt = 0; attempt < 24; ++attempt) {
        clashqt::integration::ModuleLoader loader(fakePath_);
        QVERIFY(loader.load(abi::kFakeModuleId));
        com::IComponentModule *root = loader.module();
        com::ComPtr<abi::IModuleLifetime> lifetime;
        QCOMPARE(root->QueryInterface(abi::kIModuleLifetimeId, lifetime.PutVoid()), com::kOk);

        std::atomic<bool> go{false};
        std::vector<abi::IBackendSession *> created;
        std::mutex createdGuard;
        com::Result prepared = com::kFail;

        std::thread creator([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 64; ++i) {
                abi::IBackendSession *session = nullptr;
                if (com::IsSuccess(root->CreateObject(abi::kBackendSessionClassId,
                                                      abi::kIBackendSessionId,
                                                      reinterpret_cast<void **>(&session))) &&
                    session != nullptr) {
                    const std::lock_guard<std::mutex> lock(createdGuard);
                    created.push_back(session);
                }
            }
        });
        std::thread preparer([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            prepared = lifetime->PrepareUnload();
        });
        go.store(true, std::memory_order_release);
        creator.join();
        preparer.join();

        if (prepared == com::kOk || prepared == com::kFalse) {
            // It said the module was quiescent. Then nothing may have been
            // handed out that is still alive - which is the invariant the
            // single atomic transition exists to keep.
            QCOMPARE(lifetime->LiveObjectCount(), 0);
            const std::lock_guard<std::mutex> lock(createdGuard);
            QVERIFY2(created.empty(),
                     "PrepareUnload answered 'quiescent' while a session was being handed out");
        } else {
            // It refused because objects existed. Nothing is latched, so the
            // module is still usable - and everything handed out is accounted.
            QCOMPARE(prepared, com::kInvalidState);
            const std::lock_guard<std::mutex> lock(createdGuard);
            QCOMPARE(lifetime->LiveObjectCount(), static_cast<std::int32_t>(created.size()));
        }

        {
            const std::lock_guard<std::mutex> lock(createdGuard);
            for (abi::IBackendSession *session : created) {
                session->Close(abi::kCloseDefault, 0);
                session->Release();
            }
            created.clear();
        }
        lifetime.Reset();
        QVERIFY(loader.unload());
    }
}

void AbiContractTest::aPreparedModuleCreatesNothingFurther() {
    // PrepareUnload is a point of no return for creation, and it has to be:
    // a loader that asked "may I unmap" and then got handed one more object
    // would unmap an image that is in use. The count answers the first half of
    // that question; this refusal answers the second.
    clashqt::integration::ModuleLoader loader(fakePath_);
    QVERIFY(loader.load(abi::kFakeModuleId));

    com::ComPtr<abi::IModuleLifetime> lifetime;
    QCOMPARE(loader.module()->QueryInterface(abi::kIModuleLifetimeId, lifetime.PutVoid()),
             com::kOk);
    QCOMPARE(lifetime->LiveObjectCount(), 0);
    QCOMPARE(lifetime->PrepareUnload(), com::kOk);
    // Idempotent, and still prepared.
    QCOMPARE(lifetime->PrepareUnload(), com::kAlreadyClosed);

    void *object = reinterpret_cast<void *>(0x1);
    QCOMPARE(loader.module()->CreateObject(abi::kBackendSessionClassId, abi::kIBackendSessionId,
                                           &object),
             com::kInvalidState);
    QCOMPARE(object, nullptr);
    QCOMPARE(lifetime->LiveObjectCount(), 0);

    lifetime.Reset();
    QVERIFY(loader.unload());
}

void AbiContractTest::theReaderLatchesAnOverrunInsteadOfContinuing() {
    // The decoder's first line of defence, asserted directly rather than
    // through a command. A reader that returns a default for an overrun but
    // does not LATCH keeps going, so a truncated payload is decoded as a
    // record of zeroes and delivered as if it were real.
    marshal::ByteWriter out;
    out.u32(7);

    marshal::ByteReader in(out.data().data(), out.size());
    QCOMPARE(in.u32(), 7u);
    QVERIFY(in.ok());
    QVERIFY(in.finished());

    QCOMPARE(in.u64(), 0ull);   // past the end
    QVERIFY(!in.ok());          // and it stays failed
    QVERIFY(!in.finished());
    QCOMPARE(in.u8(), 0);
    QVERIFY(!in.ok());
    QCOMPARE(in.remaining(), std::size_t{0});

    // A length prefix that claims more than remains is refused before anything
    // is allocated, and a count is measured against what one element must cost.
    marshal::ByteWriter lying;
    lying.u32(0xFFFFFF);
    marshal::ByteReader liar(lying.data().data(), lying.size());
    QVERIFY(liar.text().isEmpty());
    QVERIFY(!liar.ok());

    marshal::ByteWriter absurd;
    absurd.u32(1000000);
    marshal::ByteReader counted(absurd.data().data(), absurd.size());
    QCOMPARE(counted.count(marshal::kMinRuleBytes), 0u);
    QVERIFY(!counted.ok());
}

void AbiContractTest::closingIsIdempotentAndAClosedSessionRefusesCommands() {
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());

    QVERIFY(fixture.backend()->isValid());
    QVERIFY(fixture.backend()->shutdown(/*stopManagedCore=*/false, 500));
    QVERIFY(!fixture.backend()->isValid());
    // Idempotent: a second close is kAlreadyClosed, which is not an error the
    // caller has to guard against.
    QVERIFY(fixture.backend()->shutdown(/*stopManagedCore=*/false, 500));

    // A closed session performs nothing, and says so rather than answering
    // with a stale value.
    QCOMPARE(fixture.control(abi::kCmdGeneration), com::kAlreadyClosed);
    QCOMPARE(fixture.backend()->refreshVersion(), cb::RequestId::Invalid);
    QCOMPARE(fixture.backend()->lastError().code, cb::ErrorCode::InvalidState);
}

void AbiContractTest::outstandingWorkCountsHeldRequestsAndCloseReportsTheTimeout() {
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());

    QCOMPARE(fixture.backend()->outstandingWork(), 0);
    fixture.setRequestGate(/*held=*/true);

    const cb::RequestId request = fixture.backend()->refreshVersion();
    QVERIFY(request != cb::RequestId::Invalid);
    QCOMPARE(fixture.backend()->outstandingWork(), 1);
    QVERIFY(fixture.isPending(request));

    // Close with a zero deadline cannot settle it. An honest kTimeout, not a
    // kOk that pretends the work finished.
    QVERIFY(!fixture.backend()->shutdown(/*stopManagedCore=*/false, 0));
    QCOMPARE(fixture.backend()->lastError().code, cb::ErrorCode::Timeout);
}

// ----------------------------------------------------------- marshalling

void AbiContractTest::sixtyFourBitIdentityAndCountersSurviveTheBoundary() {
    // module-r1: "all operations/events of backend-r4 must round-trip
    // losslessly including 64-bit IDs/counters". A 32-bit field anywhere in
    // the chain turns a long-lived session's request ids into collisions.
    const auto hugeRequest = static_cast<cb::RequestId>(0xFEDC'BA98'7654'3210ull);
    const auto hugeGeneration = static_cast<cb::Generation>(0x0123'4567'89AB'CDEFull);

    marshal::ByteWriter out;
    cb::Completion completion;
    completion.request = hugeRequest;
    completion.generation = hugeGeneration;
    completion.status = cb::CompletionStatus::Superseded;
    completion.error = {cb::ErrorCode::Superseded, QStringLiteral("invalidated")};
    marshal::writeCompletion(out, completion);

    marshal::ByteReader in(out.data().data(), out.size());
    const cb::Completion decoded = marshal::readCompletion(in);
    QVERIFY(in.finished());
    QCOMPARE(cb::number(decoded.request), cb::number(hugeRequest));
    QCOMPARE(cb::number(decoded.generation), cb::number(hugeGeneration));
    QCOMPARE(decoded.status, cb::CompletionStatus::Superseded);
    QCOMPARE(decoded.error.code, cb::ErrorCode::Superseded);
    QCOMPARE(decoded.error.message, completion.error.message);

    // The supersession marking amendment A2 added is a value on the wire, not
    // something the host infers: a codec that dropped it would leave the
    // consumer's only usable check unfired.
    QVERIFY(cb::isSuperseded(cb::Generation::Initial, hugeGeneration));
}

void AbiContractTest::everyQueryOfTheContractCrossesTheBoundary() {
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());
    cb::MihomoBackend *backend = fixture.contractBackend();

    // Identity and capabilities.
    const cb::BackendIdentity identity = backend->identity();
    QCOMPARE(identity.interfaceRevision, abi::kBackendInterfaceRevision);
    QVERIFY(!identity.name.isEmpty());

    // The timings ARE the contract (backend-r4 section 3); a boundary that
    // lost one would let a consumer size its progress reporting against a
    // budget the backend does not have.
    const cb::BackendTimings timings = backend->timings();
    QCOMPARE(timings.idleDeadlineMs, cb::kContractTimings.idleDeadlineMs);
    QCOMPARE(timings.serviceIdleDeadlineMs, cb::kContractTimings.serviceIdleDeadlineMs);
    QCOMPARE(timings.hardCapMs, cb::kContractTimings.hardCapMs);
    QCOMPARE(timings.probeIntervalMs, cb::kContractTimings.probeIntervalMs);
    QCOMPARE(timings.probeTimeoutMs, cb::kContractTimings.probeTimeoutMs);
    QCOMPARE(timings.terminateWaitMs, cb::kContractTimings.terminateWaitMs);

    QVERIFY(backend->features().bits != 0);
    QCOMPARE(backend->state(), cb::CoreState::Stopped);
    QCOMPARE(backend->ownership(), cb::Ownership::None);
    QCOMPARE(backend->attachmentOwnership(), cb::Ownership::None);
    QVERIFY(!backend->isAttached());
    QVERIFY(!backend->isManagedCoreActive());
    QVERIFY(!backend->isRestartPending());

    // A setter and its getter, so the round trip is observable in both
    // directions rather than inferred from one.
    const QString path = QStringLiteral("/opt/mihomo/bin/mihomo");
    backend->setBinaryPath(path);
    QCOMPARE(backend->binaryPath(), path);
    QVERIFY(!backend->discoverBinary().isEmpty());

    // An endpoint by value, with a non-ASCII secret so the UTF-8 encoding is
    // exercised rather than assumed.
    cb::Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = 29097;
    endpoint.secret = QStringLiteral("sécrèt-χ");
    fixture.addExternalController(endpoint);
    QVERIFY(backend->attach(endpoint) != cb::RequestId::Invalid);
    QVERIFY(fixture.backend()->drain(1000));
    QCOMPARE(backend->currentEndpoint().host, endpoint.host);
    QCOMPARE(backend->currentEndpoint().port, endpoint.port);
    QCOMPARE(backend->currentEndpoint().secret, endpoint.secret);
    QVERIFY(backend->isAttached());
}

void AbiContractTest::stagedTelemetryRoundTripsThroughAReleasedRequest() {
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());
    BoundaryObserver observer(fixture.backend());
    QVERIFY(fixture.contractBackend()->addObserver(&observer));

    fixture.setRequestGate(true);
    const QString version = QStringLiteral("v1.19.31-Δ");
    fixture.stageVersion(version);

    // Rules, with a payload that is not ASCII and a delay that distinguishes
    // "untested" (-1) from "timeout" (0).
    marshal::ByteWriter rules;
    rules.u32(1);
    cb::Rule rule;
    rule.type = QStringLiteral("DOMAIN-SUFFIX");
    rule.payload = QStringLiteral("例え.テスト");
    rule.proxy = QStringLiteral("Proxy");
    marshal::writeRule(rules, rule);
    QCOMPARE(fixture.control(fixtures::kFakeStageRules, rules), com::kOk);

    marshal::ByteWriter proxies;
    proxies.u32(1);
    cb::ProxyGroup group;
    group.name = QStringLiteral("Proxy");
    group.type = QStringLiteral("Selector");
    group.now = QStringLiteral("A");
    group.all = QStringList{QStringLiteral("A"), QStringLiteral("B")};
    marshal::writeProxyGroup(proxies, group);
    proxies.u32(2);
    cb::ProxyNode tested;
    tested.name = QStringLiteral("A");
    tested.type = QStringLiteral("ss");
    tested.delay = 0;  // timeout
    marshal::writeProxyNode(proxies, tested);
    cb::ProxyNode untested;
    untested.name = QStringLiteral("B");
    untested.type = QStringLiteral("vmess");
    untested.delay = -1;  // never measured
    marshal::writeProxyNode(proxies, untested);
    QCOMPARE(fixture.control(fixtures::kFakeStageProxies, proxies), com::kOk);

    const cb::RequestId versionRequest = fixture.contractBackend()->refreshVersion();
    const cb::RequestId rulesRequest = fixture.contractBackend()->refreshRules();
    const cb::RequestId proxiesRequest = fixture.contractBackend()->refreshProxies();
    QVERIFY(fixture.releaseRequest(versionRequest));
    QVERIFY(fixture.releaseRequest(rulesRequest));
    QVERIFY(fixture.releaseRequest(proxiesRequest));
    QVERIFY(fixture.backend()->drain(2000));

    QCOMPARE(observer.versionTexts.size(), 1);
    QCOMPARE(observer.versionTexts.first(), version);
    QCOMPARE(cb::number(observer.versions.front().request), cb::number(versionRequest));

    QCOMPARE(observer.rules.size(), 1);
    QCOMPARE(observer.rules.first().payload, rule.payload);

    QCOMPARE(observer.groups.size(), 1);
    QCOMPARE(observer.groups.first().all, group.all);
    QCOMPARE(observer.nodes.size(), 2);
    QCOMPARE(observer.nodes.at(0).delay, 0);
    QCOMPARE(observer.nodes.at(1).delay, -1);

    QCOMPARE(observer.sawReentrantDelivery, false);
    fixture.contractBackend()->removeObserver(&observer);
}

void AbiContractTest::aPackedConnectionSnapshotRoundTripsExactly() {
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());
    BoundaryObserver observer(fixture.backend());
    QVERIFY(fixture.contractBackend()->addObserver(&observer));

    const cb::Connection first =
        makeConnection(QStringLiteral("c-1"), QStringList{QStringLiteral("Proxy"),
                                                          QStringLiteral("DIRECT")});
    const cb::Connection second = makeConnection(QStringLiteral("c-2"), QStringList{});

    marshal::ByteWriter args;
    marshal::writeGeneration(args, static_cast<cb::Generation>(0x1'0000'0002ull));
    args.u32(2);
    marshal::writeConnection(args, first);
    marshal::writeConnection(args, second);
    args.u64(0x0000'0002'0000'0003ull);
    args.u64(0x0000'0004'0000'0005ull);
    QCOMPARE(fixture.control(fixtures::kFakeEmitConnections, args), com::kOk);
    QVERIFY(fixture.backend()->drain(1000));

    QCOMPARE(observer.connectionSets.size(), std::size_t{1});
    const auto &event = observer.connectionSets.front();
    QCOMPARE(cb::number(event.generation), 0x1'0000'0002ull);
    QCOMPARE(event.uploadTotal, 0x0000'0002'0000'0003ull);
    QCOMPARE(event.downloadTotal, 0x0000'0004'0000'0005ull);
    QCOMPARE(event.connections.size(), 2);

    const cb::Connection &decoded = event.connections.at(0);
    QCOMPARE(decoded.id, first.id);
    QCOMPARE(decoded.chains, first.chains);
    QCOMPARE(decoded.upload, first.upload);
    QCOMPARE(decoded.download, first.download);
    // The rates are bit patterns, not re-parsed text: 0.1 does not survive a
    // decimal round trip.
    QCOMPARE(decoded.uploadRate, first.uploadRate);
    QCOMPARE(decoded.downloadRate, first.downloadRate);
    QCOMPARE(decoded.start, first.start);
    QVERIFY(!decoded.end.isValid());
    QCOMPARE(decoded.processPath, first.processPath);
    QVERIFY(event.connections.at(1).chains.isEmpty());

    fixture.contractBackend()->removeObserver(&observer);
}

void AbiContractTest::aMalformedConnectionSnapshotIsRefusedWithAReason() {
    const cb::Connection one = makeConnection(QStringLiteral("c"), QStringList{QStringLiteral("P")});
    const QVector<cb::Connection> connections{one};
    std::vector<std::uint8_t> packed = marshal::packConnectionSnapshot(
        cb::Generation::Initial, cb::makeSpan(connections), 1, 2);

    marshal::ConnectionSnapshot snapshot;
    QString reason;
    QVERIFY(marshal::unpackConnectionSnapshot(packed.data(), packed.size(), &snapshot, &reason));
    QCOMPARE(snapshot.connections.size(), 1);

    // Every rejection names its cause: "malformed packet" with no detail is
    // how a producer bug survives to the next release.
    const auto header = reinterpret_cast<abi::ConnectionSnapshotHeader *>(packed.data());

    std::vector<std::uint8_t> broken = packed;
    reinterpret_cast<abi::ConnectionSnapshotHeader *>(broken.data())->magic ^= 0xFF;
    QVERIFY(!marshal::unpackConnectionSnapshot(broken.data(), broken.size(), &snapshot, &reason));
    QVERIFY(reason.contains(QStringLiteral("magic")));

    broken = packed;
    reinterpret_cast<abi::ConnectionSnapshotHeader *>(broken.data())->recordSize += 8;
    QVERIFY(!marshal::unpackConnectionSnapshot(broken.data(), broken.size(), &snapshot, &reason));
    QVERIFY(reason.contains(QStringLiteral("record size")));

    // An offset just past the end of the blob. This is the one a naive
    // decoder reads straight out of bounds.
    broken = packed;
    auto *record = reinterpret_cast<abi::ConnectionRecord *>(broken.data() + header->recordOffset);
    record->host.offset = header->blobSize;
    record->host.length = 1;
    QVERIFY(!marshal::unpackConnectionSnapshot(broken.data(), broken.size(), &snapshot, &reason));
    QVERIFY(reason.contains(QStringLiteral("outside the blob")));

    broken = packed;
    record = reinterpret_cast<abi::ConnectionRecord *>(broken.data() + header->recordOffset);
    record->chainCount = header->chainSlots + 1;
    QVERIFY(!marshal::unpackConnectionSnapshot(broken.data(), broken.size(), &snapshot, &reason));
    QVERIFY(reason.contains(QStringLiteral("chain run")));

    broken = packed;
    broken.push_back(0);  // trailing bytes that belong to nothing
    QVERIFY(!marshal::unpackConnectionSnapshot(broken.data(), broken.size(), &snapshot, &reason));

    QVERIFY(!marshal::unpackConnectionSnapshot(packed.data(), 4, &snapshot, &reason));
    QVERIFY(!marshal::unpackConnectionSnapshot(nullptr, 0, &snapshot, &reason));
}

void AbiContractTest::thePrivilegedStatusCarriesCoreRunning() {
    // backend-r4 C1. The field whose absence left the uninstall guard inert
    // for a whole wave; a boundary that dropped it would arm nothing and say
    // nothing.
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());
    BoundaryObserver observer(fixture.backend());
    QVERIFY(fixture.contractBackend()->addObserver(&observer));

    cb::PrivilegedServiceStatus status;
    status.state = cb::ServiceState::Connected;
    status.version = QStringLiteral("1.4.2");
    status.coreRunning = true;
    marshal::ByteWriter args;
    marshal::writePrivilegedServiceStatus(args, status);
    QCOMPARE(fixture.control(fixtures::kFakeSetServiceStatus, args), com::kOk);

    QVERIFY(fixture.contractBackend()->requestPrivilegedServiceStatus() != cb::RequestId::Invalid);
    QVERIFY(fixture.backend()->drain(1000));

    QCOMPARE(observer.serviceStatuses.size(), std::size_t{1});
    QCOMPARE(observer.serviceStatuses.front().state, cb::ServiceState::Connected);
    QCOMPARE(observer.serviceStatuses.front().version, status.version);
    QCOMPARE(observer.serviceStatuses.front().coreRunning, true);

    fixture.contractBackend()->removeObserver(&observer);
}

// --------------------------------------------------------- delivery rules

void AbiContractTest::noObserverIsInvokedFromInsideACommand() {
    // backend-r4 section 7, and the half D8 says the Qt event loop provides
    // for free in process and that "across a raw ABI would have to be
    // re-established".
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());
    BoundaryObserver observer(fixture.backend());
    QVERIFY(fixture.contractBackend()->addObserver(&observer));

    cb::Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = 29098;
    fixture.addExternalController(endpoint);

    // attach() clears live state and re-issues the snapshot set, so it is the
    // call that produces the most events from inside one command.
    QVERIFY(fixture.contractBackend()->attach(endpoint) != cb::RequestId::Invalid);
    QVERIFY(fixture.contractBackend()->detach() != cb::RequestId::Invalid);
    QVERIFY(fixture.backend()->drain(2000));

    QVERIFY(observer.callbacks > 0);
    QCOMPARE(observer.sawReentrantDelivery, false);
    fixture.contractBackend()->removeObserver(&observer);
}

void AbiContractTest::removingAnObserverDuringDeliveryStopsFurtherCallbacks() {
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());

    BoundaryObserver first(fixture.backend());
    BoundaryObserver second(fixture.backend());
    QVERIFY(fixture.contractBackend()->addObserver(&first));
    QVERIFY(fixture.contractBackend()->addObserver(&second));
    QVERIFY(!fixture.contractBackend()->addObserver(&second));  // already registered
    QVERIFY(!fixture.contractBackend()->addObserver(nullptr));

    // The first observer removes the second from inside its own first
    // callback. The second must receive nothing after that, including events
    // already queued.
    first.onNextEvent = [&] { fixture.contractBackend()->removeObserver(&second); };

    cb::Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = 29099;
    fixture.addExternalController(endpoint);
    QVERIFY(fixture.contractBackend()->attach(endpoint) != cb::RequestId::Invalid);
    QVERIFY(fixture.backend()->drain(2000));

    QVERIFY(first.callbacks > 1);
    QCOMPARE(second.callbacks, 0);
    QVERIFY(!fixture.contractBackend()->removeObserver(&second));
    QVERIFY(fixture.contractBackend()->removeObserver(&first));
}

namespace {

/// What a LATE observer saw: how many callbacks of the batch that was already
/// produced when it registered, and how many of a batch produced afterwards.
///
/// Both numbers matter, and the second one is not decoration. A module that
/// stamped every event with a constant would pass the first check by accident -
/// nothing is ever admitted - and a rule that admits nothing is not the rule
/// backend-r4 states. The pair is what tells "excluded correctly" apart from
/// "excluded always".
struct LateObserverOutcome {
    int fromTheEarlierBatch = -1;
    int fromALaterBatch = -1;
};

/// Written once and run against both backends, so what is compared is two
/// answers rather than two different scripts.
///
/// attach/detach are the cheapest things in backend-r4 that produce several
/// events each and settle with no privileged access and no network.
LateObserverOutcome lateObserverOutcome(cb::MihomoBackend &backend, BoundaryObserver &early,
                                        BoundaryObserver &late,
                                        const std::function<void()> &stageController,
                                        const std::function<bool()> &drain,
                                        bool addDuringDelivery) {
    backend.addObserver(&early);
    stageController();

    cb::Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = 29098;
    if (addDuringDelivery) {
        // Registered from inside the FIRST callback of the batch, so some of
        // the events it might have received are still queued behind it.
        early.onNextEvent = [&] { backend.addObserver(&late); };
        backend.attach(endpoint);
        drain();
    } else {
        // Registered after production and before any delivery: every event of
        // the batch exists already, and none of them has been handed out.
        backend.attach(endpoint);
        backend.addObserver(&late);
        drain();
    }

    LateObserverOutcome outcome;
    outcome.fromTheEarlierBatch = late.callbacks;

    early.onNextEvent = nullptr;
    const int alreadySeen = late.callbacks;
    backend.detach();
    drain();
    outcome.fromALaterBatch = late.callbacks - alreadySeen;
    return outcome;
}

}  // namespace

void AbiContractTest::anObserverAddedAfterProductionIsRefusedTheSameEventsAsDirect() {
    // backend-r4 admits by PRODUCTION, not by arrival. In process those are the
    // same instant. Across the boundary they are not: the module produces and
    // queues, the host receives later, and a host that stamped events when they
    // arrived would hand a brand-new observer a batch that predates it.
    //
    // The two answers are compared rather than asserted separately, because the
    // claim being made is "the module-backed backend behaves like the direct
    // one" and only a comparison can fail for the right reason.
    LateObserverOutcome direct;
    {
        testsupport::backend::FakeBackend backend;
        BoundaryObserver early;
        BoundaryObserver late;
        direct = lateObserverOutcome(
            backend, early, late,
            [&] {
                cb::Endpoint endpoint;
                endpoint.host = QStringLiteral("127.0.0.1");
                endpoint.port = 29098;
                backend.addExternalController(endpoint);
            },
            [&] { return backend.flushEvents(2000); }, /*addDuringDelivery=*/false);
        QVERIFY(early.callbacks > 0);
    }

    LateObserverOutcome viaModule;
    {
        fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
        QVERIFY(fixture.load());
        BoundaryObserver early(fixture.backend());
        BoundaryObserver late(fixture.backend());
        viaModule = lateObserverOutcome(
            *fixture.contractBackend(), early, late,
            [&] {
                cb::Endpoint endpoint;
                endpoint.host = QStringLiteral("127.0.0.1");
                endpoint.port = 29098;
                fixture.addExternalController(endpoint);
            },
            [&] { return fixture.backend()->drain(2000); }, /*addDuringDelivery=*/false);
        QVERIFY(early.callbacks > 0);
    }

    QCOMPARE(direct.fromTheEarlierBatch, 0);
    QVERIFY2(direct.fromALaterBatch > 0, "the direct backend must deliver what came AFTER");
    QCOMPARE(viaModule.fromTheEarlierBatch, direct.fromTheEarlierBatch);
    QCOMPARE(viaModule.fromALaterBatch, direct.fromALaterBatch);
}

void AbiContractTest::anObserverAddedDuringDeliveryIsAdmittedTheSameAsDirect() {
    LateObserverOutcome direct;
    {
        testsupport::backend::FakeBackend backend;
        BoundaryObserver early;
        BoundaryObserver late;
        direct = lateObserverOutcome(
            backend, early, late,
            [&] {
                cb::Endpoint endpoint;
                endpoint.host = QStringLiteral("127.0.0.1");
                endpoint.port = 29098;
                backend.addExternalController(endpoint);
            },
            [&] { return backend.flushEvents(2000); }, /*addDuringDelivery=*/true);
        QVERIFY(early.callbacks > 0);
    }

    LateObserverOutcome viaModule;
    {
        fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
        QVERIFY(fixture.load());
        BoundaryObserver early(fixture.backend());
        BoundaryObserver late(fixture.backend());
        viaModule = lateObserverOutcome(
            *fixture.contractBackend(), early, late,
            [&] {
                cb::Endpoint endpoint;
                endpoint.host = QStringLiteral("127.0.0.1");
                endpoint.port = 29098;
                fixture.addExternalController(endpoint);
            },
            [&] { return fixture.backend()->drain(2000); }, /*addDuringDelivery=*/true);
        QVERIFY(early.callbacks > 0);
    }

    // Registered mid-batch, so it misses the rest of that batch on BOTH sides -
    // and still receives everything produced afterwards, which is what makes
    // this an admission rule rather than a mute button.
    QCOMPARE(direct.fromTheEarlierBatch, 0);
    QVERIFY(direct.fromALaterBatch > 0);
    QCOMPARE(viaModule.fromTheEarlierBatch, direct.fromTheEarlierBatch);
    QCOMPARE(viaModule.fromALaterBatch, direct.fromALaterBatch);
}

// ------------------------------------------------------- the reverse seam

void AbiContractTest::thePrivilegedSeamReachesTheHostsServiceAndAllowsOneListener() {
    // Driven against the SHIPPING module, because it is the only backend that
    // actually uses the seam: the deterministic fake models the service with
    // its own knobs. Nothing here touches the installed helper - the service
    // is the host's own double, and the engine is never launched.
    if (realPath_.isEmpty() || !QFileInfo::exists(realPath_)) {
        QSKIP("the shipping module artifact was not supplied to this suite");
    }

    FakePrivilegedService service;
    service.supported = true;
    service.available = false;
    service.error = QStringLiteral("socket refused");

    {
        fixtures::ModuleFixture fixture(realPath_, abi::kMihomoModuleId);
        QVERIFY(fixture.load(&service));

        // The component attached itself as THE listener while it was being
        // built, through the reverse interface. ONE connection: decision D3
        // removed a second live client to one privileged socket, and the
        // boundary does not get to reintroduce one.
        QCOMPARE(service.listener() != nullptr, true);
        QCOMPARE(service.listenerChanges, 1);

        // A forward query crosses the boundary twice - host command into the
        // module, module adapter back out to the host's service - and returns
        // the host's answer, not a cached one.
        QCOMPARE(fixture.contractBackend()->serviceSupported(), true);
        QCOMPARE(fixture.contractBackend()->serviceAvailable(), false);
        service.supported = false;
        QCOMPARE(fixture.contractBackend()->serviceSupported(), false);
        service.supported = true;

        // A listener callback travels the other way without a Qt type crossing
        // and without unwinding.
        service.listener()->privilegedConnectedChanged(true);
        service.listener()->privilegedRequestFinished(QStringLiteral("install"), false,
                                                      QStringLiteral("denied"));
        QVERIFY(fixture.backend()->drain(500));

        // Close detaches the listener and closes the lease BEFORE the host
        // reference is released, so a late callback cannot reach a destroyed
        // module object.
        QVERIFY(fixture.backend()->shutdown(/*stopManagedCore=*/true, 1000));
        QCOMPARE(service.listener(), nullptr);
        QCOMPARE(service.listenerChanges, 2);
        QVERIFY(service.closeRequests >= 1);
    }

    // And nothing called back after teardown.
    QCOMPARE(service.listener(), nullptr);
}

// ------------------------------------------------------------- refusals

void AbiContractTest::aMalformedArgumentBlockIsRefusedWithADiagnostic() {
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());

    // setMode takes one string. A three-byte block cannot hold even its
    // length prefix.
    marshal::ByteWriter truncated;
    truncated.u8(1);
    truncated.u8(2);
    truncated.u8(3);
    QCOMPARE(fixture.control(abi::kCmdSetMode, truncated), com::kInvalidArgument);

    // And a block that is longer than the command's arguments: the two sides
    // disagree about the encoding, which is the peer's bug either way.
    marshal::ByteWriter overlong;
    overlong.text(QStringLiteral("rule"));
    overlong.u32(0xDEADBEEF);
    QCOMPARE(fixture.control(abi::kCmdSetMode, overlong), com::kInvalidArgument);
}

void AbiContractTest::anUnknownCommandIsNotImplementedRatherThanIgnored() {
    fixtures::ModuleFixture fixture(fakePath_, abi::kFakeModuleId);
    QVERIFY(fixture.load());

    // A code from a newer host. kNotImplemented is an ANSWER: the caller can
    // decide the feature was optional. Silence would look like success.
    QCOMPARE(fixture.control(0x0BAD), com::kNotImplemented);
}

void AbiContractTest::theShippingModuleDoesNotImplementTheTestControlRange() {
    if (realPath_.isEmpty() || !QFileInfo::exists(realPath_)) {
        QSKIP("the shipping module artifact was not supplied to this suite");
    }

    clashqt::integration::ModuleLoader loader(realPath_);
    QVERIFY(loader.load(abi::kMihomoModuleId));
    QVERIFY(loader.handshake().moduleId == abi::kMihomoModuleId);

    com::ComPtr<abi::IBackendSession> session;
    QVERIFY(loader.createSession(session));

    // No host is installed, so no engine, controller or privileged helper is
    // constructed by this case. The test-control range is refused BEFORE the
    // backend is consulted, which is exactly why it can be asserted here.
    marshal::ByteWriter args;
    args.text(QStringLiteral("v0"));
    QCOMPARE(session->Invoke(fixtures::kFakeStageVersion, args.data().data(), args.size(), nullptr),
             com::kNotImplemented);
    QCOMPARE(session->Invoke(fixtures::kFakeEmitConnections, nullptr, 0, nullptr),
             com::kNotImplemented);

    // A command outside the range still needs a host, and says so rather than
    // crashing or answering with a default.
    QCOMPARE(session->Invoke(abi::kCmdGeneration, nullptr, 0, nullptr), com::kInvalidState);

    com::IErrorInfo *error = nullptr;
    QCOMPARE(session->LastError(&error), com::kOk);
    QVERIFY(error != nullptr);
    error->Release();

    QCOMPARE(session->Close(abi::kCloseDefault, 0), com::kOk);
    session.Reset();
    QVERIFY(loader.unload());
}

QTEST_MAIN(AbiContractTest)
#include "abi_contract_test.moc"
