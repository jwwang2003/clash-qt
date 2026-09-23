# Module API

This document is the contract for everything that crosses a boundary inside
clash-qt: the asynchronous backend facade the application codes against, the
portable object model that makes a separately built module possible, and the
binary boundary itself.

**Current revisions: `backend-r4`, `component-r1`, `module-r1`.**

A file in `src/` or `tests/` that cites a revision is citing this document. This
is the single definition of each of the three; nothing else defines them, and a
change to any rule below is a change to the revision that owns it.

The three layers are deliberately separate, and the separation is load-bearing:

| Revision | What it fixes | Where it lives |
| --- | --- | --- |
| `backend-r4` | the **semantic** contract: request identity, ownership, completion outcomes, expressed as a C++ interface that may use Qt types | `src/core/backend/` |
| `component-r1` | the **object model**: identity, reference counting, status codes, owned buffers. No Qt, no standard-library container in any published signature | `src/core/component/` |
| `module-r1` | the **binary boundary**: the exported entry point, the handshake, the command/event protocol, the loader and unloading rules | `src/core/component/abi/`, `src/integrations/component/` |

`backend-r4` does not claim binary portability. `component-r1` does not claim it
either — `QueryInterface` plus a C factory does not by itself make a C++ vtable
portable across compilers and runtimes. Only `module-r1` makes a compatibility
claim, and it makes it for one measured target.

For where these pieces sit in the application, see architecture.md. For how the
composition root wires them together at startup, see development.md.

---

# MihomoBackend — revision `backend-r4`

The asynchronous facade the application talks to. One object exposing five
facets — lifecycle, attachment, control, telemetry, capabilities — declared in
`src/core/backend/backend.h`.

A consumer that needs only part of the surface takes a reference to the facet it
needs and cannot reach the rest. The facets are separate base classes rather than
accessor methods returning pointers, so nothing hands out a reference into the
component and no facet can be deleted independently: each has a protected,
non-virtual destructor, and lifetime belongs to `MihomoBackend`.

Four implementations satisfy this contract today: the real adapter over the
existing engine collaborators, an in-process deterministic double, and each of
those again behind the module boundary. Section 10 requires all four to pass the
same assertions.

## 1. Ownership: managed versus attached

The single most important distinction in this contract, and the one the
pre-contract code got right by convention rather than by type.

- A **managed core** is a child process, or a privileged lease, that this
  component started.
- An **attached controller** is any endpoint the user or discovery pointed us at.

`Ownership { None, Managed, Attached }` is reported on every state event.

The rules, all currently load-bearing:

- **`stop()` applies only to a managed core.** Detaching from an attached
  controller must never terminate it. `BackendAttachment::detach()` closes the
  conversation and nothing else.
- **The two are independently observable.** A consumer asks
  `BackendAttachment::attachmentOwnership()` whether the controller it is talking
  to is its own managed child, and `isExternalControllerConnected()` whether it is
  connected to somebody else's. Before this contract the user interface answered
  both by comparing the managed endpoint's host and port against the attached
  one by hand, which is a question the backend can answer and a consumer cannot
  answer correctly.
- **Startup attaches first, then a managed launch overwrites that attachment.**
  The application attaches to a discovered endpoint before any launch, and a
  successful managed start re-attaches to the core's own endpoint. That sequence
  must stay expressible; `attach()` overwrites rather than refusing.

`BackendLifecycle::ownership()` answers for the managed core alone —
`Managed` while this component is running or starting one, `None` otherwise —
and is deliberately independent of what the client is attached to.

## 2. Request identity and generations

Fire-and-forget plus a bare signal is not sufficient, and this contract does not
offer it. Every mutating call returns a `RequestId` synchronously, and every
completion carries the `RequestId` it completes.

Two published values replace the four independent counters and the pending-key
set the pre-contract code carried:

- **`RequestId`** — unique per submitted operation, never reused within one
  backend instance, including across generations. `RequestId::Invalid` is
  returned by a call the backend rejected outright, and carried by an event that
  completes no request of the consumer's: an unsolicited failure, a stream
  sample, the empty collections published when live state is cleared.
- **`Generation`** — monotonic, bumped by *any* event that invalidates
  outstanding work: endpoint change, disconnect, managed start, managed stop,
  failure.

Both are scoped enumerations over `std::uint64_t`, so one cannot be passed where
the other is wanted.

### The stamping rule

The stamp names the generation of the **work the event reports on**, not the
generation current at delivery. Three cases, and the distinctions between them
are what make the consumer's rejection rule operative at all:

1. A **completion** carries the generation its request was **submitted** under.
2. Any **other** event — a state change, an endpoint change, a connect or
   disconnect, a log line, a stream sample, the empty collections published when
   live state is cleared — carries the generation current when it was produced.
3. The **terminal outcome of an operation that itself bumps** the generation
   (start, stop, a managed failure) carries the **post-bump** value.

The simpler rule "every event and completion carries the generation current when
it was produced" is self-defeating for case 1: a completion stamped at delivery
always looks current, so a consumer's rejection test could never fire. Case 3 is
the mirror image — the consumer has to *act* on a terminal outcome, so stamping
it with the generation its own operation just invalidated would make the consumer
drop the very event it was waiting for. Section 6 describes the delivered defect
that case 3 exists to prevent.

### The consumer obligation — two checks, not one

1. **Drop any completion whose `status` is `CompletionStatus::Superseded`.** The
   backend marks abandoned work; the consumer does not have to deduce it.
2. **Reject any event whose generation is older than the newest one already
   observed.** `isSuperseded(stamp, observed)` in `core/backend/types.h` is that
   test.

Check 2 alone is **not** sufficient, though it looks as though it should be.
Under the delivery order of section 7, the completions an abort produces are
queued *before* the event that announces the bump, so at the moment they are
delivered the consumer's last-observed generation is still the pre-bump value and
the comparison says "current". Check 1 is what catches them. Check 2 is the
second line of defence, for an event that arrives after a bump the consumer has
already seen.

One exemption, and it is deliberate: the managed core's terminal outcomes
(`coreReady`, `coreFailed`, `stopCompleted`) carry the post-bump generation, so
check 2 never fires on them. Uniform filtering would drop the unconfirmed stop
that blocks quit.

A Qt consumer does not implement either check itself. `BackendBridge` discharges
the obligation once, at the single point where the sink becomes signals, and does
not republish the generation. Sixty-six call sites guarding individually would be
sixty-six chances to omit one, and an omission is silent.

### The ordering rule

**Bump the generation *before* aborting in-flight work.** `finished()` may run
synchronously inside `abort()`, so an abort that precedes the bump delivers a
completion stamped with the very generation it was meant to invalidate.

This ordering is observable — but through the completion's *status*, not through
its arrival order. A backend that bumps first marks each abandoned completion
`Superseded`; one that aborts first delivers the stale reply as live `Ok` data.
That difference is what the contract test asserts.

### One global generation, and the obligation that comes with it

There is **one** global `Generation`, not one per subject. The endpoint and
request epochs of the pre-contract code collapse cleanly, because the old
`isCurrentReply` guard required both to match and either bump invalidated. The
TUN change identifier was never a generation at all — it was incremented per
operation, which makes it a `RequestId`. The launch generation folds in.

Folding the launch generation in has a consequence that is **not** benign: a
managed start, stop or failure now invalidates in-flight *controller* replies,
which the pre-contract code did not do. `/version` and `/proxies` recover through
the composition root's five-second poll. `/rules` and `/configs` do not — they
are issued only from an explicit refresh, so a discarded reply would leave the
rules list and the base configuration stale until the next endpoint change.

**The re-issue obligation, in place of a second counter:** any generation bump
that is **not** an endpoint change must re-issue the snapshot set
(`refreshVersion`, `refreshProxies`, `refreshRules`). An endpoint change
discharges it by re-fetching on attach; a disconnect, a managed start, a managed
stop and a managed failure all owe it.

This binds the fake as well as the real backend. It was once honoured by the real
one and not by the fake, with no test covering it, which is precisely the
divergence section 10 exists to prevent.

### Provider coalescing

Provider work is keyed by `(Generation, operation identity)`, and a duplicate
submission under the current generation issues nothing at all. Per-submission ids
would mint two and issue both. **Published rule: a duplicate submission returns
the outstanding request's id.** `isProviderBusy()` needs only cardinality, which
ids supply.

## 3. Readiness

**`Ready` is not "the process started".** A contract that says so is wrong, and a
consumer that believes it will hand the user a controller that is not listening.

Ready means: an HTTP `GET /version` against the parsed endpoint returned **200**
with a JSON object whose `version` field is a **string**.

The deadline is **silence-based, not elapsed-time-based**, and this must be
preserved. The published budget is `BackendTimings`, returned by
`BackendCapabilities::timings()`:

| Parameter | Value | Note |
| --- | --- | --- |
| `idleDeadlineMs` | 10 000 | refreshed by **every log line** the core emits |
| `serviceIdleDeadlineMs` | 60 000 | the same, under the privileged service |
| `hardCapMs` | 180 000 | absolute, **not** refreshable |
| `probeIntervalMs` | 200 | |
| `probeTimeoutMs` | 2 000 | per-probe transfer timeout |
| `terminateWaitMs` | 3 000 | termination escalates to a kill after this (section 4) |

A core that keeps logging keeps its deadline alive. These values are part of the
contract, and a consumer may not assume a fixed timeout — that is why they are
published as data rather than left as constants a consumer would have to guess
at or duplicate. They are instance state in the engine adapter rather than
file-scope constants, so a test can observe a silence-based deadline being
refreshed without waiting ten real seconds, and so `timings()` reports the
backend's real budget rather than a constant that has drifted from it.

**Validation precedes mutation.** A candidate configuration is validated by a
*separate child process* while the running core stays live. Only a validated
candidate is launched. A validation failure leaves the running configuration
intact: the state does not change, the endpoint does not change, and the running
configuration stays in `activeConfigPaths()`. A validation failure while a core
is running arrives on `coreFailed()` **without** a state change.

## 4. Lifecycle transitions

`CoreState { Stopped, Starting, Running, Stopping, Failed }` over
`std::uint8_t`, with reserved ranges so an unknown value from a newer module is a
value the enumeration can legally hold rather than undefined behaviour:

- `0..4` the states above;
- `5..63` reserved for future revisions of this contract;
- `64..255` reserved for backend-specific states, never interpreted by a consumer.

A consumer switches on the known values and treats anything else as `Failed`'s
conservative neighbour: unknown means "do not assume the core is usable".
`isKnownCoreState()` is that test.

**Restart waits for the prior exit.** A launch requested while a core is running
is held pending, the running child is terminated, and the pending launch is
consumed **only** when the retiring child's exit is observed. Termination
escalates to a kill after `terminateWaitMs`.

**`isRestartPending()` is observable**, because the reload gate consults it. It
is true while a pending launch, a validation, or service-mode configuration
parsing is outstanding.

**The reload gate is published as one predicate.** `isManagedCoreActive()` is
exactly `Running || Starting || (Stopping && isRestartPending())`. It exists so a
consumer can express that condition without reaching into implementation state
and without re-deriving it; the application's reload scheduling is built on it.

`setExecutionMode()` is accepted only while the managed core is `Stopped` or
`Failed` and nothing is in flight. It returns `false`, changing nothing,
otherwise — including when the mode is unsupported on this host. Selecting the
privileged service when the component was handed none is **refused** rather than
silently accepted and then wedged: a component that was given no privileged
service genuinely has none, and pretending otherwise is how a stop that can never
be confirmed gets reported as a success.

`usesPrivilegedService()` is true only while a lease is actually held, which is
not the same as having selected the privileged mode.

## 5. Cancellation

Three distinct mechanisms, all preserved. They are numbered, and other files cite
them by number.

### 5.1 Validation cancel

A cancelled validation child's configuration path **remains in
`activeConfigPaths()` until it actually exits**, so a snapshot is not deleted from
under a dying validator. Losing this is a real file-deletion race, not a tidiness
issue.

`activeConfigPaths()` therefore returns four kinds of path: the running core's, a
validating candidate's, a pending launch's, and every cancelled validation
child's that has not exited yet. It is returned by value; the caller owns the
copy.

### 5.2 Probe cancel

The readiness probe **disconnects its signals before aborting**, so the abort
cannot deliver a completion into a torn-down handler. `QNetworkReply::abort()`
emits `finished()` synchronously, which is what makes the order matter.

This ordering is unobservable from outside on its own — the handler's own
identity guard swallows a late completion, so inverting the two statements
changes nothing a test can see and the acceptance bullet would pass vacuously.
The engine adapter therefore counts readiness-probe completions that arrive after
the probe that produced them was cancelled, and that counter must always read 0.
A seam that exists only so an assertion has something to assert is still part of
the contract; without it the rule is a comment.

### 5.3 Endpoint and disconnect cancel

Bump the generation, *then* abort every in-flight request (the ordering rule in
section 2). Every request the abort abandons completes with
`CompletionStatus::Superseded` and `ErrorCode::Superseded`.

`attach()` bumps, aborts, publishes the cleared live state, re-opens whatever
streams were open, and re-issues the snapshot set — which is what discharges the
re-issue obligation for an endpoint change rather than deferring it. `detach()`
does the same except the re-issue: there is no endpoint left to fetch from.

Runtime *configuration generation* cancellation is a separate concern and belongs
to the configuration side; this contract does not swallow it. See
configuration.md.

## 6. What `stop()` guarantees

> **Stop means: no managed core that this component started is still running — or
> an explicit unconfirmed result carrying a reason.**

The terminal response is `StopCompleted { request, generation, status, confirmed,
reason }`.

- `confirmed == true`: the managed child's exit was observed.
- `confirmed == false`: lease cleanup was requested, but the privileged service
  disconnected before confirming the child exited. **This is not a failure to
  report as success.** The application turns it into a shutdown warning that
  blocks quit, and that behaviour must survive.

`stop()` additionally clears the managed endpoint, drops any pending launch, and
cancels both the validation child and the readiness probe. Like `start()`, it
bumps the generation and therefore owes the re-issue of section 2.

`StopCompleted` carries a `Generation` because section 2 requires one on every
completion, and it carries a `CompletionStatus` because an abandoned completion
has to be *marked*: a bare `confirmed` flag cannot express supersession, since
`false` would be indistinguishable from unconfirmed lease cleanup, which is a
different thing entirely. The three statuses that reach it are `Ok` (the exit was
observed), `Failed` (cleanup was requested and nothing confirmed the exit) and
`Superseded` (a newer stop replaced this one). `confirmed` stays, because this
section is written in terms of it and the application's shutdown warning keys on
it.

**The generation on `StopCompleted` is the post-bump value**, and this was a
delivered defect before it was a rule. The engine adapter emits `failed` before
`stopFinished`; the `failed` handler bumps the generation while a submit-time
stamp does not, so the delivered order was `coreFailed(N+1)` then
`stopCompleted(N)`. A consumer applying section 2's mandatory rejection rule
therefore dropped the unconfirmed stop — and the quit it was supposed to block
proceeded. Both the fake and the real backend must assert that an unconfirmed
stop is not superseded against the consumer's last-observed generation.

`coreStopped()` is emitted alongside `stopCompleted()` on the clean path. A
consumer that only cares about the terminal answer to `stop()` uses
`stopCompleted()`.

## 7. Events: an observer interface, not signals

This contract publishes an observer sink — `BackendObserver` — rather than Qt
signals, with documented thread affinity per callback. Every method has an empty
default body, so a consumer overrides only what it uses. A completion callback
whose status is not `Ok` carries no meaningful payload: the payload parameters
are then empty, not stale.

**Four delivery rules, all obligations on the backend.**

1. **No re-entrant delivery.** The backend must not invoke an observer from
   inside a mutating call. Every event a mutating call produces is delivered
   *after* that call has returned, on the owning thread.

   This is a deliberate *change* from the pre-contract behaviour, not a
   transcription of it: the engine client emitted five signals synchronously from
   within its endpoint and connection setters, so a handler could re-enter the
   object mid-mutation. A sink interface would have inherited that hazard
   unchanged, so this contract forbids it and the contract tests cover it.

2. **Order is preserved.** Events are delivered in the order they were produced.
   Note what that means for an abort: the completions it produces are queued
   *before* the event that announces the bump, so they reach the consumer first.
   That is why section 2 requires supersession to be marked rather than inferred.

3. **Removal during delivery is safe.** An observer may remove itself, or any
   other observer, from inside a callback. A removed observer receives no further
   callbacks, **including ones already produced and still queued**. Adding an
   observer during delivery is also safe; it sees only events produced after it
   was added.

4. **Nothing unwinds.** Every callback is `noexcept`. An observer that lets an
   exception escape terminates the process — it does not corrupt the backend's
   queue.

**Admission is by production, not by arrival.** An observer added after an event
was produced does not receive it, even if that event has not yet been delivered.
In process this costs nothing, because the producing queue and the observer list
are the same object. Across the module boundary they are not, which is why
`module-r1` carries a production sequence in every event envelope.

**Registration and lifetime.** `addObserver` returns `false` for a null pointer
or one already registered; `removeObserver` returns `false` when the observer was
not registered and is safe to call from inside a callback, including on the
observer currently being invoked. The backend does not own an observer and never
extends its lifetime: an observer must be removed before it is destroyed.

**Thread affinity.** Every callback is invoked on the backend's owning thread —
the thread that created it — whatever thread the underlying transport used.
Stream samples included.

**Borrowing.** Every reference and `Span` parameter is borrowed for the duration
of the call. An observer that needs the data afterwards copies it. The backend
never frees anything on the consumer's behalf and never retains a pointer the
consumer gave it.

A Qt consumer does not implement this sink. `BackendBridge` is the one adapter —
one observer in, Qt signals out — and it copies every `Span` into an owned
container *before* any emission, because Qt consumers keep what they are given.
No `Span`, and no pointer or reference derived from one, appears in any bridge
signal or outlives the callback that produced it.

## 8. Capability reporting

Managed and attached cores differ in what they support, as do the privileged and
direct execution modes. **Capabilities are queried, not assumed.**

`FeatureSet features()` answers over the `Feature` bit flags: managed lifecycle,
privileged service, confirmed TUN change, DNS query, DNS cache flush, geo
database update, memory stream, providers, configuration validation.

`BackendIdentity identity()` carries the backend's name, its module ABI version
and its `interfaceRevision`. **`interfaceRevision` is not the revision number of
this document.** It is 1, and it has been 1 across every revision of this
contract, because no revision has added or reordered a facet method — each has
tightened semantics or added status data to an existing type. It moves when the
vtable does, which under section 9 is a new interface id rather than a mutated
one. The shared suite asserts the value for all four implementations.

`serviceSupported()` and `serviceAvailable()` are **instance** methods. They were
process-global statics, and section 9 explains why that could not survive. Making
them instance methods is also what let the service settings page stop opening a
*second* privileged-service client alongside the one the backend already owns:
two live connections to one privileged socket is a correctness hazard, not a
layering complaint.

`requestPrivilegedServiceStatus()` is asynchronous, because answering it means
talking to the service. It completes on `privilegedServiceStatus()` with a
`PrivilegedServiceStatus { state, version, coreRunning, error }`.

**`coreRunning` means "a core is alive under the helper, possibly another
application session's".** The macOS helper keeps exactly one core per serving
process and answers *every* connection — lease-holder or not — with a state
derived from that core's process id. So `coreRunning` is emphatically **not** this
backend's `CoreState`; a consumer that wants its own core asks `state()`.

**The fail-safe rule.** `coreRunning` is meaningful only when the query was
actually answered — `state == ServiceState::Connected` with no error. On any
other outcome nothing was reported and the field stays `false`. A consumer
guarding on it **writes its own flag only on an answered status query**: an
unanswered or failed query must not disarm an already-armed guard, and must not
be read as "no core is running".

This field exists because of a delivered defect, and the shape of that defect is
the reason this section matters in practice. Removing the settings page's second
privileged-service client removed the only writer of its "a service core is
running" flag. The flag stayed `false` for an entire wave while being read in
three places: the uninstall guard never fired, the controls that should have been
disabled stayed enabled, and the notice that a service core was still running
could never appear. **The privileged helper could be uninstalled while another
session's core was running under it.** Nothing asserted the guard, so nothing
noticed.

The general rule that follows: **removing a producer discharges nothing until the
contract answers in its place.** Three acceptance cases assert the guard
directly, and it is proven by inversion at each of the three layers — dropping
the consumer's write, having the bridge publish `false`, and hard-coding `false`
in the backend each fail.

On the Qt side, `BackendBridge::privilegedServiceStatus` takes `coreRunning` as
an **appended** argument, so handlers written against the three-argument form
still connect unchanged.

## 9. Boundary constraints on the published interface

This is a C++ interface and may use Qt types in its implementation. It must
**not** adopt the shapes below, because the module boundary cannot remove them
later without breaking every consumer. Each is enforced throughout
`src/core/backend/`.

- **No `QObject *parent` in any published signature**, and no implicit
  construction of a collaborator when one is null. Ownership is explicit.
- **No process-global statics for capability queries.** A loaded module gets its
  own copy of every static, so binary discovery, service support and service
  availability are instance methods on the component or stay host-side.
- **No returning a reference into the component's heap.** `Endpoint` is returned
  **by value**. It is a different type from `core::Endpoint` precisely because
  that one's URL-building methods construct a `QUrl` inline and cannot cross a
  boundary; URL construction belongs to whichever side owns the transport and is
  not part of the published surface.
- **`Endpoint` is standard-layout and behaviour-free**, `static_assert`ed as
  such. Strictly trivially-copyable POD is unreachable while the struct carries a
  `QString`, and this section also permits Qt types in an in-process C++
  interface, so the two demands cannot both hold. Standard layout with no
  virtuals and no member behaviour is what is required and what is asserted.
  Comparison is published as free functions — `isValid`, `isSameEndpoint`,
  `isSameAddress` — so the record itself stays data.
- **No exception may unwind across the interface.** Every published method is
  `noexcept`. yaml-cpp throws throughout the configuration path; those exceptions
  are converted to `ErrorInfo` at the edge. This is why
  `endpointFromConfigFile()` takes an out-parameter and returns `bool` rather
  than returning an optional or throwing.
- **Every collection handed to a consumer has a documented ownership and lifetime
  rule.** `Span<T>` is that rule: borrowed for the duration of the callback, copy
  if you need it afterwards. The pre-contract signals passed const references to
  temporaries with no rule at all.
- **Every published enumeration has a fixed underlying type.**

The Qt-facing interface **stays host-side**, including `Endpoint`'s `QString`
members and the facade's public virtual destructor. The module boundary does not
change this interface's layout: it uses its own fixed-layout values and UTF-8 byte
ranges, and a host-owned shim converts between the two. Existing vtables and
consumers do not change, and `interfaceRevision` stays 1. `ErrorCode` here is
distinct from `component-r1`'s `Result`; the module boundary maps one onto the
other.

One published header in this directory is deliberately *not* part of the
boundary-crossing surface: `core/backend/privileged_core_service.h`. It is the
seam by which the composition root injects privileged execution, it uses
`QJsonObject` and non-`noexcept` methods, and it never crosses a binary boundary
— `module-r1` marshals it explicitly as a reverse interface. It is published so
that the composition root can see both sides, because the core tree may not
include a platform header and the composition root may not include the engine's
private headers.

## 10. Acceptance

**The same assertions run against every implementation of this facade.** A
passing double proves the contract is implementable and the consumers are
correct; it proves nothing about the engine. That is why there are four rows, not
two:

| Row | Subject |
| --- | --- |
| in-process double | the deterministic fake, no sockets, no processes, no wall clock |
| in-process real | the engine adapter, real child processes, a real loopback controller |
| module, real | the shipping module artifact, loaded through the production loader |
| module, fake | the test module artifact, through the same loader |

The shared cases reach their subject **only** through `core::backend::MihomoBackend &`.
None of them names an implementation and none of them branches on which one it
has. "Two suites asserting similar things" is not the same requirement and does
not satisfy it: the fake and the real backend once diverged on the re-issue
obligation of section 2 precisely because each was held to its own file.

The two module rows really do load a shared library and talk to it over the
binary boundary. Neither wraps an in-process implementation and neither falls
back to one; a missing artifact or a module that will not load is a **failure**
in that lane. Substituting a mock would assert the boundary against itself.

A driver capability may gate an **additional, strictly stronger** arm — for
instance, attributing a completion to the controller that produced it needs two
distinguishable controllers, which an in-process double does not model. No shared
assertion is ever skipped for an implementation that finds it inconvenient.

**The behaviours the acceptance set must cover**, each failing when the behaviour
is broken:

- detach leaves an attached controller running; stop terminates only a managed core;
- a completion abandoned by an abort is marked `Superseded` and carries the
  generation it was submitted under;
- the generation is bumped **before** in-flight work is aborted;
- readiness requires 200 and a string `version`, not process start;
- the idle deadline is refreshed by log output, and the hard cap is not
  refreshable;
- validation failure leaves the running configuration intact;
- a restart consumes its pending launch only after the prior exit is observed;
- a cancelled validation keeps its configuration path active until exit;
- `confirmed == false` is reported as such, is not success, and is not superseded
  against the consumer's last-observed generation;
- no observer is invoked re-entrantly from within a mutating call;
- removing an observer during delivery is safe, and an observer added before
  delivery sees no earlier events;
- request ids are unique and never reused, and a string payload survives
  unchanged — including non-ASCII, which is what a boundary that re-encodes
  would mangle;
- every published query answers from the instance, with this contract's own
  readiness budget and interface revision;
- the privileged uninstall guard fires on an answered status query and is not
  disarmed by an unanswered one.

**What the shared cases deliberately do not cover, and where it lives instead.**
The readiness deadlines, the probe-cancel ordering of section 5.2, the
unconfirmed stop and the deliberate abort-before-bump inversion are **not** in
the shared set. They need either the engine's timing seam — host-side by design,
and absent from the facade a module consumer is given — or a knob a shipping
backend must not have. They stay in the specialist suites, which say so in their
own headers, and the shared cases claim no coverage of them. A driver that cannot
express a step says so through a capability rather than by quietly weakening a
shared assertion.

**Claims must rest on what actually ran.** The claim that this contract holds
against the locally built engine is carried by the suite that drives that engine,
label-gated to the integration lane — not by a suite whose name merely contains
"real". The suite that drives a fixture core says that is what it drives. The
bullets the real engine cannot drive are enumerated in that suite's header with
reasons: the engine always answers `/version` with 200 and a string, so the
negative arms are unreachable; a core that comes up, keeps logging and never
answers cannot exist; and the unconfirmed stop is a property of the privileged
helper, not the engine. The TUN supersession case is drivable in principle but
declined, because it would ask the engine to create a TUN device on a developer's
machine.

**Inversion, not assertion count.** Each case is validated by temporarily
inverting the behaviour it protects and confirming the suite fails. A test that
passes against a deliberately broken implementation is not coverage. Where a
surviving mutant is found it is recorded as either a coverage gap or a
demonstrated redundancy, with the evidence for which — a surviving mutant that is
neither explained nor closed is an unexamined hole.

See testing.md for how the lanes are organised and which are gated.

## Earlier revisions of this contract

Some files still cite `backend-r3`. They are citing an earlier state of *this*
document, and the sections they name are the sections above. The four revisions
differ as follows, and nothing below is separately normative — the body is:

- **r1** established the facade: ownership, request identity, generations,
  readiness, the observer sink and the boundary constraints.
- **r2** corrected the stamping rule (a completion carries its submit-time
  generation, a self-bumping operation's terminal outcome carries the post-bump
  one), required supersession to be **marked** rather than inferred, relaxed
  `Endpoint` from "POD" to standard-layout, and put a `Generation` on
  `StopCompleted` and `TunChangeCompleted`.
- **r3** made every completion type carry a `CompletionStatus`, required the
  post-bump stamp on `StopCompleted` against a measured violation, bound the fake
  to the re-issue obligation, and refused to let the real-engine claim rest on a
  suite that did not drive the real engine.
- **r4** added `PrivilegedServiceStatus::coreRunning` and its fail-safe rule, and
  recorded that the module boundary adapts rather than reshapes this interface.

---

# Portable component object model — revision `component-r1`

The object model in `src/core/component/`, in namespace `clashqt::com`, built as
the target `clashqt_com`. It is what makes a separately built module possible:
identity, reference counting, status codes and owned buffers, with **no Qt type
and no standard-library container in any published signature**.

`component-r1` claims a C++ object model. It does **not** claim binary
compatibility: `QueryInterface` plus a C factory does not by itself make a C++
vtable portable across compilers and runtimes. Every supported target is
certified explicitly under `module-r1`.

## Provenance

This is an independent design. A pre-existing object model was read during
design as a source of prior art, and nothing was taken from it: no
implementation was copied, and no prefix, namespace, header path, target name or
user-facing label of that reference appears anywhere in the published API. That
reference carried no licence, so it is not redistributed with this project and
is not present in this repository; it was never modified and never built here.

It is described below only to record what was deliberately done differently,
because each difference is the reason a rule in this contract is written the way
it is.

Three defects in that reference are deliberately not reproduced, and each one is
the reason a rule below is written the way it is:

| Reference defect | What it does | This contract |
| --- | --- | --- |
| The status type is unsigned, and failure is tested with `value < 0` | `IsFailure` is *always false* and every negative constant wraps to a large positive. Every failure reads as a success | `Result` is `std::int32_t`. Negative is failure. `IsSuccess`/`IsFailure` are inline `constexpr` functions, never macros |
| The base interface id is all zeroes | An uninitialised id compares equal to it | Every interface, including the base, has a freshly generated owned id |
| Two distinct error conditions share one value | The two are indistinguishable at every call site | Every code is distinct, and the distinctness is `static_assert`ed and tested |

The signedness defect is the one worth internalising: it is invisible in review,
it makes every expected-failure test pass, and it is why every acceptance case
below insists on observing a *genuine* failure rather than a failing-looking one.

## Names

| Concept | This project |
| --- | --- |
| Namespace | `clashqt::com` |
| CMake target | `clashqt_com` |
| Include root | `core/component/…` (`src` is the include root) |
| Identity | `InterfaceId` |
| Status | `Result` |
| Base interface | `IObject` |
| Owning pointer | `ComPtr<T>` |

## Identity

`InterfaceId` is the 128-bit identifier that names an interface, a class or a
module. The same type serves all three roles, aliased as `ClassId` and
`ModuleId`.

**An id names an immutable vtable.** Once published, method order and signatures
never change. An incompatible interface gets a **new id**; both may be exposed
through `QueryInterface` during a migration. This is the whole mechanism by which
a module built against an older header stays safe.

`MakeInterfaceId` is `consteval`, so an id is always a compile-time constant and
a malformed literal, or one of the wrong length, fails to compile rather than
producing a silent zero. `FormatInterfaceId` writes into a caller-provided
buffer, so no string type crosses the published boundary. Ids have a strict total
order so they can key an ordered container; which id sorts first is not a public
promise.

Owned identifiers, freshly generated and permanently reserved:

| Interface | Id |
| --- | --- |
| `IObject` | `247a1b90-ece9-43a9-ab82-5739bdff6445` |
| `IComponentModule` | `66fdef80-2cd3-4808-ac33-547172c2952f` |
| `IErrorInfo` | `e83b4037-e096-4ae8-8f94-0f56ad29568a` |
| `IBuffer` | `8fdacf5e-be9a-47e0-bb57-1375a2322e54` |

Two further ids are **reserved and deliberately not declared**:
`5fd359f7-579a-4bcf-9743-cbbb8c5da432` (weak reference) and
`558f7604-facb-4122-86a8-16f66d58eac2` (weak source). Reserving an id prevents a
later collision; it promises nothing, and `component-r1` does not build weak
references.

`InterfaceTraits<T>` binds a C++ interface type to its id, declared through
`CLASHQT_COM_DECLARE_INTERFACE_ID`. The primary template is deliberately
incomplete: querying for a type that has no published id is a **compile error**,
not a zero id.

## `Result`

`using Result = std::int32_t;` with `IsSuccess(r) { return r >= 0; }` and
`IsFailure(r) { return r < 0; }`.

| Code | Value | Meaning |
| --- | --- | --- |
| `kOk` | `0` | Operation succeeded. |
| `kFalse` | `1` | Succeeded, and the answer is negative. Still a success. |
| `kFail` | `-1` | Unspecified failure. |
| `kNoInterface` | `-2` | The object does not implement the requested id. |
| `kNotImplemented` | `-3` | In the vtable, not implemented by this object. |
| `kInvalidArgument` | `-4` | A caller-supplied argument is unusable. |
| `kNotFound` | `-5` | A named entity does not exist. |
| `kTimeout` | `-6` | A bounded wait expired. |
| `kCancelled` | `-7` | Cancelled before completion. |
| `kUnsupportedVersion` | `-8` | ABI or interface version negotiation failed. |
| `kInvalidState` | `-9` | The object is not in a state that permits this call. |
| `kAlreadyClosed` | `-10` | The object has been closed or drained. |

Values are permanently stable; new codes append. `kFalse` exists so that a
successful "no" is not reported as a failure — the module lifetime interface uses
it to mean "nothing is alive, but the image must stay mapped", which is a
measured answer and not a failure.

The predicates are inline functions rather than macros because they obey scope,
respect the signed type, and can be used in constant expressions. The signedness
itself is `static_assert`ed, as is the distinctness of the codes, because both
are exactly what the reference got wrong.

## `IObject`

Three methods, and the vtable is exactly those three:

```
struct IObject {
    virtual Result QueryInterface(const InterfaceId& id, void** out) noexcept = 0;
    virtual std::int32_t AddRef() noexcept = 0;
    virtual std::int32_t Release() noexcept = 0;
};
```

The required behaviour, stated exactly, because these are the assertions:

1. **`out` is null** → return `kInvalidArgument`. Nothing is written, no
   reference is taken.
2. **Unsupported id** → write `nullptr` to `*out` **first**, *then* return
   `kNoInterface`. The write precedes the return so a caller that ignores the
   code cannot read an uninitialised pointer. The count is unchanged.
3. **Supported id** → write a non-null pointer and return `kOk`, having taken
   **exactly one** new strong reference. The caller owns that reference and must
   `Release()` it. A failed query never changes the reference count.
4. **Identity.** Querying `IObject` on any interface of one object yields the
   same pointer value; that pointer is the object's identity. Queries are
   reflexive, symmetric and **stable for the object's lifetime** — an id that
   succeeds once succeeds for as long as the object lives, and one that fails
   always fails. Multiple inheritance means *interface* pointers may legitimately
   differ; only the `IObject` pointer is the identity.
5. **`AddRef`/`Release` are atomic** and need no external synchronisation. They
   return the count after the operation. Only a returned `0` from `Release` is
   reliable, and it means the object was destroyed; calling any method on the
   pointer afterwards is undefined. The returned value is a debugging aid, not a
   synchronisation primitive.
6. **Atomic refcounting is not method thread-safety.** That is a statement about
   the *count* only. Each interface documents its own thread affinity separately,
   and where an interface says nothing, its calls belong to the thread that
   created the object.
7. **Destruction happens in the allocating module.** `Release` reaching zero runs
   the destructor and frees the storage inside the module that constructed it.
   Memory is never freed across a module boundary by the consumer's allocator.
   The destructor is protected and non-virtual for exactly this reason: `delete`
   on an `IObject*` does not compile.

`object_support.h` exists so these rules live in one implementation rather than
being re-derived, and re-broken, in every implementing class. `ResolveInterface`
is the whole of rule 1–3; `ReferenceCount` starts at 1, because a freshly
constructed object is already owned by whoever constructed it, and decrements with
acquire-release ordering so every previous owner's writes are visible to the
destructor. Under multiple inheritance, the `identity` passed to
`ResolveInterface` must be the same canonical branch the `IObject` row points at,
so that one object has exactly one count.

## `ComPtr<T>`

The intrusive owning pointer. **The two ways to take ownership are named**, so
they cannot be confused at a call site:

- `ComPtr<T>::Adopt(p)` — takes an **already-owned** reference; does **not**
  `AddRef`. This is what `QueryInterface` and every factory output feeds.
- `ComPtr<T>::Retain(p)` — takes a **borrowed** pointer; calls `AddRef`.

There is no constructor from a raw pointer and no implicit conversion back to
one. `Get()` is explicit about handing out a borrowed pointer, which the caller
must not release.

Copy retains, move transfers, destruction releases. Copy assignment acquires the
incoming reference **before** releasing the outgoing one, so self-assignment
cannot destroy the object it is about to keep. `Put()` / `GetAddressOf()` returns
a `T**` for output parameters and releases any previously held value first, so a
pointer cannot leak by being overwritten; `PutVoid()` is the same shaped for a
`void**` out-parameter. `Detach()` yields ownership to the caller. `As<U>(out)`
queries the held object for another interface and leaves `out` empty on every
failure.

## `IComponentModule`, `IErrorInfo`, `IBuffer`

**`IComponentModule`** is the queryable root a loaded module hands back: the
module ABI version, the module's own identity, a human-readable UTF-8
NUL-terminated description owned by the module and valid for as long as it stays
loaded, and `CreateObject(classId, interfaceId, out)`. Its failure paths are
distinguished — an unknown class is `kNotFound`, a known class that does not
implement the interface is `kNoInterface` — and every one writes `nullptr` first.
It is **free-threaded**: a module root is reached by a loader and by consumers on
whatever thread needs an object. Objects it creates carry their own affinity,
declared by their own interfaces.

The module ABI version is the version of the **handshake**, distinct from the
revision of any individual interface. An interface is versioned by its id; this
number is not.

The exported C factory entry and the host/module handshake belong to `module-r1`.
`component-r1` defines only the interface that entry returns; nothing in
`src/core/component/` loads, unloads or looks up a shared library.

**`IErrorInfo`** carries an optional diagnostic alongside a failure `Result`: the
code, a UTF-8 message and an optional source tag, the last two as `IBuffer`s.
**It is retrieved from the failing object**, by querying it — never from
thread-local or process-global state, which cannot survive a module or thread
boundary and silently reports the wrong error. `kNoInterface` from that query
means the object carries no diagnostic: **absence of error info is normal and is
not itself a failure**, and `kNotFound` from `GetMessage`/`GetSource` says the
same about an individual field. An `IErrorInfo` is an immutable snapshot taken
when the failure occurred, so it is callable from any thread for as long as the
reference is held.

**`IBuffer`** is the owned byte range used wherever data crosses a module
boundary: size, read pointer, writable pointer and a resize that can fail. **The
producing module owns and frees the storage**; the consumer holds a reference and
releases it. That is why no `std::string`, `std::vector`, `QByteArray` or
`QString` ever appears in a published signature — a container allocated by one
module and freed by another is a crash waiting for a different compiler, runtime
or allocator.

A buffer is **not** free-threaded. Its reference count is atomic; its bytes are
not. `Size`, `Data` and `MutableData` may be called concurrently only while no
thread is inside `Resize`, and a writer through `MutableData` must be
synchronised with every reader by the caller. A read-only buffer returning
`nullptr` from `MutableData` is a normal thing, not a failure. Pointers returned
by `Data` and `MutableData` are invalidated by `Resize` and by releasing the last
reference, and never by an unrelated call on another object.

## What `component-r1` deliberately excludes

Weak references and delegation, a replaceable memory allocator, lock-free
collections, signal/event primitives, threading utilities, IPC or marshalling,
and any Microsoft COM integration. Each is pulled in only when a real consumer
needs it, with its own tests — weak references specifically require
resolution/destruction race tests and control-block lifetime tests before they
are built. The reviewed reference ships several of these; their presence there is
not a reason to import them.

## Acceptance for the portable component base

`tests/contracts/component/` must cover, as genuinely failing-when-broken cases:

- null `out`;
- an unknown id returning `kNoInterface` **with** a nulled output;
- exactly one reference taken on success;
- an unchanged count on failure;
- query identity, reflexivity, symmetry and stability;
- `Adopt` versus `Retain` counts;
- `ComPtr` self-assignment, move, and overwrite-without-leak;
- destruction at zero;
- every `Result` code distinct;
- `IsFailure` actually returning true for every negative code — the specific
  defect the reference demonstrates.

A test that passes against a deliberately broken implementation is not coverage.
Each case is validated by temporarily inverting the behaviour it protects.

Qt appears in the *test* only. Nothing under `src/core/component/` includes or
links Qt, and the architecture suite proves that separately.

---

# The module boundary — revision `module-r1`

The binary boundary: `src/core/component/abi/` for the published ABI headers,
`src/integrations/component/` for the loader, the host shim and the codecs, and
`src/core/mihomo/module/` for the module side. The shipping artifact is a shared
library built as `clash_qt_backend_module`; a test double built from the same
sources is the fake module.

**The first binding constraint: the module is a thin supervisor over the existing
process edge, and it must never marshal proxy traffic.** What crosses is
lifecycle (a few calls per session), control (human-rate) and telemetry (bounded
by the engine's emission rate). The data plane stays where it already is, between
the engine child process and the network.

## Versions, and which is which

Three numbers travel, and confusing them is how a boundary fails quietly:

| Number | Current | Moves when |
| --- | --- | --- |
| module ABI version | 1 | the handshake structures, the two vtables, or the *meaning* of a protocol code change |
| wire revision | 3 | a code's meaning or a payload's layout changes. Appending a new code does **not** move it |
| `interfaceRevision` | 1 | `backend-r4`'s C++ vtable changes, which is a new interface id rather than a mutated one |

The connection snapshot format carries its own version, currently 2. Wire
revision 2 added the production-sequence envelope; revision 3 gave every
timestamp its time representation, which grew the packed record and moved the
snapshot version with it. Neither payload change touched a `component-r1` vtable
or the Qt-facing facade.

## The handshake

One exported C symbol, `clashqt_component_module_entry`, declared as
`ModuleEntryFn`. It is deliberately verbose and project-scoped: a module is found
by `dlopen`/`LoadLibrary` on a path the loader chose, and the symbol name is the
second half of "this file is one of ours".

**It is a C function with POD arguments because it is the only thing in the
boundary that runs before the two sides have agreed they are compatible.** A C++
vtable, an `IObject`, a `QString` or an exception at this point would already be
the undefined behaviour the handshake exists to prevent. So: `extern "C"`,
fixed-width fields, a size-prefixed struct in each direction, and `noexcept` in
the strongest sense available — the module must not let anything unwind out of
this function, whatever the C++ runtimes on the two sides are.

What is checked, and why each one:

| Field | Why |
| --- | --- |
| `structSize` | the two sides agree about the shape of the structures themselves, before any field is read |
| `abiVersion` | the version of this handshake and of the protocol |
| `targetAbiTag` | compiler, architecture, pointer width, endianness and C++ ABI flavour. A mismatch is not a version problem, it is a different binary contract |
| `runtimeTag` | the shared runtime the two sides must agree on and the ABI headers cannot see, because **no published ABI header includes a Qt header**. Host and module each compute it from their own build |
| `wireRevision` | the protocol revision |
| `interfaceRevision` | `backend-r4`'s semantic interface revision |
| `expectedModuleId` | which module this is. A loader that asked for the shipping module never accepts the test double, even though both export the same symbol |

**On failure the module writes null and returns a failure code.** It does not
construct anything and it does not try to work anyway: there is no fallback in
production.

The response struct is filled in **always** when non-null, including on refusal,
so the loader can report what the module actually was rather than
"incompatible". The **caller** sets `response->structSize` before the call and
zeroes the rest, so the module refuses a size it does not recognise instead of
writing past the end of a smaller struct allocated by an older host. Handshake
failures validate the response size before every write.

`CheckHandshake` is the single place the compatibility rules are evaluated, so the
module and any loader that pre-checks apply exactly the same rules in the same
order. The all-zero module id is named `kAnyModuleId` and is **not** a production
wildcard — `component-r1` records an all-zero id as a defect of the reviewed
reference — it is what a harness passes when it is loading a module in order to
ask what it is.

### The target tag

`kTargetAbiTag` is a constant-expression FNV-1a hash over architecture,
platform, compiler identity and major version, C++ ABI flavour, pointer width,
`long` width and endianness. It is a tag and not a version number because a
module built by a different compiler, for a different architecture, or against a
different C++ ABI does not have the same vtable layout: loading it is undefined
behaviour rather than a version mismatch, and the tag is how a host **refuses**
it instead of calling into it.

Two details are easy to get wrong and are handled explicitly. Endianness comes
from `std::endian` rather than a GCC/Clang-only predefined macro, so the header
compiles under MSVC and clang-cl. And clang-cl defines *both* `__clang__` and
`_MSC_VER` while following the Microsoft C++ ABI, not the Itanium one — so the
ABI flavour is keyed on whether `_MSC_VER` is defined at all, while the compiler
identity still reports clang. Keying the flavour on the compiler identity would
be the difference between refusing such a module and calling into it with the
wrong vtable layout, and every interface in this project uses multiple
inheritance somewhere.

The Qt build is deliberately **not** in the target tag; it travels as the
separate runtime tag. Two tags, both checked.

## The two vtables

`IBackendSession` is the module's side; `IBackendHost` is the host's. Both are
three to five methods wide and immutable, as their ids promise. No Qt type, no
standard-library container and no exception crosses either. Payloads are raw
bytes in the protocol encoding, and results the module produces are owned
`IBuffer`s.

**`IBackendHost`** — implemented by the host, called by the module:

- `Notify(event, data, size)` delivers one event. `kNotImplemented` for an event
  code the host does not know is **normal** when a newer module talks to an older
  host, and never fatal.
- `Invoke(command, data, size, reply)` is the reverse seam for privileged
  execution. `kInvalidState` means no privileged service is injected, which is
  deliberately distinct from a service that answers "unsupported": one is an
  absent collaborator, the other is a platform answer.

The module holds a strong reference to the host for as long as the session can
produce an event; `Close` releases it, and the host does not destroy its
implementation until `Close` has returned. Host and callbacks outlive pending
work.

**`IBackendSession`** — `SetHost` (exactly one host per session), `Invoke` for
one protocol command, `LastError` for the diagnostic of the most recent failure,
`Close(flags, timeoutMs)`, and `OutstandingWork()`.

`Close` refuses further commands, cancels what is cancellable, stops the managed
core when `kCloseStopManagedCore` is set, and releases the host reference once
nothing can call back. It returns `kOk` when the session is quiescent and
`kTimeout` when work is still outstanding. **An unconfirmed stop is not success**:
it is reported through the ordinary stop-completed event exactly as
`backend-r4` section 6 requires, and `Close` does not paper over it. `Close` is
idempotent, and `kAlreadyClosed` is not an error the caller has to guard against.

`OutstandingWork()` publishes pending commands, queued events and buffers the host
still holds. Zero means unloading cannot strand a callback. It is published
because **"no unload, ever" is not an acceptable substitute for accounting**.

`LastError` exists because `component-r1` forbids thread-local or process-global
error state: the diagnostic is retrieved from the failing object, and this is that
retrieval for an object whose failures are reported as `Result`s from `Invoke`
rather than through a failed `QueryInterface`. A session-decode failure publishes
its own diagnostic rather than exposing a stale one.

### Release, and what its return value means

`Release` keeps exactly the meaning `component-r1` gives it: **zero is returned
only when the object has actually been destroyed and its storage freed.**

A session whose destruction cannot happen on the calling thread — its child
process handle, socket notifiers and network access manager belong to the thread
that installed the host — or cannot happen *yet*, because a runnable the wrapped
backend submitted is still executing code inside the module image, does **not**
report zero. It retains a cleanup reference of its own and returns the resulting
**nonzero** count. The owner thread's cleanup drops that reference later, and
*that* release is the one that returns zero and destroys.

The caller's own reference is gone either way, and a nonzero answer is not an
error it has to act on — it is the truthful statement that a holder still exists.
It is also why the live-object count still counts the session and why an unmap in
that window is correctly refused.

**`Release` is not a stop.** It never waits for an engine and never reports one:
a confirmed stop is `Close`'s answer and the stop-completed event's. A session
that is still open when its last reference goes is closed first — the destructor
must not leave a managed core running behind a released object. An inert session,
and a quiescent one being released on its own owner thread, are destroyed
synchronously and do return zero.

## Re-entrancy across the boundary

`backend-r4` section 7 forbids an observer callback from inside a mutating call.
In process that guarantee is owned by the Qt event loop rather than by any code
written for it, so across a raw ABI it would have to be re-established.

It survives here because it is **not** re-implemented: the module's wrapped
backend still queues its events, and the host shim additionally defers any
`Notify` that arrives while it is inside a command. Both halves are asserted.

Both interfaces belong to the thread that created the session, which is the
host's Qt thread — so the module runs its wrapped backend on the **same** event
loop rather than inventing a second one. `AddRef`/`Release` remain callable from
any thread, because `component-r1` says so of every `IObject`.

## The command and event protocol

A versioned command/event protocol carried over the two fixed vtables, rather
than a vtable slot per operation.

**Why.** The facade has roughly sixty operations and twenty-seven events. Sixty
slots would freeze the *order* of every one of them: adding an operation in a
future revision would mean a new interface id and a migration, for a change that
is additive in the C++ contract. A command code is **data**, so an unknown code
is answered `kNotImplemented` by an older peer instead of being a call into the
wrong slot. The vtables stay three methods wide and immutable, which is what the
id promises.

**Encoding rules — all of them, because a decoder that guesses is a crash:**

- Little-endian, fixed width. No native `long`, no `size_t`, no bitfields, no
  padding assumptions: every field is written and read one at a time.
- A string is a `u32` byte length followed by that many UTF-8 bytes. Never
  NUL-terminated, never assumed valid UTF-8 by the reader.
- A list is a `u32` count followed by that many encoded elements. A decoder that
  reads past the end of its input **fails**; it does not clamp and continue.
- A timestamp is an instant **plus its representation** — see below.
- Every decode failure is reported to the caller as `kInvalidArgument` with a
  diagnostic naming the command or event. A malformed packet is a bug in the
  peer, and silence is how it survives to the next release.
- All sizes and offsets are validated before use, and malformed packets are
  reported rather than tolerated.

Every operation and event of `backend-r4` must round-trip **losslessly**,
including 64-bit ids and counters.

### The event envelope

Every payload passed to `Notify` begins with **one fixed-width `u64`**: the
sequence the module's wrapped backend assigned to the event when it **produced**
it. Everything after those eight bytes is the event's own payload — including the
packed connection snapshot, whose magic therefore sits at offset 8, not 0.

It is there because `backend-r4` admits an observer by **production**, not by
arrival (section 7). In process that costs nothing, because the producing queue
and the observer list are the same object. Across the boundary they are not — the
module produces and queues, the host receives later — so a host that stamped
events on arrival would hand a newly registered observer events produced before
it existed. The sequence travels so the host applies the *same* rule, and a
dedicated command is how the host learns the number to compare against at
registration time. A module whose backend cannot report one answers 0, and a host
that gets 0 admits everything: the pre-envelope behaviour, and an honest
degradation rather than a silent one.

### Timestamps carry their representation, not just their instant

A `QDateTime` is an instant **and** a time representation, and the host renders
the second one: the connections page prints start and end times as ISO strings,
so a value that arrives as local time where it left as UTC is a **visible text
change** even though equality — which compares instants — still succeeds.

Moving a backend behind this boundary must not change a single character the user
reads. So every timestamp on this wire carries the epoch milliseconds, a time
representation, the offset from UTC in seconds, and the IANA zone identifier when
the representation is a named zone. A decoder rebuilds the same representation or
**refuses the packet**; it never guesses at local time. The representation
numbers are this protocol's own and deliberately not Qt's enumerator values, so a
peer built against a Qt whose enumeration is renumbered still decodes what the
header describes. Mapping to and from Qt happens once, in the codec. A decoder
also validates the offset against the same bound Qt enforces, rather than handing
`QDateTime` a value it will silently discard.

### The test-control range

Codes at or above the test-control base are answered `kNotImplemented` by the
shipping module. The fake module implements them. **Do not add failure-inversion
switches to the shipping ABI** — a test-only control surface that ships is a
failure-injection switch in production — and the ABI suite asserts the shipping
module's refusal.

## The packed connection snapshot

The connection snapshot does **not** use the general encoding. It is one packed
buffer with fixed-layout records indexing a shared UTF-8 blob, because it is the
one payload large and frequent enough for per-string allocation to matter: a
snapshot can be hundreds of entries and arrives at the engine's emission rate.
The claim is backed by a case in the benchmark lane rather than asserted.

Layout, in one buffer, in this order, with no padding between sections:

```
[ ConnectionSnapshotHeader ]     fixed, 64 bytes
[ ConnectionRecord * count ]     fixed, 184 bytes each
[ TextRef * chainSlots ]         the flattened proxy chains
[ UTF-8 blob ]                   every string, no separators
```

A `TextRef` is an offset and a length **into the blob**. Nothing in the buffer is
a pointer, so the buffer is position-independent and a consumer can validate it
fully before reading one byte of text. Every offset and length is validated
against the blob's bounds before use. The header carries a magic, a version, and
`recordSize`, so a newer producer with wider records is **detected, not misread**.

Timestamps keep their representation here too, without giving up the fixed
layout: the instant, the representation and the UTC offset are fixed-width fields
of the record, and a named zone's identifier is a `TextRef` into the same interned
blob every other string uses. A snapshot whose rows share one zone therefore costs
that identifier once, not once per row, and the buffer is still exactly one owned
allocation.

The record's trailing padding is **declared** rather than left to the compiler: a
record whose tail is implicit padding travels with whatever happened to be on the
producer's stack, and a fixed layout means every byte is accounted for. The
layouts are `static_assert`ed at both sizes and alignments.

## Privileged execution

**Privileged execution stays host-owned.** The module never constructs a real
helper client. The host's injected service is marshalled through an ABI-safe
reverse interface — the listener callbacks travel host-to-module as commands, the
service queries travel module-to-host through `IBackendHost::Invoke` — and there
is **one connection only**. The module's adapter attaches and detaches itself as
*the* listener, and the host refuses a second attach.

This is the same rule `backend-r4` section 8 enforces in process, for the same
reason: two live connections to one privileged socket is a correctness hazard.

## The loader and unloading

The loader lives in `src/integrations/component/`. Every rule it applies is a
**refusal**:

- **The artifact is either an explicit path the caller chose or the
  installation-relative one.** There is no source-tree fallback, no build-directory
  fallback and no `PATH` search. A loader that searches can load a module the user
  did not install, and the failure mode of that is silent. The
  installation-relative path is the bundle's framework directory on macOS and
  beside the executable elsewhere; it returns the first candidate that exists, or
  the primary candidate when none does, so a failure names a real path rather than
  an empty string.
- **Identity is validated**, so a loader that asked for the shipping module never
  accepts the test double.
- **Target and version are validated by the handshake** before the module is
  asked for a single object.
- **The library is not unmapped** while any object it created is alive, while
  anyone but the loader still holds a reference to its root, or when the module
  answers that its image must stay. Three separate questions, all of them
  accounting rather than a blanket refusal.

The loader knows nothing about `backend-r4`. It hands back a session; a separate
host shim turns that into a `MihomoBackend`.

### The unload protocol, and why it is shaped like this

A loader that simply dropped every reference and then looked at the remainder
learned "somebody else is holding this" at the exact moment it had nothing left
to hold, so it could neither retry nor recover: the module was latched shut and
its image leaked for the life of the process. The protocol is therefore:

1. release the **root** interface while keeping the module-lifetime one, so the
   object is still pinned and the question is answerable;
2. call `PrepareUnload()`, which requires that the asking caller be the only
   holder and zero objects be alive, and which refuses **without latching**
   anything;
3. on refusal, restore the root through `QueryInterface` — legal because the
   lifetime reference never let the object die — and return a diagnostic;
4. on success, drop the last reference and unmap.

A refused unload changes **nothing**: the module is still loaded, sessions can
still be created, and the same call succeeds once the other holder releases. That
retry is the behaviour the loader promises. Release the lifetime interface last,
before closing the library handle.

`PrepareUnload`'s refusal and its answer are **one atomic transition**. A module
that checked its count and then latched the refusal separately would still hand
out an object between the two, and this interface is free-threaded. Its answers:

| Code | Meaning |
| --- | --- |
| `kOk` | nothing is alive; release the root **and** unmap the image |
| `kFalse` | nothing is alive and the root may be released, but the image must **stay mapped** |
| `kInvalidState` | objects are still alive; do **not** unmap. Not latched — a consumer may legitimately create more before getting round to unloading |
| `kAlreadyClosed` | prepared before; idempotent |

`LiveObjectCount()` counts **every** object the module produced, not just
sessions, and excludes the module root. A reply buffer or a diagnostic the
consumer is still holding has its vtable in this image exactly as a session does,
and a session-only count would read zero while those were outstanding. The root
is excluded because the loader observes it directly: the final release of the root
tells it whether anyone else still holds one.

### What actually makes an unmap unsafe, measured rather than assumed

Unmapping a module unmaps every shared library whose only reference was that
module, and those libraries are not inert. The network library, for one, registers
a lookup manager with the application object's destruction signal the first time
anything resolves a host name, and **that registration outlives the image it
points into**.

The first sample consumer segfaulted for exactly that reason — the faulting
address was a slot thunk in the network library, which the module's own unload had
just unmapped, reached from the application object's destructor — and **not**
because the module had registered meta-objects of its own.

So a module **pins the shared runtime it loaded before it constructs anything**,
and then its own image is genuinely unmappable. `kFalse` is what it answers when
that pinning failed; it is a measured answer, never a standing policy. "The module
may never be unloaded" is exactly the substitute this contract rules out.

The host creates the application object. Shared runtime dependencies stay mapped
for their process-cleanup callbacks; that does not pin the component image, and
qualification checks actual component unmapping and clean host exit.

## Qualification

**Compatibility claims are limited to measured macOS arm64 artifacts.** Code
paths for other platforms do not constitute compile or runtime evidence. The
target tag produces a value on every target it compiles for, but a **match** only
means "the two sides agree about what they were built for" — it is not a
qualification claim. The MSVC and clang-cl branches in the target header exist so
the header does not *fail to compile* there, and that is the entire claim being
made about them. Windows, Linux and untested architectures remain explicitly
unqualified. `kTargetIsQualified` is false everywhere but the measured target, so
a consumer can say so in its own diagnostics rather than inferring it.

Host and module use a matching shared Qt and a host-created application object.

## Acceptance for the module boundary

- The shared `backend-r4` assertions of section 10 run against the shipping
  module and the test module through the production loader, alongside the two
  in-process rows. The module rows load a real shared library; a missing artifact
  or a module that will not load is a failure in that lane.
- The fake module uses the **same** adapter and marshalling around the
  deterministic double, built for tests only. Existing specialist suites stay
  where they are.
- The shipping module answers every test-control command `kNotImplemented`, and
  that is asserted.
- An independent sample project under `examples/` builds **outside** the main
  CMake graph, using only the published ABI headers and the platform loader and
  Qt Core, and drives a pinned local engine. The published component headers are
  installable for exactly this purpose: a consumer can be handed them without
  being handed the Qt-facing backend or the private engine implementation.
- Unloading is exercised for real: the shipping module, driven through an engine
  and back, is unmapped, and both the refusal path and the retry after the other
  holder releases are covered.

See build.md for how the module and the sample are built, and packaging.md for
where the artifact is installed.
