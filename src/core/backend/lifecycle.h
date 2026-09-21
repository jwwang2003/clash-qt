#ifndef CLASHQT_CORE_BACKEND_LIFECYCLE_H
#define CLASHQT_CORE_BACKEND_LIFECYCLE_H

// BackendLifecycle: the managed core - a child process or a privileged lease
// that THIS component started.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r3, sections 1, 3, 4,
// 6, with amendments A1 and A4 (StopCompleted carries a Generation, stamped
// post-bump), B1 (which is where the real backend violated A1) and B2
// (StopCompleted carries a CompletionStatus).

#include <cstdint>

#include <QString>
#include <QStringList>

#include "core/backend/types.h"

namespace core::backend {

// Fixed underlying type with a reserved range, so an unknown value from a newer
// module is a value this enum can legally hold rather than undefined behaviour.
// A consumer switches on the known values and treats anything else as Failed's
// conservative neighbour: unknown means "do not assume the core is usable".
enum class CoreState : std::uint8_t {
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
    Failed = 4,
    // 5..63   reserved for future revisions of this contract
    // 64..255 reserved for backend-specific states; never interpreted by a consumer
};

inline constexpr std::uint8_t kCoreStateContractMax = 63;

constexpr bool isKnownCoreState(CoreState state) noexcept {
    return static_cast<std::uint8_t>(state) <= static_cast<std::uint8_t>(CoreState::Failed);
}

enum class ExecutionMode : std::uint8_t {
    Managed = 0,            // an ordinary child process owned by this component
    PrivilegedService = 1,  // a lease held through the privileged service
};

// Thread affinity
//   Every method belongs to the thread that created the backend (the owning
//   thread). Observer callbacks are delivered on that same thread, never from
//   inside one of these calls - see observer.h.
//
// Exceptions
//   None crosses this interface. Every method is noexcept and reports failure
//   through a completion event or a return value.
class BackendLifecycle {
  public:
    // ---- binary and mode (instance methods; contract section 9 forbids the
    //      process-global statics CoreProcess::discoverBinary/serviceSupported/
    //      serviceAvailable are today - a loaded module gets its own copy of
    //      every static)

    // Locates a usable engine. Empty when none is found. Returned by value.
    virtual QString discoverBinary() const noexcept = 0;
    virtual void setBinaryPath(const QString &path) noexcept = 0;
    virtual QString binaryPath() const noexcept = 0;

    // Accepted only while the managed core is Stopped or Failed and nothing is
    // in flight (core_process.cpp:207-210). Returns false, changing nothing,
    // otherwise - including when the mode is unsupported on this host.
    virtual bool setExecutionMode(ExecutionMode mode) noexcept = 0;
    virtual ExecutionMode executionMode() const noexcept = 0;
    // True only while a privileged lease is actually held, which is not the
    // same as having selected PrivilegedService as the mode.
    virtual bool usesPrivilegedService() const noexcept = 0;

    // ---- launch and stop

    // Validates `configPath` in a SEPARATE child process while the running core
    // stays live, and launches only a validated candidate (contract section 3).
    // A validation failure leaves the running configuration intact: the state
    // does not change, the endpoint does not change, and the running config
    // stays in activeConfigPaths().
    //
    // Requested while a core is running, the launch is held pending, the
    // running child is terminated, and the pending launch is consumed ONLY when
    // the retiring child's exit is observed (contract section 4). Termination
    // escalates to a kill after BackendTimings::terminateWaitMs.
    //
    // Completion: observer.coreReady() on success, observer.coreFailed() on
    // failure, both carrying the returned RequestId. Ready means an HTTP
    // GET /version answered 200 with a JSON object whose `version` field is a
    // string - NOT that the process started.
    //
    // A managed start bumps the generation, and that bump is not an endpoint
    // change, so it carries r2's re-issue obligation: the backend must re-issue
    // the snapshot set (see telemetry.h). Its own terminal outcome carries the
    // POST-bump generation (amendment A1), because the consumer has to act on
    // it rather than reject it as invalidated work.
    virtual RequestId start(const QString &configPath, const QString &workDir) noexcept = 0;

    // Terminal response: observer.stopCompleted(). Applies ONLY to a managed
    // core; an attached controller is never terminated by it (contract
    // section 1). Also clears the managed endpoint, drops any pending launch,
    // and cancels both the validation child and the readiness probe.
    //
    // Like start(), this bumps the generation and therefore owes the re-issue.
    // StopCompleted carries a Generation (A4) and a CompletionStatus (B2), and
    // the generation is the POST-bump value (A1, restated by B1 after the real
    // backend was measured emitting coreFailed(N+1) before stopCompleted(N)).
    // An unconfirmed stop must reach the consumer: it is what blocks quit, and
    // a consumer applying section 2's rejection rule to a stale stamp would
    // drop it and wedge the quit forever.
    virtual RequestId stop() noexcept = 0;

    // ---- observation

    virtual CoreState state() const noexcept = 0;

    // Managed, when this component is running or starting a core of its own;
    // None otherwise. Independent of what the client is attached to: ask
    // BackendAttachment::attachmentOwnership() about that.
    virtual Ownership ownership() const noexcept = 0;

    // The controller the managed core listens on, parsed from the config it was
    // launched with. By value; isValid() is false when no managed core is up.
    virtual Endpoint managedEndpoint() const noexcept = 0;

    // Configuration paths that must not be deleted yet: the running core's, a
    // validating candidate's, a pending launch's, and - crucially - the path of
    // every CANCELLED validation child that has not exited yet. Losing the last
    // one is a file-deletion race, not a tidiness issue (contract section 5.1).
    // Returned by value; the caller owns the copy.
    virtual QStringList activeConfigPaths() const noexcept = 0;

    // True while a pending launch, a validation, or service-mode config parsing
    // is outstanding. Consulted by the reload gate.
    virtual bool isRestartPending() const noexcept = 0;

    // The reload gate the application applies today, published as one predicate
    // so a consumer does not reach into implementation state (contract
    // section 4). Exactly:
    //   Running || Starting || (Stopping && isRestartPending())
    virtual bool isManagedCoreActive() const noexcept = 0;

  protected:
    // Non-virtual and protected: a facet is a view, not an owner. Lifetime
    // belongs to MihomoBackend.
    ~BackendLifecycle() = default;
};

}  // namespace core::backend

#endif  // CLASHQT_CORE_BACKEND_LIFECYCLE_H
