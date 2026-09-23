# Testing

This describes the test strategy as it is actually practised in this tree. The
per-entry registry — every registered test, its labels and the product areas it
is evidence for — lives in `tests/README.md`; this document explains the shape
behind it, and why each lane exists.

## The standard of proof

**A test that cannot fail proves nothing.** This is the whole of the method, and
everything below is a consequence of it.

A green suite is not evidence that anything works. It is evidence that nothing
in the suite noticed a problem, which is a different and much weaker claim — and
this project has shipped real defects through a fully green run more than once.
So a check is not trusted until it has been seen to go red for the reason it
claims to guard:

- **Behaviour assertions are verified by inversion.** A high-value assertion is
  checked by deliberately breaking the behaviour in an external copy of the
  source and confirming the case fails. Those inversions are run once, recorded,
  and not re-run in the routine lane — they are evidence about the *test*, not
  about the product. A mutation that survives is not called coverage; it is
  recorded as a redundant check.
- **Every rule in the architecture checker has a failing fixture.** The
  self-test suite pairs each rule with a tiny synthetic project the rule must
  accept and another it must reject, and each case declares the exact violation
  code it expects, so a case cannot pass by failing for the wrong reason. A rule
  with no failing case is not a rule; it is a green light.
- **A skip is not a pass.** Entries that need an unavailable resource carry a
  label the routine lane excludes, so they are honestly *not run* rather than
  reported "Passed" while silently skipping. Two graphics entries once did
  exactly that from inside a larger suite; they are separate, labelled entries
  now for that reason.
- **Guards are checked for being alive.** A guard that quietly does nothing is
  worse than no guard. The offscreen-platform block in the test registration
  used to sit above the suite registrations, where `if(TEST ...)` was simply
  false for anything not yet defined, so seven widget suites ran against the
  real display and would have failed on a headless runner. It is applied at the
  end now, and the same failure recurred once by scope instead of order — a
  subdirectory's tests are invisible to the parent's `if(TEST ...)`, so naming
  them there set nothing and said nothing.

## The lanes

Lanes are selected by CTest label through the `make` facade. Use a build
directory you own:

```sh
make test             BUILD_DIR=build/local   # everything but the four gated labels
make test-integration BUILD_DIR=build/local   # real-core and integration
make test-native      BUILD_DIR=build/local   # opt-in; needs a disposable host
make package          BUILD_DIR=build/local
```

`make test` excludes `native|privileged|benchmark|real-core`.
`make test-integration` selects `real-core|integration` and builds the pinned
local engine first. Counts change as suites are added; measure them rather than
quoting them, with `ctest --test-dir <dir> -N` and `--label-regex`.

Every entry gets its own data directory. That is not a convenience: preference
access falls back to the developer's real native store when the data-directory
variable is unset, isolation used to be opt-in per suite, and a new suite that
forgot it wrote into the real user store. Forgetting is no longer sufficient to
reach it.

### unit

Deterministic, no real I/O beyond a temporary directory. Value types,
composition, codecs, the coordinators driven through their ports, and the
component object model's own contract.

**Proves what nothing else can:** that a rule holds for inputs a running system
would not produce. The composer's refusal of excessive nesting, a decode that
must fail on a malformed packet, a status code that must be distinguishable from
another — these are reachable only by constructing the case directly.

**Cannot prove:** that anything constructs the object in production.

### contract

The published backend contract as an acceptance set, run against more than one
implementation: the deterministic in-process fake, the real engine adapter, and
both of those again behind the loaded module. The component object model and the
binary ABI have their own contract suites.

**Proves what nothing else can:** that the contract is *implementable* and that
its consumers are correct against it (the fake), and separately that the engine
*honours* it (the real adapter) — and that both answers survive a module
boundary. Running one assertion set over four fixtures is what makes "the module
behaves like the in-process object" a measured claim rather than a hope.

**Cannot prove:** anything about mihomo. A suite driving the compiled fixture
core is evidence about the adapter, not about the engine.

### ui

Widget suites. They construct the real pages from the real source files against
the Qt bridge and the fake backend, running offscreen, with the software Quick
backend where a view is embedded.

**Proves what nothing else can:** that a surface renders the state it is given
and issues the request it claims to, including the states that are hard to reach
from a journey — an empty list, an error, a control that must be disabled and a
notice that must explain why.

**Cannot prove:** that the page is reachable, or that anything supplies it the
state it renders.

### workflow

Complete journeys: first launch, subscription update, restore, recovery, routing
controls, and an executable smoke test. They drive the assembled coordinators
over the real profile store, the real filesystem, real loopback networking, the
compiled fake core and the dynamically loaded module. They run serially because
they share a fixed controller port that the profile store forces into every
generated configuration. The smoke entry launches the actual shipped binary with
its own data directory, helper socket and controller fixtures.

**Proves what nothing else can:** that the pieces are *wired together*. This is
the only lane that can see a component which builds, links, passes its own
suite, and that nothing ever constructs. It is also the only lane that starts
from an empty data directory, which is how a cold-start deadlock becomes
visible.

**Cannot prove:** anything about a path the journey does not walk. A journey is
one route through the system, and it is expensive; there will never be many.

### architecture

Five entries, all cheap, all structural.

- **The graph check** reads the *evaluated* CMake target graph through the File
  API — not by parsing `CMakeLists.txt` — and checks it against the declared
  module graph in `tests/architecture/architecture.json`. See the next section.
- **The public-header probe** compiles every published header of every module,
  one header per translation unit, included first and then included again, into
  a consumer that links *only* that module. It deliberately does not add the
  project's include root: the header must be reachable through the module's own
  interface include directories, or a real client could not use it either.
- **The checker self-test** runs the synthetic accept/reject fixtures described
  above.
- **The reference check** requires that every documentation path named anywhere
  in the tracked tree resolves, that nothing names a path the merge to the
  published branch deletes, and that no comment identifies anything by its
  position in a plan rather than by what it is. It is driven by `git ls-files`
  rather than by globs, because glob-driven scans of this repository have
  reported a clean tree while a real violation sat in a subdirectory build file.
- **Its self-test** does for those three rules what the checker self-test does
  for the graph rules.

**Proves what nothing else can:** that the shape of the system is what it is
declared to be — including the parts of that shape no runtime test can observe,
such as a dependency that arrives transitively through an umbrella target, an
`add_dependencies()` edge, a static-library cycle CMake configures without
complaint, or a header that only compiles because something else was included
first.

**Cannot prove:** anything about behaviour.

### real-core

Two entries, label-gated so an absent engine cannot make the routine lane fail
and cannot be mistaken for a pass. They drive the locally built mihomo and
require the build's own artifact with matching provenance and hash; a missing
artifact fails rather than skips.

**Proves what nothing else can:** that the engine this project builds from the
recorded source commit actually starts, accepts a configuration, serves its
controller and stops. Composition acceptance is not engine acceptance.

**Cannot prove:** that a user's installed engine behaves the same.

### packaging

The smoke entry additionally carries a packaging label: it exercises the shipped
binary, module selection, isolated helper and controller routing, single
instance, startup wiring and data-directory precedence.

**Proves what nothing else can:** that what is *staged* is what runs. A module
that is built but not staged, or staged but not found, fails only here.

**Cannot prove:** that a signed, notarized, installed release behaves the same.
No such claim is made.

### benchmark, native, privileged

Three benchmark entries are excluded from routine qualification: two need a real
display and their own measurement flag, and the third measures the connection
codec (its correctness slots run in the normal lane from the same binary).
Record build mode, workload, time and payload size; the packed-versus-naive
ratio differs by build mode and machine.

The `native` and `privileged` labels are **reserved and currently carry zero
registered entries.** They exist so that a lane cannot pass vacuously later.
Helper fixture tests carry a service label and stay in routine tests; they run
unprivileged. Tests never install, remove or restart the privileged helper, and
never change operating-system proxy, VPN or trust state.

## The architecture checker

The checker fails on anything the declared graph does not allow. Every rule that
can fail has its own violation code, so a self-test can assert that *that* rule
fired rather than that something went wrong. A code no case can provoke is a
rule that has quietly stopped checking anything.

What each declaration means:

| Field | Meaning |
| --- | --- |
| `deps` | The allowed edges to other project targets, exactly |
| `external` | The allowed **direct** external dependencies, exactly |
| `transitive_external` | A closed allowlist over the evaluated transitive closure — this is what catches a dependency acquired through a broad umbrella target |
| `forbidden_external` | Desktop Qt. Never allowed in a backend module, and never in the closure of a target marked headless |
| `public_headers` | Compiled standalone by the probe |
| `include_rules` | The textual include scan: no upward includes, no desktop Qt, platform-not-core, core-not-platform, component-private |

Two rules do not ask "is this edge allowed" at all. They ask whether the
*ledger* still describes reality, because a baseline that drifts away from the
graph stops being a ratchet while still printing a reassuring site count. One
fails any baselined site naming a target the project declares nowhere — a
renamed or deleted target leaves a dead site that is indistinguishable from
discharged debt. The other is the seal, below.

### The exception ratchet

A baselined exception needs five fields, all required: an id, a kind, an owner,
a removal phase and a reason. The point of requiring an owner and a date is that
migration debt always carries one.

- An exception that **matches nothing** is reported stale and fails the check. A
  baseline allowed to rot stops being a ratchet. The stale rule deliberately
  excuses a site whose target this configuration does not build — a widget suite
  in a headless build is absent but not dead — and that excuse is paid for by
  the dead-site rule above, which uses the *declaration* rather than the build
  as its discriminator.
- Exceptions are grouped by class in the summary. A migration baseline is debt
  that must shrink to zero and carries the phase that clears it. An
  architecture-class exception with removal `never` is a permanent allowance,
  for an edge that is correct rather than owed — kept out of the group whose
  count is supposed to fall, so it cannot inflate it.
- **The migration baseline is currently empty.** One permanent allowance
  remains, with six sites. Both migration exceptions that once let application
  code reach the component-private engine are gone: the application and the
  journey suites load the shared module through the host facade instead.

### The seal

The graph rules consult `deps` *before* they consult exceptions. That means a
target which declares a component-private module in its `deps` is permanently
allowed — no owner, no removal phase, invisible to the ratchet. One line of JSON
converts tracked debt into architecture. Eight sites once became architecture
exactly that way.

The seal closes that route, in both directions. While the named exceptions are
open, no specification may name the sealed module in its `deps`; and every
target that *actually links* the module in the evaluated graph must appear as a
baselined site. The ledger is therefore derived from the graph rather than from
memory, and both clauses lift automatically when the last named exception is
deleted.

The seal is matched to its exceptions by an id glob, which makes the id a
mechanism rather than a label: the exception's id must keep its agreed suffix,
or the seal stops applying and nothing fails to say so. Both the glob and the id
carry that warning in their own text. The one exempt linker pattern is the
public-header probe, which compiles published headers — the opposite of reaching
inside.

The six baselined sites are the suites whose *subject* is the private
implementation: the controller and provider client suites, the core-process
suite, the engine-discovery suite, and the two suites that run the backend
acceptance set against the real adapter. A test whose subject is a
component-private implementation must link that implementation. Nothing planned
removes these links, so `never` is the honest value, and the entry is classed as
an allowance rather than as debt. Each site names one target, so a seventh
implementation-subject linker still fails and has to be argued for.

## The recurring defect class

This project has a documented, recurring defect class with **six** recorded
instances — more than any other single cause — and every one of them was
invisible to a green suite.

**The shape.** Two things were wired to each other. One end was replaced, moved
or deleted; the other end was left exactly where it was. The survivor still
compiles, still links, still passes the tests written for it, and is connected
to nothing. Nothing in the toolchain objects: an unused private member is legal,
an emitted signal with no receiver is legal, a link edge no source needs is
legal, a comment naming a deleted file compiles fine, and a `make` target that
builds the wrong thing exits 0.

**It runs in both directions.** The producer can go and leave the consumer, or
the consumer can go and leave the producer. Looking for only one direction finds
half of them.

**Why this project keeps producing it.** A refactor of this shape — sever a
coupling, publish a contract, move the wiring to a composition root — *is* a
machine for producing it. Every discharged edge is one half of a pair removed on
purpose, and the other half is by definition in a file the change that removed
the first half did not touch.

### The six, and what detects each

| Instance | Which half went | Detected by | State |
| --- | --- | --- | --- |
| The privileged-service injection. The engine's concrete platform client was replaced by a null service and the composition root was never taught to inject the real one, so service mode failed at every call and the user's saved preference was discarded silently | producer | nothing, at the time — found by reading. Now pinned by the smoke entry and by core-process cases | fixed |
| The cold-start system-proxy deadlock. The only thing that ever set a proxy target was a checkbox that the controller disabled until a target existed | producer | the workflow lane, because a journey starts from an empty data directory | fixed; kept as a named regression |
| The service page's "a core is running under the helper" flag: three readers, no writer, after its only writer was removed as a correctness fix. The guard refusing to uninstall the helper while in use was permanently inert | producer | nothing, at the time — found by reading the page | fixed; pinned by named cases in the service-settings suite |
| The settings page's two system-proxy notification signals, emitted with no receiver anywhere | consumer | nothing | **still open** — see below |
| The component object model linked by nothing that shipped: its only consumers were its own contract suite and the header probe | consumer (the loader that would use it had not been written) | the evaluated link graph, read by hand | discharged — the application now reaches it through the module host |
| The `make module` target did not build the module. It built the static backend libraries and the engine, and never touched the component it was named for | consumer | running the documented command and checking what it did | fixed; it now builds the shared artifact and the engine it needs |

### Which lanes genuinely detect it

**Lanes that do detect it:**

- **workflow** — the strongest detector, and the only one that finds the
  *wiring* form. The smoke entry drives the shipped binary, so a component that
  nothing constructs shows up as a behaviour that never happens; the journeys
  start from an empty data directory, so a producer that only ever ran after
  some other state existed is exposed. Two of the six are now pinned here.
- **architecture** — detects the *build-graph* form, mechanically and cheaply: a
  baselined site naming a deleted target, an exception that matches nothing, a
  library nobody declared, a target that links a sealed module without a site.
  It does **not** detect a library nothing links; that shape is legal and was
  found by reading the dump.
- **the reference check** — detects the *documentation* form, which is why it
  exists. A comment citing a document compiles exactly as well when the document
  is gone, and nothing else in the tree looks at a path that merely appears in
  prose.
- **ui**, but only retroactively. A widget suite catches this class only when
  someone writes a case naming the specific flag. The service-settings suite
  exists for precisely that reason and its cases fail if the flag goes inert
  again — but it was written *after* the defect was found by reading.

**Lanes that do not detect it, and will not:**

- **unit** and **contract**. Structurally incapable of it. They construct the
  object under test and supply it with exactly what production forgot to supply;
  a missing injection at the composition root is invisible by design.
- **real-core**. It proves the engine honours the contract. It says nothing
  about whether anything in the application is wired to that contract.
- **packaging**. It proves the artifact is staged. A staged artifact nothing
  loads still stages.
- **benchmark**. Not a correctness lane at all.

**What nothing detects.** An emitted signal with no receiver, and a member read
and never written, have no automated detector in this tree. The one open
instance above is of exactly that shape: two signals are emitted at
`src/ui/pages/settings/settings_page.cpp:277-278`, declared at
`src/ui/pages/settings/settings_page.h:47` and `:51`, and there is no `connect()`
to either anywhere in `src/` or `tests/`. A green run says nothing about it.
Until a rule is written, the next instance of this class will be found by
reading.

**The sweeps that have paid**, in the order they have paid:

1. Members read and never written, and fields written and never read. A
   compiler will not say so for a member; counting assignments by name will.
2. Signals with no `connect()`. Do it for every signal a migration touched.
3. Targets nothing links. Read the *evaluated* graph, not the build files: a
   library whose only consumers are its own test and the header probe is the
   signature.
4. Build and run targets whose name is a promise. Run the documented command and
   check that it did what the name says.
5. Whenever a coupling is severed, grep the far end in the same sitting. The
   change that removes a producer does not own the consumer's file, which is
   exactly why the consumer survives.

## Known limits

These are stated because a limit that is not written down gets mistaken for
coverage.

- **The include scan is a supplement, not a proof.** It sees textual `#include`
  lines only — not template instantiation, not linker-visible symbol use, not
  runtime coupling through signals, meta-object lookup, dynamic properties or
  the application property bag. The target rules read the evaluated graph, which
  does cover transitive and generated edges. The checker prints this note on
  every run.
- **One include rule has no failing fixture.** Four of the five include rules
  are exercised by the reject fixture, one source file each. The rule that keeps
  `src/core/**` from including `platform/**` — the rule that made the privileged
  seam necessary — is not among them, and the reject fixture's own graph does
  not even declare it. By this suite's own standard, that rule is currently
  unverified: it could have stopped firing and nothing would report it.
- **A test executable that is not declared as a consumer is exempt from the
  graph rules entirely.** Undeclared *libraries* fail the check; undeclared
  *executables* are skipped, because the consumer allowlist is what covers them.
  Nine registered test executables currently link project modules without
  appearing in that allowlist, so their edges and their headless status are
  unchecked. The seal still catches the worst case — it derives its linker list
  from the evaluated graph, not from the allowlist — but nothing else does.
- **Qualification is macOS arm64.** Windows and Linux have zero compile or run
  evidence. Conditional code and registered tests do not qualify a platform, and
  some older process fixtures remain POSIX-specific.
- **Port-unavailable skips are unavailable evidence, not passes.** The journeys
  share a fixed controller port; do not run separate journey processes against
  it concurrently.
- **No signed-release, notarization, native-GPU or installed-privileged-service
  claim is made here.** Fresh-clone build and package runs plus independent
  read-only audits are what close the integration gate, not a green unit count.

## Related documents

- [architecture.md](architecture.md) — the layering the checker enforces.
- [module-api.md](module-api.md) — the contracts the contract lane tests.
- [build.md](build.md) — build directories, presets and the engine.
- [packaging.md](packaging.md) — what the packaging entry exercises.
- [development.md](development.md) — working conventions.
