#ifndef CLASHQT_CORE_BACKEND_TYPES_H
#define CLASHQT_CORE_BACKEND_TYPES_H

// Shared vocabulary of the MihomoBackend facade: request identity, generations,
// outcomes, and the value types that cross the boundary.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r3.
//
// r3 is r1's section text as amended by A1-A4 (r1 -> r2) and B1-B4 (r2 -> r3).
// Four of those amendments are load-bearing in this file and are cited where
// they apply: A1 (completion stamping), A2 and B2 (supersession is MARKED, not
// merely inferred), A3 (Endpoint is standard-layout, not POD), A4 (StopCompleted
// and TunChangeCompleted carry a Generation).
//
// r3 is the SEMANTIC contract, not the binary boundary (COMPONENT-ABI owns that
// in P4), so Qt value types are permitted here. What is NOT permitted is listed
// in the contract's section 9 and is enforced throughout:
//   * no QObject *parent in any published signature;
//   * no process-global statics - every capability query is an instance method;
//   * Endpoint is returned by value, is standard-layout and carries no
//     behaviour (section 9 as refined by A3);
//   * no reference into the component's heap is returned;
//   * no exception may unwind across the interface - every published method is
//     noexcept, and yaml-cpp's exceptions are converted to ErrorInfo at the edge;
//   * every collection handed to a consumer has a documented lifetime rule;
//   * every published enum has a fixed underlying type.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <QString>

#include "core/types.h"  // core::ProxyGroup, ProxyNode, Rule, Connection, LogEntry, BaseConfig

namespace core::backend {

// ---------------------------------------------------------------- identity

// Unique per submitted operation. Returned synchronously by every mutating
// call; every completion carries the RequestId it completes. Ids are unique
// within one backend instance and are never reused, including across
// generations. A scoped enum, so a RequestId cannot be passed where a
// Generation is wanted and vice versa.
enum class RequestId : std::uint64_t {
    // No request. Returned by a mutating call the backend rejected outright,
    // and carried by an event that completes no request of the consumer's (an
    // unsolicited failure, a stream sample, the empty collections published
    // when live state is cleared).
    Invalid = 0,
};

// Monotonic, bumped by ANY event that invalidates outstanding work: endpoint
// change, disconnect, managed start, managed stop, failure.
//
// CONSUMER OBLIGATION (contract section 2). A consumer must reject any
// completion or event whose Generation is older than the one it last observed.
// This is not an optimisation: it is what the ~20 isCurrentReply guards in
// mihomo_client.cpp do today, and dropping it lets an in-flight snapshot
// repopulate a view that a disconnect just cleared.
//
// STAMPING RULE. The stamp names the generation of the WORK the event reports
// on, which is what makes the rejection rule above operative:
//   * a completion carries the generation its request was SUBMITTED under, not
//     the one current at delivery. Stamping it at delivery would make every
//     completion look current and the rejection rule would never fire;
//   * every other event - a state change, an endpoint change, a connect or
//     disconnect, a log line, a stream sample, the empty collections published
//     when live state is cleared - carries the generation current when it was
//     produced;
//   * the terminal outcome of an operation that bumps the generation itself
//     (start, stop, a managed failure) carries the generation AFTER that bump.
//     It reports the managed core's new state and a consumer must act on it; it
//     is not work that the bump invalidated.
// The three bullets above are amendment A1, which SUPERSEDES the last paragraph
// of r1 section 2. r1 said every event and completion carries "the Generation
// current when it was produced"; for a completion that is self-defeating,
// because a completion stamped at delivery always looks current and the
// rejection rule above could never fire.
//
// ORDERING RULE (contract section 2, inherited from mihomo_client.cpp:44). The
// generation is bumped BEFORE in-flight work is aborted. finished() may run
// synchronously inside abort(), so an abort that precedes the bump delivers a
// completion stamped with the very generation it was meant to invalidate.
enum class Generation : std::uint64_t {
    Initial = 0,
};

constexpr std::uint64_t number(RequestId id) noexcept { return static_cast<std::uint64_t>(id); }
constexpr std::uint64_t number(Generation g) noexcept { return static_cast<std::uint64_t>(g); }

// True when `stamp` is older than `observed` and must therefore be rejected.
//
// Amendment A2: this test is the consumer's SECOND line of defence, not its
// only one. Completions abandoned by an abort are queued BEFORE the event that
// bumped the generation, so at the moment they are delivered `observed` is
// still the pre-bump value and this comparison cannot catch them. The backend
// marks them CompletionStatus::Superseded instead; a consumer checks both.
constexpr bool isSuperseded(Generation stamp, Generation observed) noexcept {
    return stamp < observed;
}

// ------------------------------------------------------------------ errors

// Fixed underlying type; values are permanently stable. Distinct from
// clashqt::com::Result, which P4 maps these onto.
enum class ErrorCode : std::int32_t {
    None = 0,  // no error; the only non-failure value

    Unspecified = 1,
    InvalidArgument = 2,
    InvalidState = 3,       // the backend is not in a state that permits this
    NotSupported = 4,       // the feature is absent from this backend
    NotFound = 5,           // a named entity (node, group, provider) does not exist
    Timeout = 6,            // a bounded wait expired
    Cancelled = 7,          // cancelled before completion, by an explicit request
    Superseded = 8,         // invalidated by a newer generation
    Network = 9,            // transport failure against the controller
    Protocol = 10,          // the controller answered, unusably
    Unauthorised = 11,      // the controller rejected the secret
    BinaryNotFound = 12,    // no mihomo binary
    ValidationFailed = 13,  // the candidate configuration did not validate
    LaunchFailed = 14,      // the child could not be started
    CoreExited = 15,        // the managed child exited on its own
    ReadyTimeout = 16,      // the core went quiet before answering /version
    ServiceUnavailable = 17,
    ServiceDisconnected = 18,  // lease cleanup could not be confirmed (section 6)
    ConfigUnreadable = 19,     // includes every converted yaml-cpp exception
};

constexpr bool isFailure(ErrorCode code) noexcept { return code != ErrorCode::None; }

// A diagnostic by value. `message` is already localised and is owned by the
// struct; it is copied, never borrowed.
struct ErrorInfo {
    ErrorCode code = ErrorCode::None;
    QString message;

    bool isFailure() const noexcept { return core::backend::isFailure(code); }
};

// -------------------------------------------------------------- completions

enum class CompletionStatus : std::uint8_t {
    Ok = 0,          // the operation completed and its payload is valid
    Failed = 1,      // the operation ran and failed; see error
    Cancelled = 2,   // cancelled before completion by an explicit request
    Superseded = 3,  // abandoned because the generation moved on
    Rejected = 4,    // never submitted: bad argument, wrong state, unsupported
};

// The envelope every completion event carries as its first parameter.
//
// `generation` is the generation the request was submitted under (see the
// stamping rule above). A consumer applies the section 2 rule to it verbatim:
//   if (completion.generation < lastObserved) return;   // reject
struct Completion {
    RequestId request = RequestId::Invalid;
    Generation generation = Generation::Initial;
    CompletionStatus status = CompletionStatus::Ok;
    ErrorInfo error;

    bool isOk() const noexcept { return status == CompletionStatus::Ok; }
};

// Terminal response to BackendLifecycle::stop(). Contract section 6:
//   "Stop means: no managed core that this component started is still running -
//    or an explicit unconfirmed result carrying a reason."
//
// confirmed == false is NOT a success to report as one. It means lease cleanup
// was requested but the privileged service disconnected before confirming the
// child exited; the application turns it into a shutdown warning that blocks
// quit (main.cpp:195-210) and that behaviour must survive.
struct StopCompleted {
    RequestId request = RequestId::Invalid;
    // Amendment A4 added this field: section 2 requires a generation on every
    // completion and section 6's StopCompleted omitted one.
    //
    // backend-r3 B1. stop() is an operation that bumps the generation itself,
    // so per A1 this carries the POST-bump value. It is not the generation
    // captured when stop() was submitted: the real backend emits coreFailed
    // (which bumps) before stopFinished, so a submit-time stamp is delivered as
    // coreFailed(N+1) then stopCompleted(N), and a consumer applying section 2's
    // mandatory rejection rule drops the very unconfirmed stop that blocks quit.
    Generation generation = Generation::Initial;
    // backend-r3 B2. Every completion type carries a status, because A2 requires
    // an abandoned completion to be MARKED and a bare `confirmed` flag cannot
    // express that: `false` would be indistinguishable from the unconfirmed
    // lease cleanup below, which is a different thing entirely.
    //   Ok         - the managed child's exit was observed (confirmed == true)
    //   Failed     - cleanup was requested and nothing confirmed the exit
    //   Superseded - abandoned because a newer stop replaced this one
    // `confirmed` stays, because section 6 is written in terms of it and the
    // application's shutdown warning keys on it.
    CompletionStatus status = CompletionStatus::Ok;
    bool confirmed = false;
    ErrorInfo reason;  // populated when !confirmed; empty otherwise
};

// Terminal response to BackendControl::setTunEnabled(). `actual` is a READ-BACK
// of the controller's state after the change, never an echo of `requested`.
struct TunChangeCompleted {
    RequestId request = RequestId::Invalid;
    Generation generation = Generation::Initial;
    // backend-r3 B2. A TUN change cancelled because the controller changed or
    // disconnected is a SUPERSESSION, not a protocol error: the controller never
    // answered unusably, it stopped being the controller. Reporting it as
    // ErrorCode::Protocol told a consumer the engine misbehaved when nothing of
    // the sort happened.
    CompletionStatus status = CompletionStatus::Ok;
    bool requested = false;
    bool actual = false;
    ErrorInfo error;
};

// ---------------------------------------------------------------- ownership

// Reported on every state event (contract section 1).
enum class Ownership : std::uint8_t {
    None = 0,      // nothing is running / nothing is attached
    Managed = 1,   // a child process or privileged lease THIS component started
    Attached = 2,  // a controller the user or discovery pointed us at
};

// ------------------------------------------------------------------ endpoint
//
// Deliberately a DIFFERENT type from core::Endpoint. Contract section 9
// requires the published endpoint to be returned by value and to carry no
// behaviour: core::Endpoint's baseUrl/httpBase/wsBase build a QUrl inline and
// cannot cross a module boundary. URL construction belongs to whichever side
// owns the transport; it is not part of the published surface.
//
// Strictly POD is unreachable while the struct carries QString (section 9 also
// permits Qt types in an in-process C++ interface), and amendment A3 settled
// that: r1 demanded both and the two cannot hold together. What r3 requires,
// and what is asserted below, is standard layout with no virtuals and no
// behaviour; trivial copyability is a P4 layout change under COMPONENT-ABI and
// is not claimed here. P4 replaces the QStrings
// with UTF-8 pointer+length pairs, which is a layout change inside one struct
// rather than a change to any signature.
struct Endpoint {
    QString host;
    quint16 port = 0;
    QString secret;
};

inline bool isValid(const Endpoint &endpoint) noexcept {
    return endpoint.port != 0 && !endpoint.host.isEmpty();
}

// Identity as the transport sees it. Two endpoints that differ only in secret
// are different endpoints: the secret is part of what a reply was authorised by.
inline bool isSameEndpoint(const Endpoint &a, const Endpoint &b) noexcept {
    return a.port == b.port && a.host == b.host && a.secret == b.secret;
}

// Address identity only, ignoring the secret. This is the comparison
// ui/main_window.cpp:324-328 performs by hand today to decide whether the
// attached controller is the managed child; consumers should ask
// BackendAttachment::attachmentOwnership() instead of calling this.
inline bool isSameAddress(const Endpoint &a, const Endpoint &b) noexcept {
    return a.port == b.port && a.host == b.host;
}

static_assert(std::is_standard_layout_v<Endpoint>,
              "Endpoint is data only: standard layout, no virtuals, no behaviour.");

// -------------------------------------------------------------------- spans
//
// The documented ownership rule contract section 9 demands for every collection
// handed to a consumer. Today the signals pass const references to temporaries
// with no rule at all.
//
// LIFETIME: a Span handed to an observer is BORROWED. It is valid only for the
// duration of that callback. An observer that needs the data past the callback
// copies it. The backend never frees anything on the consumer's behalf and
// never retains a pointer the consumer gave it.
template <typename T>
class Span {
  public:
    constexpr Span() noexcept = default;
    constexpr Span(const T *data, std::size_t size) noexcept : data_(data), size_(size) {}

    constexpr const T *data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool isEmpty() const noexcept { return size_ == 0; }
    constexpr const T *begin() const noexcept { return data_; }
    constexpr const T *end() const noexcept { return data_ + size_; }
    const T &operator[](std::size_t index) const noexcept { return data_[index]; }

  private:
    const T *data_ = nullptr;
    std::size_t size_ = 0;
};

// Borrows from any contiguous Qt or standard container. The span is valid for
// as long as that container is - callers keep it alive across the dispatch.
template <typename Container>
auto makeSpan(const Container &container) noexcept
    -> Span<std::remove_const_t<std::remove_reference_t<decltype(*container.data())>>> {
    using Value = std::remove_const_t<std::remove_reference_t<decltype(*container.data())>>;
    return Span<Value>(container.data(), static_cast<std::size_t>(container.size()));
}

// ---------------------------------------------------- re-exported value types
//
// Reused from core/types.h rather than redeclared, so MOD-RUNTIME and
// MOD-LIFECYCLE do not have to convert between two spellings of the same
// record. MOVE-CORE owns the eventual split of that header; these aliases are
// the only thing that has to follow it.
using BaseConfig = core::BaseConfig;
using Connection = core::Connection;
using LogEntry = core::LogEntry;
using ProxyGroup = core::ProxyGroup;
using ProxyNode = core::ProxyNode;
using Rule = core::Rule;

// Declared here rather than reused: core::Provider lives in provider_client.h,
// a QObject header a published interface must not include.
struct Provider {
    QString name;
    QString type;
    QString vehicle;
    QString behavior;
    int count = 0;
    QDateTime updated;
    quint64 used = 0;
    quint64 total = 0;
    QDateTime expires;
};

// core::ProxyGroup::selectable() as a free function, so the published record
// stays behaviour-free.
inline bool isSelectable(const ProxyGroup &group) noexcept {
    return group.type == QLatin1String("Selector") || group.type == QLatin1String("URLTest") ||
           group.type == QLatin1String("Fallback");
}

}  // namespace core::backend

#endif  // CLASHQT_CORE_BACKEND_TYPES_H
