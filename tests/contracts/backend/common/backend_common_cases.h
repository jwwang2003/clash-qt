#ifndef CLASHQT_TESTS_CONTRACTS_BACKEND_COMMON_BACKEND_COMMON_CASES_H
#define CLASHQT_TESTS_CONTRACTS_BACKEND_COMMON_BACKEND_COMMON_CASES_H

// The SHARED MihomoBackend contract: one set of assertions, four
// implementations.
// Contract: .refactor/BACKEND_CONTRACT.md revision backend-r4 section 10,
// amendments A1-A4 and B1-B4; .refactor/P4_ABI_CONTRACT.md revision module-r1;
// decision D8.
//
// Every function below takes a BackendDriver & and reaches the implementation
// only through core::backend::MihomoBackend &. None of them names
// FakeBackend, MihomoBackendImpl or ModuleBackend, and none of them branches on
// which one it has: the four rows run the SAME statements over the same
// recorded events. That is what backend-r4 section 10 asks for and what r3 B3
// found missing - the fake and the real backend diverged on the re-issue
// obligation precisely because "the same contract tests" meant two files
// asserting similar things.
//
// A driver Ability gates only an ADDITIONAL, strictly stronger arm (attributing
// a completion to the controller that produced it needs two distinguishable
// controllers, which an in-process double does not model). No shared assertion
// is ever skipped for an implementation that finds it inconvenient.
//
// WHAT THESE CASES DO NOT COVER, AND WHERE IT LIVES INSTEAD
//   The readiness deadlines (silence-based idle deadline, absolute hard cap),
//   the probe-cancel ordering, the unconfirmed stop and the deliberate
//   abort-before-bump inversion are NOT here. They need either
//   core::CoreTimings - host-side by design, and absent from the facade a
//   module consumer is given - or a knob that a shipping backend must not
//   have. They stay in backend_real_contract_test.cpp and
//   contracts/backend/backend_contract_test.cpp, which say so in their own
//   headers. These cases claim no coverage of them.

namespace testsupport::backend::common {
class BackendDriver;
}

namespace testsupport::backend::common::cases {

/// Every published query answers from the instance, with the contract's own
/// readiness budget and interface revision. Section 3, 4, 9, C1.
void identityAndPublishedBudget(BackendDriver &driver);

/// RequestIds are unique and never reused, every completion carries back the
/// id it completes, a completion carries the generation its request was
/// SUBMITTED under (A1), and a string payload survives unchanged - including
/// non-ASCII, which is what a boundary that re-encodes would mangle.
void requestIdentityAndLosslessPayloads(BackendDriver &driver);

/// Registration is explicit: null is refused, a duplicate is refused, removing
/// an unregistered observer answers false, and a removed observer is not
/// invoked again. Section 7 rule 3.
void observerRegistrationIsExplicit(BackendDriver &driver);

/// An observer removed from inside a callback - its own or another's - receives
/// nothing further, including events already produced and still queued.
void removingAnObserverDuringDeliveryStopsFurtherCallbacks(BackendDriver &driver);

/// An observer added from inside a callback sees only what follows it.
void anObserverAddedDuringACallbackSeesOnlyLaterEvents(BackendDriver &driver);

/// An observer registered AFTER a mutating call produced its events, but
/// BEFORE any of them was delivered, must see none of them. This is the same
/// rule as the case above, at the point where it actually bites: delivery is
/// deferred, so "produced" and "delivered" are different moments and a
/// registration can fall between them.
void anObserverAddedBeforeDeliverySeesNoEarlierEvents(BackendDriver &driver);

/// No observer is invoked from inside a mutating call, including from the call
/// that clears the live view and from a mutation issued inside a callback.
/// Section 7 rule 1.
void deliveryIsNeverReentrant(BackendDriver &driver);

/// A completion abandoned by an endpoint change is MARKED Superseded (A2),
/// carries the generation it was submitted under (A1), compares older than the
/// newest observed generation, and carries no payload.
void anAbandonedCompletionIsMarkedSuperseded(BackendDriver &driver);

/// A generation bump that is not an endpoint change re-issues the snapshot set
/// (backend-r2's obligation on the single global generation, r3 B3), and an
/// endpoint change publishes the live view as empty rather than leaving it
/// stale.
void aNonEndpointBumpReissuesTheSnapshotSet(BackendDriver &driver);

/// Detaching never terminates an attached controller: re-attaching proves the
/// controller is still there. Section 1.
void detachLeavesTheAttachedControllerRunning(BackendDriver &driver);

/// With a managed core and an external controller alive at the same moment,
/// stop() ends the managed one and leaves the other attached, connected and
/// answering. Its StopCompleted is confirmed and carries the post-bump
/// generation, so a consumer applying the section 2 rule does not drop it
/// (A1, B1, B2).
void stopTerminatesOnlyTheManagedCore(BackendDriver &driver);

/// A candidate that fails validation leaves the running configuration intact:
/// no state change, the same endpoint, the candidate gone from the active set
/// and the running config still in it. Section 3.
void aFailedValidationLeavesTheRunningConfigurationIntact(BackendDriver &driver);

/// A duplicate provider submission returns the OUTSTANDING request's id and
/// settles as one completion, while a different one gets its own id
/// (backend-r2, answer 1).
void aDuplicateProviderRequestIsCoalesced(BackendDriver &driver);

/// The TUN completion reports what the controller says afterwards, not what
/// was asked for - in both directions. No real TUN device is ever created: the
/// read-back is a loopback fixture's answer or a staged one.
void theTunCompletionIsAReadBackNotAnEcho(BackendDriver &driver);

/// The privileged helper's own "a core is running under me" report reaches the
/// published status, and only on an answered query (C1).
void thePrivilegedStatusCarriesTheHelpersRunningCore(BackendDriver &driver);

}  // namespace testsupport::backend::common::cases

#endif  // CLASHQT_TESTS_CONTRACTS_BACKEND_COMMON_BACKEND_COMMON_CASES_H
