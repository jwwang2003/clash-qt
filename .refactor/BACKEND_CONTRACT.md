# MihomoBackend — contract revision `backend-r1`

Coordinator-owned. MOD-CORE implements it against the real engine, MOD-RUNTIME and
MOD-LIFECYCLE consume it, and the fake backend under `tests/support/backend/`
implements it for their tests. **A worker that disagrees escalates; it does not
redesign.** Three packages code against this simultaneously.

Implements G2 and roadmap stage 3. Published headers live in `src/core/backend/`.

## Scope of r1, and what it is not

r1 is the **semantic** contract: an asynchronous facade with explicit request
identity, ownership and completion outcomes, expressed as a C++ interface. Existing
`CoreProcess`, `MihomoClient` and `ProviderClient` stay behind it.

r1 is **not** the binary boundary. Factory, loader, module versioning and ABI
qualification are COMPONENT-ABI's job in P4. r1 is designed so that work is possible
— see "ABI-readiness constraints" — but it does not claim binary portability, and
`clashqt_com` is not yet threaded through it.

Implement only capabilities the application actually consumes today.

## 1. Ownership: managed versus attached

The single most important distinction, and the one the current code gets right by
convention rather than by type.

- A **managed core** is a child process (or privileged lease) this component started.
- An **attached controller** is any endpoint the user or discovery pointed us at.

Rules, all currently load-bearing:

- `Stop()` applies **only** to a managed core. Detaching from an attached controller
  **must never terminate it**.
- The two are independently observable. Today `ui/main_window.cpp:324-328` compares
  the managed endpoint's host and port against the attached one to decide whether the
  running core is a known direct child, and `:340` shows "external core connected"
  when the managed state is `Stopped` but the client is connected. The contract must
  answer both questions directly instead of making consumers compare endpoints.
- Startup attaches to a discovered endpoint, then a managed launch **overwrites** that
  attachment (`main.cpp:269`, then `main.cpp:111-112`). That sequence must remain
  expressible.

`Ownership { Managed, Attached, None }` is reported on every state event.

## 2. Request identity and generations

Fire-and-forget plus a bare signal is **not sufficient**. The current implementation
carries **four independent counters and a pending-key set**, and every one is
load-bearing:

| Existing counter | Purpose |
| --- | --- |
| `MihomoClient::endpointEpoch_` | bumped in `setEndpoint`; invalidates replies for a previous endpoint |
| `MihomoClient::requestEpoch_` | bumped on disconnect, so an in-flight snapshot cannot repopulate a just-cleared offline view |
| `MihomoClient::tunChangeId_` | separate identity for the confirmed TUN operation |
| `CoreProcess::launchGeneration_` | bumped on start, stop and fail |
| `ProviderClient::epoch_` + `pending_` + `sameEndpoint()` | provider refresh deduplication |

r1 replaces these with **two explicit, published values**:

- `RequestId` — unique per submitted operation, returned synchronously by every
  mutating call. Every completion carries the `RequestId` it completes.
- `Generation` — monotonic, bumped by *any* event that invalidates outstanding work:
  endpoint change, disconnect, managed start, managed stop, failure. Every event and
  completion carries the `Generation` current when it was produced.

**Consumers must reject any completion or event whose `Generation` is older than the
one they last observed.** This is a contract obligation on the consumer, not an
optimisation, and it is what the ~20 `isCurrentReply` guards do today.

**Ordering rule, inherited and mandatory:** bump the generation *before* aborting
in-flight work. `mihomo_client.cpp:44` documents why — `finished()` may run
synchronously inside `abort()`, so an abort that precedes the bump delivers a
completion stamped with the generation it was meant to invalidate.

## 3. Readiness

`Ready` is **not** "the process started". A contract that says so is wrong.

Ready means: an HTTP `GET /version` against the parsed endpoint returned **200** with
a JSON object whose `version` field is a **string**.

The deadline is **silence-based, not elapsed-time-based**, and this must be preserved:

| Parameter | Value | Note |
| --- | --- | --- |
| Idle deadline | 10 000 ms | refreshed by **every log line** the core emits |
| Idle deadline, service mode | 60 000 ms | |
| Hard cap | 180 000 ms | absolute, not refreshable |
| Probe interval | 200 ms | |
| Per-probe transfer timeout | 2 000 ms | |

A core that keeps logging keeps its deadline alive. These values are part of the
contract; a consumer may not assume a fixed timeout.

**Validation precedes mutation.** A candidate configuration is validated by a
*separate child process* while the running core stays live. Only a validated
candidate is launched. A validation failure leaves the running configuration intact.

## 4. Lifecycle transitions

`CoreState { Stopped, Starting, Running, Stopping, Failed }` — fixed underlying type,
with a reserved range for future states so an unknown value from a newer module is
not undefined behaviour.

- **Restart waits for the prior exit.** A launch requested while a core is running is
  held pending, the running child is terminated, and the pending launch is consumed
  **only** when the retiring child's exit is observed. Termination escalates to a kill
  after 3 000 ms.
- `IsRestartPending()` must remain observable: it is consulted by the reload gate,
  and it is true while a pending launch, a validation, or service-mode parsing is
  outstanding.
- The reload gate the application applies today is
  `Running || Starting || (Stopping && IsRestartPending())`. r1 must let a consumer
  express exactly that without reaching into implementation state.

## 5. Cancellation — three distinct mechanisms, all preserved

1. **Validation cancel.** A cancelled validation child's configuration path **remains
   in the active set** until it actually exits, so a snapshot is not deleted from
   under a dying validator. Losing this is a real file-deletion race, not a tidiness
   issue.
2. **Probe cancel.** The readiness probe disconnects its signals *before* aborting, so
   the abort cannot deliver a completion into a torn-down handler.
3. **Endpoint/disconnect cancel.** Bump the generation, *then* abort every in-flight
   request (see the ordering rule in §2).

Runtime *configuration generation* cancellation belongs to `ProfileStore` on the
config side. r1 must **not** swallow it; it stays a separate concern owned by CFG-CORE.

## 6. What `Stop` guarantees

> **Stop means: no managed core that this component started is still running — or an
> explicit unconfirmed result carrying a reason.**

`StopCompleted { RequestId, bool confirmed, ErrorInfo reason }`.

- `confirmed == true`: the managed child's exit was observed.
- `confirmed == false`: lease cleanup was requested, but the privileged service
  disconnected before confirming the child exited. This is **not** a failure to
  report as success. The application turns it into a shutdown warning that blocks
  quit, and that behaviour must survive.

`Stop` additionally clears the endpoint, drops any pending launch, and cancels both
validation and the readiness probe.

## 7. Events: an observer interface, not signals

r1 publishes an observer/sink interface rather than Qt signals, with documented
thread affinity per callback.

**No re-entrant delivery.** The backend must not invoke an observer from inside a
mutating call. Events are delivered after that call returns, on the owning thread.

This is a deliberate *change* from current behaviour, not a transcription of it:
`clearLiveState()` emits **five** signals synchronously from within
`setEndpoint`/`setConnected`, so today a handler can re-enter the object mid-mutation.
A sink interface would inherit that hazard unchanged, so r1 forbids it, and the
contract tests must cover it.

Observer registration and removal are explicit, and removing an observer during
delivery must be safe.

## 8. Capability reporting

Managed and attached cores differ in what they support, as do service and direct
modes. Capabilities are **queried, not assumed** — this is what lets
`ui/service_settings.cpp` stop opening its own second `PrivilegedServiceClient`
alongside the one `CoreProcess` owns (decision D3), which is a correctness hazard
today: two live connections to one privileged socket.

## 9. ABI-readiness constraints on r1

r1 is a C++ interface and may use Qt types in its implementation. It must **not**
adopt these shapes, because COMPONENT-ABI cannot remove them later without breaking
every consumer:

- No `QObject *parent` in any published signature, and no implicit construction of a
  collaborator when one is null. Ownership is explicit.
- No process-global statics for capability queries. `discoverBinary`,
  `serviceSupported`, `serviceAvailable` and the system-proxy singleton become
  instance methods on the component or stay host-side: a loaded module gets its own
  copy of every static.
- No returning a reference into the component's heap. `Endpoint` is returned **by
  value**, and it becomes POD — its `baseUrl`/`httpBase`/`wsBase` inline methods build
  a `QUrl` and cannot cross a boundary.
- No exception may unwind across the interface. yaml-cpp throws
  (`YAML::BadConversion` and friends) throughout the configuration path; convert to
  error results at the edge.
- Every collection handed to a consumer has a documented ownership and lifetime rule.
  Today signals pass const references to temporaries with no rule at all.
- `CoreState` and every published enum has a fixed underlying type.

## 10. Acceptance

The fake backend and the real backend satisfy **the same contract tests**. A passing
fake proves the contract is implementable and the consumers are correct; it proves
nothing about the engine. The real backend additionally passes against the locally
built mihomo.

Contract tests must cover, each failing when the behaviour is broken:

- detach leaves an attached controller running; stop terminates only a managed core
- a completion stamped with a superseded generation is rejected
- generation is bumped **before** in-flight work is aborted
- readiness requires 200 + a string `version`, not process start
- the idle deadline is refreshed by log output, and the hard cap is not refreshable
- validation failure leaves the running configuration intact
- a restart consumes its pending launch only after the prior exit is observed
- a cancelled validation keeps its configuration path active until exit
- `confirmed == false` is reported as such and is not success
- no observer is invoked re-entrantly from within a mutating call
- removing an observer during delivery is safe

## Open questions for implementers to raise, not decide

`ProviderClient`'s `pending_` key set may or may not reduce to `RequestId` —
MOD-CORE reports which. Whether `Generation` is global or per-subject (endpoint,
managed lifecycle, TUN) is a genuine design question: r1 specifies **one global
generation**, and MOD-CORE must report if any current behaviour cannot be expressed
that way, rather than silently adding a second counter.
