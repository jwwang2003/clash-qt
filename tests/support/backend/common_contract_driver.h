#ifndef CLASHQT_TESTS_SUPPORT_BACKEND_COMMON_CONTRACT_DRIVER_H
#define CLASHQT_TESTS_SUPPORT_BACKEND_COMMON_CONTRACT_DRIVER_H

// Fixture adapters for the SHARED backend contract suite.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r4 section 10,
// .refactor/P4_ABI_CONTRACT.md revision module-r1, decision D8.
//
// WHY THIS EXISTS
//   backend-r4 section 10 and module-r1 both require ONE set of assertions to
//   hold against every implementation of the facade. Two suites asserting
//   similar things are not that: r3 B3 records a divergence that survived
//   precisely because the fake and the real backend were held to two files
//   rather than to one statement. So the scenarios in
//   tests/contracts/backend/common/ take a BackendDriver & and never name an
//   implementation, and this header supplies four drivers:
//
//     DirectFake   testsupport::backend::FakeBackend, in process
//     DirectReal   core::MihomoBackendImpl, in process, real child processes
//                  and a real loopback controller
//     ModuleReal   ModuleBackend over the SHIPPING module artifact, loaded
//                  through the production loader
//     ModuleFake   ModuleBackend over the test module artifact
//
//   The two module drivers really do load a shared library and talk to it over
//   the ABI; neither wraps an in-process implementation, and neither falls back
//   to one. A missing artifact or a module that will not load is a FAILURE in
//   this lane, named by unavailableReason(): the module rows are required
//   coverage, and substituting a mock for them would assert the ABI against
//   itself.
//
// WHAT A DRIVER MAY DO, AND WHAT IT MAY NOT
//   A driver performs CONTROLLED ENVIRONMENT ACTIONS: it scripts a loopback
//   controller or the double's staged payloads, parks a request, releases a
//   validation child, advances the double's clock, answers a privileged status
//   query from a host-side stub. It never injects a failure through the
//   shipping ABI - module-r1 forbids a failure switch there, and the shipping
//   module answers every test-control command kNotImplemented, which the ABI
//   suite asserts. Nothing here touches the installed privileged helper, the
//   system proxy, a TUN device or a real subscription: the engine is the
//   compiled fixture core and the controller is a loopback fixture.
//
// WHAT IS DELIBERATELY NOT SHARED
//   The specialist cases stay where they are. A virtual clock out to four
//   times the 180 s hard cap, setAbortOrdering(AbortThenBump) and the
//   StopStamping knob exist only on the in-process double; the readiness
//   deadline cases need CoreTimings, which is host-side by design and is not
//   part of the facade a module consumer gets. A driver that cannot express a
//   step says so through Ability rather than by quietly weakening a shared
//   assertion.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <QString>
#include <QStringList>

#include "core/backend/backend.h"

namespace testsupport {
class ScopedEnvironment;
}

namespace testsupport::backend::common {

namespace cb = ::core::backend;

// ---------------------------------------------------------------- the rows

enum class Subject : std::uint8_t {
    DirectFake = 0,
    DirectReal = 1,
    ModuleReal = 2,
    ModuleFake = 3,
};

inline constexpr Subject kSubjects[] = {Subject::DirectFake, Subject::DirectReal,
                                        Subject::ModuleReal, Subject::ModuleFake};

/// The row name a QTest data row carries, e.g. "module-real".
const char *subjectName(Subject subject) noexcept;

/// Adds the "subject" column and one row per implementation. Every shared case
/// uses this one builder, so no case can quietly run against three of four.
void addSubjectRows();

/// The subject of the row being executed. Valid only inside a test function.
Subject currentSubject();

// ------------------------------------------------------------- capabilities

/// What an ADAPTER can arrange, as distinct from what the contract requires.
/// A shared assertion is never gated on an Ability; only an additional,
/// strictly stronger arm is.
enum class Ability : std::uint32_t {
    None = 0,
    /// Two controllers answer distinguishable payloads, so a completion can be
    /// attributed to the controller that produced it. The in-process double
    /// stages one payload per backend, not one per controller.
    DistinctControllerPayloads = 1u << 0,
    /// A managed core can be driven to Running and back deterministically.
    ManagedCore = 1u << 1,
    /// A candidate configuration's validation outcome can be chosen.
    ValidationOutcome = 1u << 2,
};

// ------------------------------------------------------------- the observer

/// Records what reached an observer, and - the point of the exercise - whether
/// anything reached it from inside a mutating call of the subject. Driver-
/// agnostic, so the SAME witness applies to all four implementations:
/// testsupport::backend::RecordingObserver cannot, because its re-entrancy
/// witness takes a FakeBackend *.
class BackendDriver;

class ContractObserver final : public cb::BackendObserver {
  public:
    explicit ContractObserver(BackendDriver *driver = nullptr,
                              QString name = QStringLiteral("observer"));
    /// Removes itself from the subject. The contract says an observer must be
    /// removed before it is destroyed, and a case that fails an assertion
    /// returns EARLY - so the removal cannot be the last statement of a case
    /// without every failure turning into a dangling observer and a crash in
    /// teardown.
    ~ContractObserver() override;

    struct Record {
        QString channel;
        cb::Completion completion;
        QString payload;
        /// False when the section 2 rule ("reject a stamp older than the
        /// newest observed") or the A2 marking would have turned it away.
        bool admitted = true;
    };
    struct StateEvent {
        cb::Generation generation = cb::Generation::Initial;
        cb::CoreState state = cb::CoreState::Stopped;
        cb::Ownership ownership = cb::Ownership::None;
    };

    std::vector<Record> records;
    std::vector<StateEvent> states;
    std::vector<cb::StopCompleted> stops;
    std::vector<cb::TunChangeCompleted> tunChanges;
    std::vector<cb::Completion> coreFailures;
    std::vector<cb::Endpoint> readyEndpoints;
    std::vector<cb::Endpoint> endpoints;
    std::vector<bool> connects;
    QStringList logLines;
    int callbackCount = 0;
    int stoppedCount = 0;
    int liveStateClears = 0;
    bool sawReentrantDelivery = false;
    /// Runs inside the next callback, once.
    std::function<void()> onNextEvent;

    QString name() const { return name_; }
    cb::Generation lastObserved() const noexcept { return lastObserved_; }
    const Record *find(const QString &channel, cb::RequestId request) const;
    int countOf(const QString &channel) const;
    int countOf(const QString &channel, cb::CompletionStatus status) const;
    /// Successful completions of `channel` stamped at or after `generation`
    /// that carry `payload`. The payload is what separates re-issued LIVE data
    /// from the empty view an invalidation publishes - counting completions
    /// alone cannot, and requiring a non-Invalid RequestId cannot either,
    /// because a backend-initiated re-issue completes no request of the
    /// consumer's and may legitimately carry RequestId::Invalid.
    int countAtOrAfter(const QString &channel, cb::Generation generation,
                       const QString &payload) const;
    /// Every recorded completion as `channel#request:status@generation`, for a
    /// failure message that says what DID arrive instead.
    QString digest() const;
    void clear();

    // --- BackendObserver
    void coreStateChanged(cb::Generation generation, cb::CoreState state,
                          cb::Ownership ownership) noexcept override;
    void coreReady(const cb::Completion &completion, const cb::Endpoint &endpoint) noexcept override;
    void coreLogLine(cb::Generation generation, const QString &line) noexcept override;
    void coreFailed(const cb::Completion &completion) noexcept override;
    void coreStopped(cb::Generation generation) noexcept override;
    void stopCompleted(const cb::StopCompleted &result) noexcept override;
    void endpointChanged(cb::Generation generation, const cb::Endpoint &endpoint,
                         cb::Ownership ownership) noexcept override;
    void connectedChanged(cb::Generation generation, bool connected) noexcept override;
    void configReceived(const cb::Completion &completion,
                        const cb::BaseConfig &config) noexcept override;
    void modeChanged(const cb::Completion &completion, const QString &mode) noexcept override;
    void tunChangeCompleted(const cb::TunChangeCompleted &result) noexcept override;
    void nodeSelected(const cb::Completion &completion, const QString &group,
                      const QString &node) noexcept override;
    void versionReceived(const cb::Completion &completion,
                         const QString &version) noexcept override;
    void proxiesUpdated(const cb::Completion &completion, cb::Span<cb::ProxyGroup> groups,
                        cb::Span<cb::ProxyNode> nodes) noexcept override;
    void rulesUpdated(const cb::Completion &completion, cb::Span<cb::Rule> rules) noexcept override;
    void providersReceived(const cb::Completion &completion, bool rules,
                           cb::Span<cb::Provider> providers) noexcept override;
    void providerBusyChanged(cb::Generation generation, bool busy) noexcept override;
    void privilegedServiceStatus(const cb::Completion &completion,
                                 const cb::PrivilegedServiceStatus &status) noexcept override;
    void errorOccurred(const cb::Completion &completion) noexcept override;

  private:
    void witness(cb::Generation generation) noexcept;
    void record(const QString &channel, const cb::Completion &completion, const QString &payload);

    BackendDriver *driver_ = nullptr;
    QString name_;
    cb::Generation lastObserved_ = cb::Generation::Initial;
};

// -------------------------------------------------------------- the adapter

/// The channels a shared case can park, so an operation can be observed
/// OUTSTANDING and then invalidated or released. A real driver parks the
/// matching route at controller 0; a double holds its request gate, which
/// parks every controller-bound request.
///
/// TunChange parks the PATCH the real client issues between reading the
/// controller's TUN state and reading it back, which is the only point where
/// the change is outstanding on both a socket and a deterministic double.
enum class Channel : std::uint8_t { Version, Providers, TunChange };

class BackendDriver {
  public:
    virtual ~BackendDriver();

    BackendDriver(const BackendDriver &) = delete;
    BackendDriver &operator=(const BackendDriver &) = delete;

    // ------------------------------------------------------------ identity
    Subject subject() const noexcept { return subject_; }
    QString name() const { return QString::fromLatin1(subjectName(subject_)); }
    /// Non-empty when this implementation's artifact was not supplied to the
    /// suite, and it names which one. In THIS integration lane a module
    /// artifact is REQUIRED, so the host turns a non-empty reason into a
    /// FAILURE rather than a skip: a required row that quietly disappears while
    /// CTest prints Passed is the outcome the shared suite exists to prevent.
    /// A host that genuinely has no module lane may still read it and say so.
    virtual QString unavailableReason() const { return {}; }
    QString errorString() const { return error_; }

    /// Builds the subject. False means the row fails, not that it skips.
    virtual bool prepare() = 0;
    /// Releases the subject before its environment goes. For a module driver
    /// this is the ABI teardown order: close the session, release the objects,
    /// only then unmap.
    virtual void teardown() = 0;

    /// The ONLY view a shared case gets of the implementation.
    virtual cb::MihomoBackend &backend() = 0;

    virtual bool has(Ability ability) const noexcept = 0;

    /// The facade's own state, for a failure message that says what the
    /// subject was doing instead of what was expected.
    QString describe();
    /// What the environment was doing: the fixture core's invocations, the
    /// controller's parked requests. Empty for a driver with no environment.
    virtual QString environmentReport() const { return {}; }

    // ------------------------------------------------------------ delivery
    /// Delivers everything already produced, pumping the owning thread.
    virtual bool deliver(int timeoutMs = 5000) = 0;
    /// True while a mutating call of the subject is on the stack. An observer
    /// invoked while this holds was delivered re-entrantly, which section 7
    /// forbids.
    virtual bool isInsideMutatingCall() const = 0;
    /// Spins the owning thread until `predicate` holds. The deadline only
    /// bounds a failure; it never decides an outcome.
    bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 15000);

    // --------------------------------------------------------- controllers
    /// A controller that is RUNNING and that nothing has attached. Index 0 and
    /// 1 are different controllers; neither is the managed core's.
    virtual cb::Endpoint controller(int index) const = 0;
    /// What controller `index` answers GET /version with.
    virtual bool stageVersion(int index, const QString &version) = 0;
    /// Makes the controller answer the snapshot set with LIVE data: exactly
    /// kStagedRuleCount rules and kStagedMode as the mode. A loopback fixture
    /// already answers that; a double has to be told. It is what separates a
    /// re-issued snapshot from an emptied view.
    virtual bool stageSnapshot() = 0;
    static constexpr int kStagedRuleCount = 1;
    static const QString &kStagedMode();

    /// Parks `channel` so a request can be observed outstanding and then
    /// invalidated. release() lets every parked request through.
    virtual bool hold(Channel channel) = 0;
    virtual bool release(Channel channel) = 0;

    // -------------------------------------------------------- managed core
    /// Chooses the outcome of the NEXT candidate validation.
    virtual bool prepareValidation(bool succeeds) = 0;
    /// Starts a managed core from the named configuration slot. The driver owns
    /// the config file and the controller the core listens on; a shared case
    /// asserts on activeConfigPaths() through configPath().
    virtual cb::RequestId startManagedCore(const QString &slot) = 0;
    virtual QString configPath(const QString &slot) const = 0;
    /// Settles an outstanding validation with the outcome prepareValidation()
    /// chose, and waits for the backend to have acted on it.
    virtual bool settleValidation(int timeoutMs = 15000) = 0;
    /// Drives a started core to Running: readiness is 200 + a string
    /// `version`, answered by a loopback fixture or by the double.
    virtual bool driveToRunning(int timeoutMs = 20000) = 0;
    /// Lets a terminating child's exit be observed. Section 4: this is what
    /// consumes a pending launch, and nothing else may.
    virtual bool settleStop(int timeoutMs = 20000) = 0;

    // ------------------------------------------------------------- control
    /// What the controller will REPORT for TUN after a change is accepted -
    /// the read-back, staged apart from the request. Never a real TUN device.
    virtual bool stageTunReadback(bool actual) = 0;

    // -------------------------------------------- privileged service status
    /// What the helper will answer the next status query with. For the real
    /// implementations this is a host-side stub answering the helper's own
    /// JSON; nothing connects to an installed helper.
    virtual bool publishHelperCoreRunning(bool running) = 0;

  protected:
    explicit BackendDriver(Subject subject) : subject_(subject) {}
    bool fail(const QString &reason);

    Subject subject_;
    mutable QString error_;
};

/// One driver per subject. `environment` owns the temporary data directory and
/// the environment restoration; the driver borrows it and must not outlive it.
std::unique_ptr<BackendDriver> makeDriver(Subject subject,
                                          testsupport::ScopedEnvironment &environment);

}  // namespace testsupport::backend::common

#endif  // CLASHQT_TESTS_SUPPORT_BACKEND_COMMON_CONTRACT_DRIVER_H
