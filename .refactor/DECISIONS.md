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
