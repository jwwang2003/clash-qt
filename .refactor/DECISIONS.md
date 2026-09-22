# Coordinator decisions

Numbered, dated, with the evidence that forced them. A worker that disagrees files
an escalation; it does not re-decide. Process notes only — see the binding cutover
rule in [PROGRESS_LEDGER.md](PROGRESS_LEDGER.md): none of `.refactor/` reaches `main`.

---

## D1 — Homes for `core/types.h` and `core/yaml_util.*` (resolves PRE-ARCH escalation E1)

**Problem.** The six planned static libraries have no home for either file.
`core/types.h` is a *public* dependency of `clash_mihomo_impl` (which G2 makes
component-private) and is also included by six `src/ui/**` headers. Placing it in
`clash_mihomo_impl` would make the UI include a component-private header, violating
G2; placing it in `clash_config` would give the component a PUBLIC edge onto
`clash_config`. `yaml_util.h` is shared by three of the six targets.

**Decision.**

*Phase 1 (now):* `src/core/types.h` **stays put** — the plan defers its split to
phase 3, and all 9 include lines referring to it stay byte-identical.
`src/core/yaml_util.*` moves to `src/core/config/` exactly as the plan's table says.

*Phase 2 partition:* adopt PRE-ARCH's recommendation with one refinement — **two**
extra targets beyond the planned six:

| Target | Kind | Contents | Why |
| --- | --- | --- | --- |
| `clash_types` | INTERFACE, header-only | `core/types.h`, later `core/types/*.h` | Depended on by the UI, `clash_config`, `clash_mihomo_impl` and the published component contract alike, without any of them depending on each other. |
| `clash_yaml` | STATIC, yaml-cpp only | `core/config/yaml_util.*` | Keeps `clash_mihomo_impl` off `clash_config` (PRE-ARCH edge #5). The component ships separately, so it must not drag the configuration library into its package. |

**Directory and target deliberately do not coincide:** `yaml_util` lives under
`src/core/config/` but belongs to target `clash_yaml`. The source plan explicitly
permits this — "A folder is not automatically a library or plugin."

*Phase 3:* `core/types.h` splits into six focused headers under `src/core/types/`.
MOD-CORE signs off that boundary **before** the split happens, because the field
layout of those values is ABI-relevant once they cross the component boundary.

Free win recorded for CFG-CORE: `core/profile/profile_store.h` includes
`core/types.h` and uses nothing from it. Deleting that include drops `clash_profiles`
off the shared types header entirely.

---

## D2 — `clash_mihomo_impl` → `clash_platform` is injected, not absorbed (resolves E2)

**Problem.** `platform::PrivilegedServiceClient` appears in `CoreProcess`'s
constructor signature (`core/process/core_process.h:31`, forward-declared at `:17`),
so a platform type is part of the component's *public* surface. Two ways out, and
both cross package boundaries.

**Decision: inject through an abstract interface owned by `clash_mihomo_impl`.**
The privileged-service client is **not** absorbed into the component package.

**Why this way.** G2 states that privileged execution remains a separate service,
and the COM plan requires privileged operations to stay in narrow OS helpers.
Absorbing the client would make the shipped component responsible for privileged
IPC and would change what `clash_platform` contains, cascading into both MOVE-CORE's
and MOD-CORE's file ownership. Injection keeps the privileged boundary narrow and
OS-owned, and removes the platform type from the component's published surface.
The composition root adapts the concrete `platform::PrivilegedServiceClient` to the
interface.

**Timing: nothing changes in P1 or P2.** The edge is recorded as a *named baselined
architecture exception with an owning removal phase of P3 / MOD-CORE*, which is the
migration rule the test strategy prescribes — baseline existing exceptions with an
owner, and refuse new ones casually. The architecture checker must therefore ship
with this exception pre-registered and fail if any *additional* one appears.
MOD-CORE owns the constructor change when it takes its lease.

---

## D3 — The duplicate privileged-service connection is a contract question, not a move

`ui/service_settings.cpp:21` constructs a **second** `PrivilegedServiceClient`
(`probe_`, driven by a retry timer at `:24`) alongside the one `CoreProcess` already
owns at `core/process/core_process.cpp:91`. Two live connections to one privileged
socket is a correctness hazard in its own right, not merely a layering complaint.

**Decision.** It becomes a capability/status query on the backend contract, owned by
MOD-CORE and integrated by the coordinator. **Not touched during P1** — it is a
behaviour change, and P1 is mechanical relocation only. MOVE-UI relocates the file
without altering its constructor.

**Discharged in `88ef6cd`.** `src/ui/pages/settings/service_settings.cpp`
constructs no `PrivilegedServiceClient`; status arrives on
`core::backend::BackendBridge::privilegedServiceStatus`. One connection to the
privileged socket, the one `CoreProcess` owns. Verified 2026-09-22: `grep -rn
PrivilegedServiceClient src/` finds no construction under `src/ui/**`, only two
comments in `service_settings.h` recording that there used to be one. The ledger
went on listing this as open for two commits afterwards; that is corrected.

**What discharging it cost, and what that cost teaches.** The second client was
the only producer of `ServiceSettings::serviceRunning_` — "a core is already
running under the privileged service, possibly for another app session". Removing
it left three readers and no writer, so the guard that should refuse to install,
repair or remove the helper while a core runs under it was inert: uninstallable
state, silently permitted. The decision's own wording anticipated this — the flag
becomes "a capability/status query on the backend contract" — but D3 was recorded
as closed on the removal alone, and the contract did not yet carry the flag. **A
decision that moves a value from one producer to another is not discharged when
the old producer goes; it is discharged when the new one answers.** The repair
adds `coreRunning` to `core::backend::PrivilegedServiceStatus` rather than
restoring a second connection, which is what D3 asked for in the first place. See
"Recorded defect class" in [PROGRESS_LEDGER.md](PROGRESS_LEDGER.md).

---

## D4 — What the architecture checker must and must not flag

PRE-ARCH found **no** `core/**` or `platform/**` file that includes `src/ui/**`, and
none using Widgets, Quick or Graphs. The expected violation does not exist. Two
legitimate patterns would produce false positives if the rule is written carelessly:

| Pattern | Evidence | Rule |
| --- | --- | --- |
| Platform uses Qt **Gui** | `platform/system/hotkeys.h:3` puts `QKeySequence` in a public header, so `clash_platform` links `Qt6::Gui` PUBLIC | Allowed. The source plan permits platform adapters to use Qt Gui where native hotkey/browser behaviour requires it. Forbid Widgets/Quick/Graphs only. |
| Core uses Qt **Qml** | `core/enhance/config_enhancer.cpp` uses QJSEngine to run profile enhancement scripts | Allowed. The rule must distinguish **Qml** from **Quick**; banning "anything QML-ish" breaks a real feature. |

The violation that **does** exist runs the other way and is the concrete G2 blocker:
**12 UI translation units include `core/mihomo_client.h`**, and `ui/main_window.h`
names `core::CoreState` in a slot signature. That is what the checker must catch —
UI reaching into component-private implementation headers — and what MOD-CORE's
published contract has to displace.

---

## D5 — GNU Make 3.81 is the supported floor

Recorded in the ledger with its probe evidence. Both 3.81 and 4.4.1 are installed
locally, so the facade is verified under each before it ships. Recipes stay single
`&&`-chained commands: `.ONESHELL` is *silently* ignored by 3.81, and the Windows
requirement to avoid `/bin/sh` forces the same shape anyway.

---

## D6 — The portable component is implemented independently, not copied

`3rdparty/ref/fxcom` carries no licence file, so no implementation is taken from it.
`component-r1` is an independent design of the reviewed model, with owned interface
ids, a signed `Result` and distinct error codes. See
[COMPONENT_CONTRACT.md](COMPONENT_CONTRACT.md) for the three reference defects that
are deliberately not reproduced.


---

## D7 — Architecture checker baselines, ratified

BASE-ARCH shipped **four** baselined entries rather than the one D2 named, and asked
for ratification. **Ratified**, with conditions.

**What is baselined.** D2's `clash_mihomo_impl → clash_platform` edge plus its single
include site, and a separate `migration-baseline` class covering the G2 coupling that
already exists: **8 link sites and 20 include sites** of the component-private mihomo
headers by UI and application code. Owner **MOD-CORE**, removal phase **P3**.

**Why baselining is right here.** Without it the checker is red on day one, and a
permanently red check is ignored exactly as fast as a permanently green one. The test
strategy prescribes this: baseline named existing exceptions with an owning removal
phase, and refuse new ones casually. The ratchet is what makes it honest — any 9th
linker or 21st include fails.

**Condition: the ratchet must actually bite, and it does.** Verified independently at
integration, not taken on the worker's word: injecting one extra UI include of
`core/mihomo/mihomo_client.h` fails `arch-graph` with
`ARCH-R5-INCLUDE-IR-COMPONENT-PRIVATE src/ui/widgets/toggle_switch.cpp:1`, and the
check passes again once reverted. The baselines are printed under their own heading
so they cannot be mistaken for a clean result.

**Accepted limitation.** Test-strategy rule 1 says configuration targets must not
acquire an HTTP dependency. That is **not achievable** for `clash_config`: `Qt6::Qml`
re-exports `Qt6::Network`, and `Qt6::Qml` is required because `QJSEngine` runs the
profile enhancement scripts. Declared with a note rather than enforced. Recording it
as a known gap is correct; silently dropping the rule would not be.

**D2's cost is wider than the edge itself.** Through `clash_platform`,
`clash_mihomo_impl` transitively sees `Qt6::Gui`, Carbon, CoreServices and Security.
These are listed, not exempted, so Widgets/Quick/Graphs still fail there. The listing
clears when D2 clears.

**Consequence for COMPONENT-BASE.** `src/core/component/` will trip
`ARCH-R4-UNDECLARED-TARGET` the moment it becomes a library. That is the ratchet
working; the coordinator adds it to `modules` with its allowed dependencies at
registration, rather than widening the rule.

**New declared dependency.** The checker is Python. Python is therefore a *test-only*
prerequisite, reported by `make doctor` as a note rather than a hard failure: a clean
Windows machine has no interpreter, and the application itself needs none.

**What the checker provably does not cover**, recorded so it is not mistaken for full
coverage: the include scan is textual, so no template instantiation or symbol-level
use; runtime coupling through Qt signals, meta-object lookup and the dynamic-property
bus is invisible; a header-only dependency with no link edge is invisible to the edge
rule; and only one configuration is checked, so Windows and Linux platform
dependencies are unverified. Test-strategy item 5, replaceable implementations, is not
implemented — it needs MOD-CORE's contract to exist first.

---

## D8 — COMPONENT-ABI adapts; the Qt contract stays host-side

**The conflict.** G2 requires the engine to be a separately built, separately
packaged shared library, and says a static-library-only test is insufficient. But
`clash_backend` passes Qt types across what must become that boundary
(`control.h:29`, `virtual RequestId setMode(const QString &mode)`), while
`clashqt_com` deliberately links nothing — not even `Qt6::Core` — because a Qt
type in a published signature defeats the point. `QString` cannot cross a module
boundary: its layout and implicit sharing depend on the exact Qt build, and its
memory would be allocated in one module and freed in another. The project also
builds **no** `SHARED` or `MODULE` library today.

So "publish a factory from `clash_backend`" described two incompatible designs.

**Decision: option A.** The module exposes ABI-safe COM interfaces. A thin
host-side shim holds COM pointers, marshals each call, and implements
`MihomoBackend`. The UI and the Qt bridge are unchanged.

**Why, on the evidence.**

*Performance is not a discriminator.* The engine is already a separate child
process reached over a loopback controller, so the **data plane never crosses the
boundary**. What crosses is lifecycle (a few calls per session), control
(human-rate), and telemetry (bounded by mihomo's emission rate). And the bridge
**already** materialises every `Span` into an owned Qt container for queued
delivery — so most of the marshalling cost is already being paid. The real costs
are JSON parsing and Qt signal delivery, unchanged either way.

*Robustness favours A.* `MihomoBackend` already has two implementations, real and
fake; a module-backed third joins them and **the same contract suite runs against
all three**, which is G2's "fake and production modules satisfy common contracts"
for free. A boundary bug is contained in one shim rather than spread across 66
call sites. And non-re-entrant observer delivery — the rule that cost amendment
A2 and produced a live defect — is currently guaranteed by the Qt event loop;
across a raw ABI it would have to be re-established.

**Two binding constraints.**

1. **The module stays a thin supervisor over the existing process edge.** It must
   never marshal proxy traffic. Object pointers never cross into Go.
2. **Telemetry marshalling is measured, not assumed.** A connections snapshot can
   be hundreds of entries; naive per-string allocation is the one plausible
   regression. One buffer per snapshot with fixed-layout structs indexing into
   it, and a case in the `benchmark` lane before the claim is made.

**Stated plainly, because "component" implies more than it delivers:** a loaded
module shares the process. Neither this design nor its alternative gives crash
isolation — if the module faults, the application dies. The boundary buys
replaceability and versioning. The real isolation in this system is that mihomo
is already a separate process, and that is not changing.

**Open for P4's first worker to answer, not to assume:** whether the fake backend
also becomes module-backed. If it does not, the module path has exactly one
implementation and the common-contract claim is weaker than it sounds.
