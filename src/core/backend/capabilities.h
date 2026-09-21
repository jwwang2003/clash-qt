#ifndef CLASHQT_CORE_BACKEND_CAPABILITIES_H
#define CLASHQT_CORE_BACKEND_CAPABILITIES_H

// BackendCapabilities: what this backend can do, queried rather than assumed.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r3, sections 3, 4, 8, 9.
//
// Capabilities are INSTANCE methods. CoreProcess::serviceSupported() and
// serviceAvailable() are static today, and a loaded module gets its own copy of
// every static (contract section 9). Publishing them here is also what lets
// ui/service_settings.cpp stop opening a second PrivilegedServiceClient
// alongside the one the backend owns - two live connections to one privileged
// socket is a correctness hazard today (decision D3).

#include <cstdint>

#include <QString>

#include "core/backend/types.h"

namespace core::backend {

enum class Feature : std::uint32_t {
    None = 0,
    ManagedLifecycle = 1u << 0,   // can start and stop a core of its own
    PrivilegedService = 1u << 1,  // can hold a lease through the privileged service
    ConfirmedTunChange = 1u << 2,  // setTunEnabled reads the result back
    DnsQuery = 1u << 3,
    DnsCacheFlush = 1u << 4,
    GeoDatabaseUpdate = 1u << 5,
    MemoryStream = 1u << 6,
    Providers = 1u << 7,
    ConfigValidation = 1u << 8,  // validates a candidate in a separate process
};

struct FeatureSet {
    std::uint32_t bits = 0;
    constexpr bool has(Feature feature) const noexcept {
        return (bits & static_cast<std::uint32_t>(feature)) != 0;
    }
};

constexpr FeatureSet operator|(FeatureSet set, Feature feature) noexcept {
    return FeatureSet{set.bits | static_cast<std::uint32_t>(feature)};
}
constexpr FeatureSet operator|(Feature a, Feature b) noexcept {
    return FeatureSet{static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b)};
}

// Required by the COMPONENT-ABI handshake in P4. `interfaceRevision` is 1 for
// this interface and is distinct from the module ABI version.
//
// It is NOT the contract revision number. r1, r2 and r3 are one interface: the
// A1-A4 and B1-B4 amendments corrected and tightened its semantics without
// adding or reordering a published method, so the value stayed 1 across all
// three. Both the real backend and the fake report 1 today, and both contract
// suites assert it. It moves only when the vtable does - which, per section 9,
// is a new interface id rather than a mutated one.
struct BackendIdentity {
    QString name;
    std::uint32_t moduleAbiVersion = 0;
    std::uint32_t interfaceRevision = 0;
};

// The readiness and termination parameters ARE part of the contract
// (section 3): a consumer may not assume a fixed timeout, because a core that
// keeps logging keeps its deadline alive. Published as data so a consumer can
// size its own progress reporting against the backend's real budget.
struct BackendTimings {
    // Silence-based, refreshed by EVERY log line the core emits.
    std::uint32_t idleDeadlineMs = 10000;
    std::uint32_t serviceIdleDeadlineMs = 60000;
    // Absolute. NOT refreshable by log output.
    std::uint32_t hardCapMs = 180000;
    std::uint32_t probeIntervalMs = 200;
    std::uint32_t probeTimeoutMs = 2000;
    // Termination escalates to a kill after this long (contract section 4).
    std::uint32_t terminateWaitMs = 3000;
};

inline constexpr BackendTimings kContractTimings{};

enum class ServiceState : std::uint8_t {
    Unsupported = 0,  // the platform has no privileged service
    NotInstalled = 1,
    Installed = 2,  // installed but not connected
    Connected = 3,
    Failed = 4,
};

struct PrivilegedServiceStatus {
    ServiceState state = ServiceState::Unsupported;
    QString version;
    ErrorInfo error;
};

// Thread affinity: the owning thread, as BackendLifecycle.
class BackendCapabilities {
  public:
    virtual BackendIdentity identity() const noexcept = 0;
    virtual FeatureSet features() const noexcept = 0;
    bool hasFeature(Feature feature) const noexcept { return features().has(feature); }

    // Instance methods, deliberately: see the note at the top of this file.
    virtual bool serviceSupported() const noexcept = 0;
    virtual bool serviceAvailable() const noexcept = 0;

    virtual BackendTimings timings() const noexcept = 0;

    // Asynchronous, because answering it means talking to the service.
    // Completion: observer.privilegedServiceStatus().
    virtual RequestId requestPrivilegedServiceStatus() noexcept = 0;

  protected:
    ~BackendCapabilities() = default;
};

}  // namespace core::backend

#endif  // CLASHQT_CORE_BACKEND_CAPABILITIES_H
