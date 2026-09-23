#pragma once

// The test module's control surface, in the reserved range.
//
// WHY IT IS A SEPARATE RANGE AND NOT A FLAG ON THE SHIPPING ABI
//   module-r1: "Do not add failure-inversion switches to shipping ABI." Every
//   code here is at or above abi::kCmdTestControlBase, and the shipping module
//   installs no command extension, so it answers all of them kNotImplemented.
//   The ABI suite asserts exactly that against the REAL module, which is what
//   makes this arrangement a claim rather than an intention.
//
// The payloads reuse the same private marshalling the shipping path uses, so
// the double exercises the codec rather than going around it.

#include <cstdint>

#include "core/component/abi/wire.h"

namespace testsupport::component {

namespace abi = ::clashqt::com::abi;

enum FakeCommand : std::uint32_t {
    // ---- staged payloads, so a released request carries real data
    kFakeStageVersion = abi::kCmdTestControlBase + 0x01,   // text
    kFakeStageMode = abi::kCmdTestControlBase + 0x02,      // text
    kFakeStageProxies = abi::kCmdTestControlBase + 0x03,   // groups, nodes
    kFakeStageRules = abi::kCmdTestControlBase + 0x04,     // rules
    kFakeStageProviders = abi::kCmdTestControlBase + 0x05, // providers
    kFakeStageConfig = abi::kCmdTestControlBase + 0x06,    // BaseConfig
    kFakeStageTunActual = abi::kCmdTestControlBase + 0x07, // bool
    kFakeStageDnsResult = abi::kCmdTestControlBase + 0x08, // text

    // ---- the request gate
    kFakeSetRequestGate = abi::kCmdTestControlBase + 0x10,     // u8 (0 immediate, 1 held)
    kFakeReleaseRequest = abi::kCmdTestControlBase + 0x11,     // u64 id, u8 outcome, ErrorInfo
    kFakeReleaseAllRequests = abi::kCmdTestControlBase + 0x12, // u8 outcome
    kFakeIsPending = abi::kCmdTestControlBase + 0x13,          // u64 id -> bool

    // ---- controller and managed core
    kFakeAddExternalController = abi::kCmdTestControlBase + 0x20,  // Endpoint
    kFakeDisconnectController = abi::kCmdTestControlBase + 0x21,   // -
    kFakeEmitCoreLogLine = abi::kCmdTestControlBase + 0x22,        // text
    kFakeSetValidationGate = abi::kCmdTestControlBase + 0x23,      // u8
    kFakeCompleteValidation = abi::kCmdTestControlBase + 0x24,     // bool, text -> bool
    kFakeSetChildExitGate = abi::kCmdTestControlBase + 0x25,       // u8
    kFakeReleaseChildExit = abi::kCmdTestControlBase + 0x26,       // i32 -> bool
    kFakeAdvanceTime = abi::kCmdTestControlBase + 0x27,            // i64 ms
    kFakeSetDiscoverableBinary = abi::kCmdTestControlBase + 0x28,  // text

    // ---- privileged service
    kFakeSetServiceStatus = abi::kCmdTestControlBase + 0x30,     // PrivilegedServiceStatus
    kFakeSetServiceSupported = abi::kCmdTestControlBase + 0x31,  // bool
    kFakeSetServiceAvailable = abi::kCmdTestControlBase + 0x32,  // bool
    // Asks the module's adapter, not the fake: proves the reverse seam reaches
    // the HOST's privileged service and comes back with its answer.
    kFakeQueryHostPrivileged = abi::kCmdTestControlBase + 0x33,  // u32 host command -> bool
    kFakeHostPrivilegedStartCore = abi::kCmdTestControlBase + 0x34,  // text json

    // ---- telemetry the deterministic fake does not produce on its own.
    // Injected through the session's own observer face, so it travels the
    // identical encode/decode path a real sample would.
    kFakeEmitConnections = abi::kCmdTestControlBase + 0x40,    // Generation, connections, u64, u64
    kFakeEmitLogEntry = abi::kCmdTestControlBase + 0x41,       // Generation, LogEntry
    kFakeEmitTrafficSample = abi::kCmdTestControlBase + 0x42,  // Generation, u64, u64
    kFakeEmitMemorySample = abi::kCmdTestControlBase + 0x43,   // Generation, u64, u64
};

}  // namespace testsupport::component
