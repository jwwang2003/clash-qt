#include "support/backend/common_contract_driver.h"

#include <algorithm>
#include <array>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTimer>
#include <QtTest>

#include "core/mihomo/mihomo_backend.h"
#include "integrations/component/marshal/backend_marshal.h"
#include "integrations/component/marshal/codec.h"
#include "support/backend/fake_backend.h"
#include "support/component/fake_module_protocol.h"
#include "support/component/module_fixture.h"
#include "support/fake_core.h"
#include "support/loopback_server.h"
#include "support/scoped_environment.h"

namespace testsupport::backend::common {

namespace {

namespace fakes = ::testsupport::backend;
namespace marshal = ::clashqt::integration::marshal;
namespace proto = ::testsupport::component;
namespace abi = ::clashqt::com::abi;

// The external controllers a shared case attaches. Deliberately NOT 9090: the
// double parses every unmapped configuration path to 127.0.0.1:9090, so a
// controller on that address would be reported Managed rather than Attached and
// "stop terminates only a managed core" would stop meaning anything.
constexpr quint16 kDoubleController0 = 29101;
constexpr quint16 kDoubleController1 = 29102;

/// The HTTP request a channel names at a loopback controller.
struct ChannelRoute {
    QByteArray method;
    QString path;
};

ChannelRoute routeOf(Channel channel) {
    switch (channel) {
        case Channel::Version: return {QByteArrayLiteral("GET"), QStringLiteral("/version")};
        case Channel::Providers:
            return {QByteArrayLiteral("GET"), QStringLiteral("/providers/rules")};
        case Channel::TunChange:
            // Between the read and the read-back. Holding the initial GET
            // /configs would park the confirmation read as well, since a gate
            // stays installed after a release.
            return {QByteArrayLiteral("PATCH"), QStringLiteral("/configs")};
    }
    return {QByteArrayLiteral("GET"), QStringLiteral("/version")};
}

}  // namespace

// ------------------------------------------------------------------- rows

const char *subjectName(Subject subject) noexcept {
    switch (subject) {
        case Subject::DirectFake: return "direct-fake";
        case Subject::DirectReal: return "direct-real";
        case Subject::ModuleReal: return "module-real";
        case Subject::ModuleFake: return "module-fake";
    }
    return "unknown";
}

void addSubjectRows() {
    QTest::addColumn<int>("subject");
    for (const Subject subject : kSubjects) {
        QTest::newRow(subjectName(subject)) << static_cast<int>(subject);
    }
}

Subject currentSubject() {
    const void *data = QTest::qData("subject", ::qMetaTypeId<int>());
    return data == nullptr ? Subject::DirectFake
                           : static_cast<Subject>(*static_cast<const int *>(data));
}

// --------------------------------------------------------------- observer

ContractObserver::ContractObserver(BackendDriver *driver, QString name)
    : driver_(driver), name_(std::move(name)) {}

ContractObserver::~ContractObserver() {
    // Idempotent: removeObserver answers false for one that is not registered,
    // so a case that removed it explicitly is not a double removal.
    if (driver_ != nullptr) driver_->backend().removeObserver(this);
}

void ContractObserver::witness(cb::Generation generation) noexcept {
    ++callbackCount;
    // The section 7 obligation, enforced rather than assumed, and enforced
    // identically for all four implementations.
    if (driver_ != nullptr && driver_->isInsideMutatingCall()) sawReentrantDelivery = true;
    if (generation > lastObserved_) lastObserved_ = generation;
    if (onNextEvent) {
        const auto once = onNextEvent;
        onNextEvent = nullptr;
        once();
    }
}

void ContractObserver::record(const QString &channel, const cb::Completion &completion,
                              const QString &payload) {
    // Admission is decided BEFORE lastObserved_ moves, because the consumer
    // obligation is stated against what it had already seen.
    const bool admitted = completion.status != cb::CompletionStatus::Superseded &&
                          !cb::isSuperseded(completion.generation, lastObserved_);
    witness(completion.generation);
    records.push_back({channel, completion, payload, admitted});
}

const ContractObserver::Record *ContractObserver::find(const QString &channel,
                                                       cb::RequestId request) const {
    for (const Record &entry : records) {
        if (entry.channel == channel && entry.completion.request == request) return &entry;
    }
    return nullptr;
}

int ContractObserver::countOf(const QString &channel) const {
    return static_cast<int>(std::count_if(records.begin(), records.end(), [&](const Record &entry) {
        return entry.channel == channel;
    }));
}

int ContractObserver::countOf(const QString &channel, cb::CompletionStatus status) const {
    return static_cast<int>(std::count_if(records.begin(), records.end(), [&](const Record &entry) {
        return entry.channel == channel && entry.completion.status == status;
    }));
}

int ContractObserver::countAtOrAfter(const QString &channel, cb::Generation generation,
                                     const QString &payload) const {
    return static_cast<int>(std::count_if(records.begin(), records.end(), [&](const Record &entry) {
        return entry.channel == channel && entry.payload == payload &&
               entry.completion.status == cb::CompletionStatus::Ok &&
               !cb::isSuperseded(entry.completion.generation, generation);
    }));
}

QString ContractObserver::digest() const {
    QStringList entries;
    entries.reserve(static_cast<qsizetype>(records.size()));
    for (const Record &entry : records) {
        entries.append(QStringLiteral("%1#%2:%3@%4")
                           .arg(entry.channel)
                           .arg(cb::number(entry.completion.request))
                           .arg(static_cast<int>(entry.completion.status))
                           .arg(cb::number(entry.completion.generation)));
    }
    return QStringLiteral("[%1] states=%2 stops=%3 clears=%4 callbacks=%5")
        .arg(entries.join(QLatin1Char(' ')))
        .arg(states.size())
        .arg(stops.size())
        .arg(liveStateClears)
        .arg(callbackCount);
}

void ContractObserver::clear() {
    records.clear();
    states.clear();
    stops.clear();
    tunChanges.clear();
    coreFailures.clear();
    readyEndpoints.clear();
    endpoints.clear();
    connects.clear();
    logLines.clear();
    callbackCount = 0;
    stoppedCount = 0;
    liveStateClears = 0;
}

void ContractObserver::coreStateChanged(cb::Generation generation, cb::CoreState state,
                                        cb::Ownership ownership) noexcept {
    witness(generation);
    states.push_back({generation, state, ownership});
}
void ContractObserver::coreReady(const cb::Completion &completion,
                                 const cb::Endpoint &endpoint) noexcept {
    record(QStringLiteral("ready"), completion, endpoint.host);
    readyEndpoints.push_back(endpoint);
}
void ContractObserver::coreLogLine(cb::Generation generation, const QString &line) noexcept {
    witness(generation);
    logLines.append(line);
}
void ContractObserver::coreFailed(const cb::Completion &completion) noexcept {
    record(QStringLiteral("core-failed"), completion, completion.error.message);
    coreFailures.push_back(completion);
}
void ContractObserver::coreStopped(cb::Generation generation) noexcept {
    witness(generation);
    ++stoppedCount;
}
void ContractObserver::stopCompleted(const cb::StopCompleted &result) noexcept {
    witness(result.generation);
    stops.push_back(result);
}
void ContractObserver::endpointChanged(cb::Generation generation, const cb::Endpoint &endpoint,
                                       cb::Ownership ownership) noexcept {
    (void)ownership;
    witness(generation);
    endpoints.push_back(endpoint);
}
void ContractObserver::connectedChanged(cb::Generation generation, bool connected) noexcept {
    witness(generation);
    connects.push_back(connected);
}
void ContractObserver::configReceived(const cb::Completion &completion,
                                      const cb::BaseConfig &config) noexcept {
    record(QStringLiteral("config"), completion, config.mode);
}
void ContractObserver::modeChanged(const cb::Completion &completion, const QString &mode) noexcept {
    record(QStringLiteral("mode"), completion, mode);
}
void ContractObserver::tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept {
    witness(result.generation);
    tunChanges.push_back(result);
}
void ContractObserver::nodeSelected(const cb::Completion &completion, const QString &group,
                                    const QString &node) noexcept {
    record(QStringLiteral("node"), completion, group + QLatin1Char('/') + node);
}
void ContractObserver::versionReceived(const cb::Completion &completion,
                                       const QString &version) noexcept {
    record(QStringLiteral("version"), completion, version);
}
void ContractObserver::proxiesUpdated(const cb::Completion &completion,
                                      cb::Span<cb::ProxyGroup> groups,
                                      cb::Span<cb::ProxyNode> nodes) noexcept {
    if (completion.request == cb::RequestId::Invalid && groups.isEmpty() && nodes.isEmpty())
        ++liveStateClears;
    record(QStringLiteral("proxies"), completion, QString::number(groups.size()));
}
void ContractObserver::rulesUpdated(const cb::Completion &completion,
                                    cb::Span<cb::Rule> rules) noexcept {
    record(QStringLiteral("rules"), completion, QString::number(rules.size()));
}
void ContractObserver::providersReceived(const cb::Completion &completion, bool rules,
                                         cb::Span<cb::Provider> providers) noexcept {
    record(rules ? QStringLiteral("providers/rules") : QStringLiteral("providers/proxies"),
           completion, QString::number(providers.size()));
}
void ContractObserver::providerBusyChanged(cb::Generation generation, bool busy) noexcept {
    (void)busy;
    witness(generation);
}
void ContractObserver::privilegedServiceStatus(
    const cb::Completion &completion, const cb::PrivilegedServiceStatus &status) noexcept {
    record(QStringLiteral("service-status"), completion,
           QStringLiteral("%1/%2/%3")
               .arg(static_cast<int>(status.state))
               .arg(status.coreRunning ? 1 : 0)
               .arg(status.version));
}
void ContractObserver::errorOccurred(const cb::Completion &completion) noexcept {
    record(QStringLiteral("error"), completion, completion.error.message);
}

// ----------------------------------------------------------------- driver

BackendDriver::~BackendDriver() = default;

const QString &BackendDriver::kStagedMode() {
    static const QString mode = QStringLiteral("rule");
    return mode;
}

/// The one rule the snapshot set answers with, in both worlds: the loopback
/// fixture's /rules body and the double's staged vector describe the SAME
/// record.
static cb::Rule stagedRule() {
    cb::Rule rule;
    rule.type = QStringLiteral("MATCH");
    rule.proxy = QStringLiteral("DIRECT");
    return rule;
}

QString BackendDriver::describe() {
    cb::MihomoBackend &subject = backend();
    return QStringLiteral("%1: state=%2 ownership=%3 attachment=%4 connected=%5 "
                          "restartPending=%6 managedEndpoint=%7:%8 active=[%9] generation=%10 %11")
        .arg(name())
        .arg(static_cast<int>(subject.state()))
        .arg(static_cast<int>(subject.ownership()))
        .arg(static_cast<int>(subject.attachmentOwnership()))
        .arg(subject.isConnected() ? 1 : 0)
        .arg(subject.isRestartPending() ? 1 : 0)
        .arg(subject.managedEndpoint().host)
        .arg(subject.managedEndpoint().port)
        .arg(subject.activeConfigPaths().join(QLatin1Char(',')))
        .arg(cb::number(subject.generation()))
        .arg(environmentReport());
}

bool BackendDriver::fail(const QString &reason) {
    error_ = reason;
    return false;
}

bool BackendDriver::waitUntil(const std::function<bool()> &predicate, int timeoutMs) {
    QDeadlineTimer deadline(timeoutMs);
    QEventLoop loop;
    while (!predicate()) {
        if (deadline.hasExpired()) {
            deliver(100);
            return predicate();
        }
        loop.processEvents(QEventLoop::AllEvents, 5);
        deliver(0);
    }
    deliver(0);
    return true;
}

namespace {

// ------------------------------------------------- the in-process double

/// Shared by both drivers that drive testsupport::backend::FakeBackend - one
/// directly, one through the module boundary. The control surface is the same
/// set of actions either way, so the two drivers differ only in HOW each action
/// is delivered, which is the point of the exercise.
class FakeControls {
  public:
    virtual ~FakeControls() = default;
    virtual bool stageVersionText(const QString &version) = 0;
    virtual bool stageSnapshotData(const cb::Rule &rule, const cb::BaseConfig &config) = 0;
    virtual bool setRequestGate(bool held) = 0;
    virtual bool releaseAllRequests() = 0;
    virtual bool setValidationGate(bool held) = 0;
    virtual bool completeValidation(bool valid) = 0;
    virtual bool releaseChildExit() = 0;
    virtual bool advanceTime(qint64 ms) = 0;
    virtual bool addExternalController(const cb::Endpoint &endpoint) = 0;
    virtual bool stageTunActual(bool actual) = 0;
    virtual bool setServiceStatus(const cb::PrivilegedServiceStatus &status) = 0;
};

cb::Endpoint doubleController(int index) {
    cb::Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = index == 0 ? kDoubleController0 : kDoubleController1;
    endpoint.secret = QStringLiteral("fixture-secret");
    return endpoint;
}

/// The shared behaviour of the two double-backed drivers: every managed-core
/// and controller action expressed once, against FakeControls.
class DoubleDriver : public BackendDriver {
  public:
    DoubleDriver(Subject subject, testsupport::ScopedEnvironment &environment)
        : BackendDriver(subject), environment_(environment) {}

    bool has(Ability ability) const noexcept override {
        switch (ability) {
            case Ability::ManagedCore:
            case Ability::ValidationOutcome: return true;
            case Ability::DistinctControllerPayloads:
            case Ability::None: return false;
        }
        return false;
    }

    cb::Endpoint controller(int index) const override { return doubleController(index); }

    bool stageVersion(int index, const QString &version) override {
        (void)index;  // one staged payload per double, not one per controller
        return controls().stageVersionText(version);
    }

    bool stageSnapshot() override {
        cb::BaseConfig config;
        config.mode = kStagedMode();
        return controls().stageSnapshotData(stagedRule(), config);
    }

    bool hold(Channel channel) override {
        (void)channel;  // the gate holds every controller-bound request
        return controls().setRequestGate(true);
    }
    bool release(Channel channel) override {
        (void)channel;
        return controls().setRequestGate(false) && controls().releaseAllRequests();
    }

    bool prepareValidation(bool succeeds) override {
        validationSucceeds_ = succeeds;
        return controls().setValidationGate(true);
    }

    cb::RequestId startManagedCore(const QString &slot) override {
        // The double parses any unmapped path to 127.0.0.1:9090 and never
        // touches the filesystem; the file is written anyway so the two real
        // drivers and these two see the same shape of argument.
        const QString path = configPath(slot);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write("external-controller: 127.0.0.1:9090\n");
            file.close();
        }
        return backend().start(path, environment_.dataDir());
    }

    QString configPath(const QString &slot) const override {
        return environment_.filePath(QStringLiteral("cfg-%1.yaml").arg(slot));
    }

    bool settleValidation(int timeoutMs) override {
        if (!controls().completeValidation(validationSucceeds_)) {
            return fail(QStringLiteral("the double had no validation to settle"));
        }
        return deliver(timeoutMs);
    }

    bool driveToRunning(int timeoutMs) override {
        // Virtual time is what drives readiness probing, at the contract's own
        // probe interval. Nothing sleeps.
        const int steps = std::max(4, timeoutMs / static_cast<int>(cb::kContractTimings.probeIntervalMs));
        for (int step = 0; step < steps; ++step) {
            if (backend().state() == cb::CoreState::Running) break;
            if (!controls().advanceTime(cb::kContractTimings.probeIntervalMs)) return false;
            deliver(50);
        }
        return backend().state() == cb::CoreState::Running;
    }

    bool settleStop(int timeoutMs) override {
        // The retiring child's exit is observed. Section 4: this, and only
        // this, is what consumes a pending launch.
        controls().releaseChildExit();
        return waitUntil([this] { return backend().state() != cb::CoreState::Stopping; },
                         timeoutMs);
    }

    bool stageTunReadback(bool actual) override { return controls().stageTunActual(actual); }

    bool publishHelperCoreRunning(bool running) override {
        cb::PrivilegedServiceStatus status;
        status.state = cb::ServiceState::Connected;
        status.version = QStringLiteral("helper-1.2.3");
        status.coreRunning = running;
        return controls().setServiceStatus(status);
    }

  protected:
    virtual FakeControls &controls() = 0;
    virtual const FakeControls &controls() const = 0;
    bool registerControllers() {
        return controls().addExternalController(doubleController(0)) &&
               controls().addExternalController(doubleController(1));
    }

    testsupport::ScopedEnvironment &environment_;
    bool validationSucceeds_ = true;
};

// --------------------------------------------------- 1. the direct double

class DirectFakeControls final : public FakeControls {
  public:
    explicit DirectFakeControls(fakes::FakeBackend &fake) : fake_(fake) {}

    bool stageVersionText(const QString &version) override {
        fake_.staged().version = version;
        return true;
    }
    bool stageSnapshotData(const cb::Rule &rule, const cb::BaseConfig &config) override {
        fake_.staged().rules = QVector<cb::Rule>{rule};
        fake_.staged().config = config;
        return true;
    }
    bool setRequestGate(bool held) override {
        fake_.setRequestGate(held ? fakes::Gate::Held : fakes::Gate::Immediate);
        return true;
    }
    bool releaseAllRequests() override {
        fake_.releaseAllRequests(fakes::RequestOutcome::Success);
        return true;
    }
    bool setValidationGate(bool held) override {
        fake_.setValidationGate(held ? fakes::Gate::Held : fakes::Gate::Immediate);
        return true;
    }
    bool completeValidation(bool valid) override { return fake_.completeValidation(valid); }
    bool releaseChildExit() override { return fake_.releaseChildExit(0); }
    bool advanceTime(qint64 ms) override {
        fake_.advanceTime(ms);
        return true;
    }
    bool addExternalController(const cb::Endpoint &endpoint) override {
        fake_.addExternalController(endpoint);
        return true;
    }
    bool stageTunActual(bool actual) override {
        fake_.staged().tunActual = actual;
        return true;
    }
    bool setServiceStatus(const cb::PrivilegedServiceStatus &status) override {
        fake_.setServiceStatus(status);
        return true;
    }

  private:
    fakes::FakeBackend &fake_;
};

class DirectFakeDriver final : public DoubleDriver {
  public:
    explicit DirectFakeDriver(testsupport::ScopedEnvironment &environment)
        : DoubleDriver(Subject::DirectFake, environment) {}

    bool prepare() override {
        fake_ = std::make_unique<fakes::FakeBackend>();
        controls_ = std::make_unique<DirectFakeControls>(*fake_);
        return registerControllers();
    }
    void teardown() override {
        controls_.reset();
        fake_.reset();
    }
    cb::MihomoBackend &backend() override { return *fake_; }
    bool deliver(int timeoutMs) override { return fake_->flushEvents(std::max(timeoutMs, 1)); }
    bool isInsideMutatingCall() const override { return fake_->isInsideMutatingCall(); }

  protected:
    FakeControls &controls() override { return *controls_; }
    const FakeControls &controls() const override { return *controls_; }

  private:
    std::unique_ptr<fakes::FakeBackend> fake_;
    std::unique_ptr<DirectFakeControls> controls_;
};

// -------------------------------------------- 2. the module-backed double

/// Every control travels as a command in the reserved test range, through the
/// same codec the shipping path uses. The shipping module answers all of them
/// kNotImplemented, which is what keeps this out of the published ABI.
class ModuleFakeControls final : public FakeControls {
  public:
    explicit ModuleFakeControls(::testsupport::component::ModuleFixture &fixture)
        : fixture_(fixture) {}

    bool stageVersionText(const QString &version) override {
        marshal::ByteWriter args;
        args.text(version);
        return ok(proto::kFakeStageVersion, args);
    }
    bool stageSnapshotData(const cb::Rule &rule, const cb::BaseConfig &config) override {
        marshal::ByteWriter rules;
        rules.u32(1);  // the count guard readVector applies
        marshal::writeRule(rules, rule);
        if (!ok(proto::kFakeStageRules, rules)) return false;
        marshal::ByteWriter base;
        marshal::writeBaseConfig(base, config);
        return ok(proto::kFakeStageConfig, base);
    }
    bool setRequestGate(bool held) override {
        marshal::ByteWriter args;
        args.u8(held ? 1 : 0);
        return ok(proto::kFakeSetRequestGate, args);
    }
    bool releaseAllRequests() override {
        marshal::ByteWriter args;
        args.u8(0);  // RequestOutcome::Success
        return ok(proto::kFakeReleaseAllRequests, args);
    }
    bool setValidationGate(bool held) override {
        marshal::ByteWriter args;
        args.u8(held ? 1 : 0);
        return ok(proto::kFakeSetValidationGate, args);
    }
    bool completeValidation(bool valid) override {
        marshal::ByteWriter args;
        args.boolean(valid);
        args.text(QStringLiteral("unknown field: tun.stakc"));
        return fixture_.controlFlag(proto::kFakeCompleteValidation, args);
    }
    bool releaseChildExit() override {
        marshal::ByteWriter args;
        args.i32(0);
        return fixture_.controlFlag(proto::kFakeReleaseChildExit, args);
    }
    bool advanceTime(qint64 ms) override {
        marshal::ByteWriter args;
        args.i64(ms);
        return ok(proto::kFakeAdvanceTime, args);
    }
    bool addExternalController(const cb::Endpoint &endpoint) override {
        marshal::ByteWriter args;
        marshal::writeEndpoint(args, endpoint);
        return ok(proto::kFakeAddExternalController, args);
    }
    bool stageTunActual(bool actual) override {
        marshal::ByteWriter args;
        args.boolean(actual);
        return ok(proto::kFakeStageTunActual, args);
    }
    bool setServiceStatus(const cb::PrivilegedServiceStatus &status) override {
        marshal::ByteWriter args;
        marshal::writePrivilegedServiceStatus(args, status);
        return ok(proto::kFakeSetServiceStatus, args);
    }

  private:
    bool ok(std::uint32_t command, const marshal::ByteWriter &args) {
        return !::clashqt::com::IsFailure(fixture_.control(command, args));
    }

    ::testsupport::component::ModuleFixture &fixture_;
};

class ModuleFakeDriver final : public DoubleDriver {
  public:
    explicit ModuleFakeDriver(testsupport::ScopedEnvironment &environment)
        : DoubleDriver(Subject::ModuleFake, environment) {}

    QString unavailableReason() const override {
        const QString path = ::testsupport::component::fakeModulePath();
        if (path.isEmpty()) return QStringLiteral("CLASH_QT_FAKE_MODULE was not supplied");
        if (!QFileInfo::exists(path))
            return QStringLiteral("no test module artifact at %1").arg(path);
        return {};
    }

    bool prepare() override {
        fixture_ = std::make_unique<::testsupport::component::ModuleFixture>(
            ::testsupport::component::fakeModulePath(), abi::kFakeModuleId);
        if (!fixture_->load(nullptr)) {
            return fail(QStringLiteral("the test module did not load or did not hand out a "
                                       "usable session"));
        }
        controls_ = std::make_unique<ModuleFakeControls>(*fixture_);
        return registerControllers();
    }
    void teardown() override {
        controls_.reset();
        if (fixture_) fixture_->unload();
        fixture_.reset();
    }
    cb::MihomoBackend &backend() override { return *fixture_->contractBackend(); }
    bool deliver(int timeoutMs) override { return fixture_->backend()->drain(std::max(timeoutMs, 1)); }
    bool isInsideMutatingCall() const override {
        return fixture_->backend()->isInsideCommand();
    }

  protected:
    FakeControls &controls() override { return *controls_; }
    const FakeControls &controls() const override { return *controls_; }

  private:
    std::unique_ptr<::testsupport::component::ModuleFixture> fixture_;
    std::unique_ptr<ModuleFakeControls> controls_;
};

// ------------------------------------------------------ the real backend

/// A privileged core service with no socket at all, so the status query has a
/// producer without anything reaching the installed helper. It answers the
/// helper's OWN JSON, because what is under test is what the backend makes of
/// the helper's fields (backend-r4 C1).
class StubPrivilegedService final : public core::PrivilegedCoreService {
  public:
    void setListener(core::PrivilegedCoreServiceListener *listener) override {
        listener_ = listener;
    }
    bool isSupported() const override { return true; }
    bool isAvailable() const override { return true; }
    bool isConnected() const override { return true; }
    bool isBusy() const override { return false; }
    QString connectionError() const override { return {}; }
    void requestStatus() override {
        ++statusRequests_;
        if (listener_ == nullptr) return;
        QJsonObject status;
        status.insert(QStringLiteral("state"),
                      coreRunning_ ? QStringLiteral("running") : QStringLiteral("stopped"));
        status.insert(QStringLiteral("version"), QStringLiteral("helper-1.2.3"));
        // QUEUED, not answered from inside the query: the real helper answers
        // over a socket, and a stub that called back synchronously would
        // model something no privileged service does. It also perturbs the
        // subject - see the synchronous-answer note in registrations.md.
        QTimer::singleShot(0, &context_, [this, status] {
            if (listener_ != nullptr) listener_->privilegedStatusReceived(status);
        });
    }
    void requestLogs() override {}
    void startCore(const QJsonObject &config) override { (void)config; }
    void stopCore() override {}
    void close() override {}

    void setCoreRunning(bool running) { coreRunning_ = running; }
    int statusRequests() const { return statusRequests_; }

  private:
    QObject context_;  // the queued answer's owner; this class is not a QObject
    core::PrivilegedCoreServiceListener *listener_ = nullptr;
    bool coreRunning_ = false;
    int statusRequests_ = 0;
};

/// The loopback controllers and the compiled fixture core, shared by the two
/// drivers that exercise the real implementation. Three controllers: two
/// external ones a case may attach, and the one the managed core listens on.
class RealFixtures {
  public:
    explicit RealFixtures(testsupport::ScopedEnvironment &environment)
        : environment_(environment) {}

    bool start(QString *error) {
        for (std::size_t index = 0; index < servers_.size(); ++index) {
            servers_[index] = std::make_unique<LoopbackServer>();
            // The two external controllers check the bearer token, because a
            // case attaches them through an Endpoint that carries the secret.
            // The MANAGED controller cannot: the backend parses its endpoint
            // out of the configuration file the core is launched with, and a
            // secret the file does not mention would make every readiness
            // probe answer 401 - which is a fixture mistake, not a contract
            // failure, and it cost this driver one round of exactly that.
            const QString secret = index == kManagedIndex ? QString()
                                                          : QStringLiteral("fixture-secret");
            if (!servers_[index]->listen(secret)) {
                *error = QStringLiteral("a loopback controller refused to listen");
                return false;
            }
        }
        script(0, QStringLiteral("controller-0"));
        script(1, QStringLiteral("controller-1"));
        script(2, QStringLiteral("managed-core"));
        core_ = std::make_unique<FakeCore>(environment_.dataDir());
        if (!core_->isValid()) {
            *error = core_->errorString();
            return false;
        }
        return commitCore(true);
    }

    void script(int index, const QString &version) {
        using Reply = LoopbackServer::Reply;
        LoopbackServer &server = *servers_[static_cast<std::size_t>(index)];
        server.route("GET", "/version",
                     Reply::json(QStringLiteral(R"({"version":"%1"})").arg(version).toUtf8()));
        server.route("GET", "/proxies",
                     Reply::json(R"({"proxies":{"GLOBAL":{"type":"Selector","now":"DIRECT","all":["DIRECT"]}}})"));
        server.route("GET", "/rules",
                     Reply::json(R"({"rules":[{"type":"MATCH","payload":"","proxy":"DIRECT"}]})"));
        server.route("GET", "/configs", configsBody(tunActual_));
        server.route("PATCH", "/configs", Reply::json("{}"));
        server.route("GET", "/providers/rules", Reply::json(R"({"providers":{}})"));
        server.route("GET", "/providers/proxies", Reply::json(R"({"providers":{}})"));
    }

    static LoopbackServer::Reply configsBody(bool tunEnabled) {
        return LoopbackServer::Reply::json(
            QStringLiteral(R"({"mode":"rule","tun":{"enable":%1}})")
                .arg(tunEnabled ? QStringLiteral("true") : QStringLiteral("false"))
                .toUtf8());
    }

    void setTunReadback(bool actual) {
        tunActual_ = actual;
        for (auto &server : servers_) server->route("GET", "/configs", configsBody(actual));
    }

    bool commitCore(bool validationSucceeds) {
        if (validationSucceeds) {
            core_->validationSucceeds();
        } else {
            core_->validationFails(QStringLiteral("unknown field: tun.stakc"));
        }
        // The run directives are APPENDED by FakeCore, so they are added once
        // and only the validation outcome is rewritten afterwards. A script
        // that grew a second run-forever on every commit would still work and
        // would still be wrong.
        if (!runDirectives_) {
            core_->printsLine(QStringLiteral("[INFO] up")).runsForever();
            runDirectives_ = true;
        }
        return core_->commit();
    }

    QString writeConfig(const QString &path, quint16 port) const {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        file.write("external-controller: 127.0.0.1:" + QByteArray::number(port) + "\n");
        file.close();
        return path;
    }

    LoopbackServer &server(int index) { return *servers_[static_cast<std::size_t>(index)]; }
    const LoopbackServer &server(int index) const {
        return *servers_[static_cast<std::size_t>(index)];
    }
    FakeCore &core() { return *core_; }
    StubPrivilegedService &service() { return service_; }

    cb::Endpoint endpoint(int index) const {
        cb::Endpoint endpoint;
        endpoint.host = server(index).host();
        endpoint.port = server(index).port();
        endpoint.secret = server(index).secret();
        return endpoint;
    }

    /// Installs the gate for `channel` at controller 0 once, and returns it.
    LoopbackGate *gate(Channel channel) {
        LoopbackGate *&held = gates_[static_cast<std::size_t>(channel)];
        if (held == nullptr) {
            const ChannelRoute route = routeOf(channel);
            held = server(0).hold(route.method, route.path);
        }
        return held;
    }

  public:
    /// The controller the managed core listens on, as opposed to the two
    /// external ones a case may attach.
    static constexpr std::size_t kManagedIndex = 2;

  private:
    testsupport::ScopedEnvironment &environment_;
    std::array<std::unique_ptr<LoopbackServer>, 3> servers_;
    std::unique_ptr<FakeCore> core_;
    StubPrivilegedService service_;
    std::array<LoopbackGate *, 3> gates_{nullptr, nullptr, nullptr};
    bool tunActual_ = false;
    bool runDirectives_ = false;
};

/// What the two real drivers share: every action expressed against
/// RealFixtures and the published facade, so the direct and module-backed rows
/// differ only in which object the calls go through.
class RealDriver : public BackendDriver {
  public:
    RealDriver(Subject subject, testsupport::ScopedEnvironment &environment)
        : BackendDriver(subject), fixtures_(environment), environment_(environment) {}

    bool has(Ability ability) const noexcept override {
        switch (ability) {
            case Ability::ManagedCore:
            case Ability::ValidationOutcome:
            case Ability::DistinctControllerPayloads: return true;
            case Ability::None: return false;
        }
        return false;
    }

    cb::Endpoint controller(int index) const override { return fixtures_.endpoint(index); }

    bool stageVersion(int index, const QString &version) override {
        fixtures_.server(index).route(
            "GET", QStringLiteral("/version"),
            LoopbackServer::Reply::json(
                QStringLiteral(R"({"version":"%1"})").arg(version).toUtf8()));
        return true;
    }

    /// The loopback fixture already answers one MATCH rule and mode `rule`;
    /// RealFixtures::script is where that body lives.
    bool stageSnapshot() override { return true; }

    bool hold(Channel channel) override { return fixtures_.gate(channel) != nullptr; }
    bool release(Channel channel) override {
        LoopbackGate *gate = fixtures_.gate(channel);
        if (gate == nullptr) return false;
        // The request travels a real socket, so it may not have ARRIVED when a
        // case releases the gate - and release() only lets through what is
        // parked at that instant. Waiting for it here is what makes "park,
        // observe, release" deterministic rather than a race the case would
        // lose about one time in however many.
        const bool parked = gate->waitForPending(1);
        gate->release();
        return parked || gate->released() > 0;
    }

    bool prepareValidation(bool succeeds) override { return fixtures_.commitCore(succeeds); }

    cb::RequestId startManagedCore(const QString &slot) override {
        const QString path = fixtures_.writeConfig(
            configPath(slot), fixtures_.server(RealFixtures::kManagedIndex).port());
        if (path.isEmpty()) {
            fail(QStringLiteral("the candidate configuration could not be written"));
            return cb::RequestId::Invalid;
        }
        backend().setBinaryPath(fixtures_.core().binaryPath());
        return backend().start(path, environment_.dataDir());
    }

    QString configPath(const QString &slot) const override {
        return environment_.filePath(QStringLiteral("cfg-%1.yaml").arg(slot));
    }

    bool settleValidation(int timeoutMs) override {
        // The validation child is a real process; it is settled when the
        // backend no longer has one outstanding.
        return waitUntil([this] { return !backend().isRestartPending(); }, timeoutMs);
    }

    bool driveToRunning(int timeoutMs) override {
        // Production readiness budget: the probe interval is 200 ms and the
        // loopback controller answers at once, so this is a bounded wait on a
        // real socket rather than a sleep.
        return waitUntil([this] { return backend().state() == cb::CoreState::Running; }, timeoutMs);
    }

    bool settleStop(int timeoutMs) override {
        return waitUntil(
            [this] {
                return backend().state() == cb::CoreState::Stopped ||
                       backend().state() == cb::CoreState::Failed;
            },
            timeoutMs);
    }

    bool stageTunReadback(bool actual) override {
        fixtures_.setTunReadback(actual);
        return true;
    }

    bool publishHelperCoreRunning(bool running) override {
        fixtures_.service().setCoreRunning(running);
        return true;
    }

    QString environmentReport() const override {
        RealFixtures &fixtures = const_cast<RealFixtures &>(fixtures_);
        return QStringLiteral("core=[%1] managedController=%2 %3")
            .arg(fixtures.core().invocations().join(QLatin1Char('|')))
            .arg(fixtures.server(RealFixtures::kManagedIndex).port())
            .arg(fixtures.server(RealFixtures::kManagedIndex).pendingReport());
    }

  protected:
    RealFixtures fixtures_;
    testsupport::ScopedEnvironment &environment_;
};

// ------------------------------------------------- 3. the direct real one

class DirectRealDriver final : public RealDriver {
  public:
    explicit DirectRealDriver(testsupport::ScopedEnvironment &environment)
        : RealDriver(Subject::DirectReal, environment) {}

    bool prepare() override {
        QString error;
        if (!fixtures_.start(&error)) return fail(error);
        backend_ = std::make_unique<core::MihomoBackendImpl>(&fixtures_.service());
        return true;
    }
    void teardown() override {
        if (backend_) {
            backend_->stop();
            waitUntil(
                [this] {
                    return backend_->state() == cb::CoreState::Stopped ||
                           backend_->state() == cb::CoreState::Failed;
                },
                5000);
        }
        backend_.reset();
    }
    cb::MihomoBackend &backend() override { return *backend_; }
    bool deliver(int timeoutMs) override {
        return backend_->drainPendingEvents(std::max(timeoutMs, 1));
    }
    bool isInsideMutatingCall() const override { return backend_->isInsideMutatingCall(); }

  private:
    std::unique_ptr<core::MihomoBackendImpl> backend_;
};

// ------------------------------------------ 4. the module-backed real one

class ModuleRealDriver final : public RealDriver {
  public:
    explicit ModuleRealDriver(testsupport::ScopedEnvironment &environment)
        : RealDriver(Subject::ModuleReal, environment) {}

    QString unavailableReason() const override {
        const QString path = ::testsupport::component::realModulePath();
        if (path.isEmpty()) return QStringLiteral("CLASH_QT_BACKEND_MODULE was not supplied");
        if (!QFileInfo::exists(path))
            return QStringLiteral("no shipping module artifact at %1").arg(path);
        return {};
    }

    bool prepare() override {
        QString error;
        if (!fixtures_.start(&error)) return fail(error);
        fixture_ = std::make_unique<::testsupport::component::ModuleFixture>(
            ::testsupport::component::realModulePath(), abi::kMihomoModuleId);
        // The host owns privileged execution (module-r1): the module is handed
        // the seam, it never constructs a helper client of its own.
        if (!fixture_->load(&fixtures_.service())) {
            return fail(QStringLiteral("the shipping module did not load or did not hand out a "
                                       "usable session"));
        }
        return true;
    }
    void teardown() override {
        if (fixture_ && fixture_->backend() != nullptr) {
            // The ABI teardown order: cancel and stop what the session owns,
            // release the module's objects, and only then unmap.
            fixture_->backend()->shutdown(/*stopManagedCore=*/true, 5000);
        }
        if (fixture_) fixture_->unload();
        fixture_.reset();
    }
    cb::MihomoBackend &backend() override { return *fixture_->contractBackend(); }
    bool deliver(int timeoutMs) override {
        return fixture_->backend()->drain(std::max(timeoutMs, 1));
    }
    bool isInsideMutatingCall() const override { return fixture_->backend()->isInsideCommand(); }

  private:
    std::unique_ptr<::testsupport::component::ModuleFixture> fixture_;
};

}  // namespace

std::unique_ptr<BackendDriver> makeDriver(Subject subject,
                                          testsupport::ScopedEnvironment &environment) {
    switch (subject) {
        case Subject::DirectFake: return std::make_unique<DirectFakeDriver>(environment);
        case Subject::DirectReal: return std::make_unique<DirectRealDriver>(environment);
        case Subject::ModuleReal: return std::make_unique<ModuleRealDriver>(environment);
        case Subject::ModuleFake: return std::make_unique<ModuleFakeDriver>(environment);
    }
    return nullptr;
}

}  // namespace testsupport::backend::common
