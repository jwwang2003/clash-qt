# MihomoBackend — contract revision `backend-r3`

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


---

# Amendments: `backend-r1` → `backend-r2`

BACKEND-FAKE implemented r1 and reported five defects rather than working around
them. Four are accepted and change the contract; consumers code against **r2**.

## A1 — Completion stamping (supersedes §2's last paragraph)

r1 said every event and completion "carries the `Generation` current when it was
produced". **That is self-defeating**: a completion stamped at delivery time always
looks current, so the consumer's rejection rule can never fire. Corrected:

- A **completion** carries the generation **current when its operation was submitted**.
  That is what makes it comparable against the consumer's last-observed value.
- Any **other event** carries the generation current when it was produced.
- The **terminal outcome of an operation that itself bumps** the generation (start,
  stop, managed failure) carries the **post-bump** value. Otherwise a consumer would
  reject the very failure it has to act on.

## A2 — Superseded completions must be marked (supersedes §10 bullet 2)

r1 asked consumers to reject a completion stamped with a superseded generation. With
r1's own non-re-entrant delivery (§7) that is **not observable from generations
alone**: the completions produced by an abort are queued *before* the invalidating
event, so the consumer's "older than last observed" test cannot catch them.

The backend must therefore mark them explicitly, with a `Superseded` completion
status. Generation comparison remains the consumer's second line of defence, not its
only one. §10 bullet 3 (bump before abort) is observable and stays as written — it is
what the ordering test asserts.

## A3 — `Endpoint` is standard-layout, not "POD" (refines §9)

r1 demanded `Endpoint` "becomes POD" while also permitting Qt types in r1's
implementation; those cannot both hold. r2 requires: standard-layout,
behaviour-free, `static_assert`ed, with URL construction removed from the type.
Strict trivial-copyability is a P4 layout change under COMPONENT-ABI, not an r2
claim.

## A4 — `StopCompleted` and `TunChangeCompleted` carry a `Generation` (fixes §6)

§2 requires a generation on every completion; §6's `StopCompleted` omitted it. Both
now carry one.

## Answers to the two open questions

**`ProviderClient::pending_` does not reduce to `RequestId`.** Evidence:
`provider_client.cpp:67,125` key it as `epoch + ':' + path`, and `:68,126` return
**without issuing anything and without minting an id**. Per-submission ids would mint
two and issue both. It is `(Generation, operation identity) → coalescing`. Published
rule: **a duplicate submission returns the outstanding request's id.** `busyChanged`
(`:59-62`) needs only cardinality, which ids supply. `sameEndpoint()` is redundant:
`endpoint_` changes only in `setEndpoint`, which always emits `endpointChanged`,
which bumps `epoch_`.

**One global `Generation` suffices — with one obligation.** `endpointEpoch_` and
`requestEpoch_` collapse cleanly, since `isCurrentReply` requires both to match and
either bump invalidates. `tunChangeId_` is **not** a generation at all — it is
incremented per operation and is a `RequestId`. `launchGeneration_` folds in, but a
single global counter means a managed start/stop/fail invalidates in-flight
*controller* replies, which does **not** happen today.

That difference is not benign. `fetchVersion`/`fetchProxies` recover via the 5 s poll
at `main.cpp:277-283`, but **`fetchRules` and `fetchConfigs` do not** — they are
issued only from `refreshState` and `main_window.cpp:132-133`, so a discarded
`/rules` or `/configs` reply would leave the rules list and `BaseConfig` stale until
the next endpoint change.

**Obligation, in place of a second counter:** any generation bump that is *not* an
endpoint change must re-issue the snapshot set. No case requires a second counter.

## Known surviving mutant, with justification

Of 15 inversions, 14 failed the suite. One survived: removing **one** of the two
observer skip-guards in the fake's `drain()`. They are genuinely redundant —
`removeObserver` erases from both containers — so the single-guard build is still
correct. Removing **both**, or iterating the live list, does fail. Recorded rather
than papered over, because a surviving mutant is either a coverage gap or a
redundancy, and here it is demonstrably the latter.


---

# Amendments: `backend-r2` → `backend-r3`

An independent audit implemented r2's rules as written and found the real backend
violating two of them. These are defects in the delivered code *and* gaps in the
contract's own specification.

## B1 — `StopCompleted` must carry the post-bump generation (A1 violated in practice)

`core_process.cpp:486-491` emits `failed` **before** `stopFinished`. The backend's
`failed` handler bumps the generation, while `stopFinished` stamps the
`stopGeneration_` captured at submit. The delivered order is therefore
`coreFailed(N+1)` then `stopCompleted(N)` — measured as `stopGen=2, coreFailedGen=3`.

A consumer applying §2's **mandatory** rejection rule drops the unconfirmed stop.
That is exactly the failure A1 was written to prevent, and it is live.

`ShutdownCoordinator` escapes only because it does not override `coreFailed`. Its own
comment asserts a guarantee the real backend does not provide. The fake stamps both
identically, so fake and real diverge and the fake cannot catch it.

**Required:** a stop completion carries the generation current *after* any bump its
own operation caused, per A1. Either `stopFinished` re-reads the generation at emit,
or the backend defers the bump until the terminal outcome is delivered. Both suites
must assert `!isSuperseded(result.generation, lastObserved)` on the unconfirmed path.

## B2 — Every completion type carries a `CompletionStatus` (A2 was unimplementable)

`types.h` gives `StopCompleted` and `TunChangeCompleted` no `CompletionStatus`, so A2's
`Superseded` marking cannot be expressed for them. Additionally, every TUN error is
labelled `Protocol`, including "cancelled because the controller changed", which is a
supersession and must be classified as one.

**Required:** both types carry `CompletionStatus`; supersession is never reported as a
protocol error.

## B3 — The fake must implement the re-issue obligation

r2 attached a re-issue obligation to the single global generation. The real backend
honours it; the fake does not — `start`, `stop`, `failManaged` and `setConnected(false)`
all bump without re-issuing, and no test covers it.

§10 requires the fake and the real backend to satisfy **the same** contract tests. They
currently do not. Either the fake implements the obligation, or §10's claim is false.

## B4 — "Passes against the locally built mihomo" is not currently true

§10 says the real backend additionally passes against the locally built engine.
`backend-real-contract` drives `clash-qt-fake-core`, not mihomo. The only suite that
drives the real engine is `real-core`-labelled and therefore excluded from `make test`.

**Required:** either the acceptance set runs against the real engine in the integration
lane, or §10 stops claiming it. The claim is not permitted to stand on a suite whose
name merely contains "real".

## Known weak coverage, to be closed with these fixes

Mutation testing found three acceptance bullets that the **real** suite does not
actually protect, though the fake suite does:

- **#5**, the hard cap: making `readyHardDeadlineMs_` refreshable survives the real
  suite, which asserts only a lower bound.
- **#1**, second half: no scenario ever has a managed core and an external controller
  alive at once, so "stop terminates only a managed core" is untested.
- **§5.2**, probe-cancel ordering: aborting before disconnecting survives the real suite.

A bullet that only the fake protects is a bullet the engine does not have to honour.
