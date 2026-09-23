# Working in the clash-qt tree

What lives where, what each library is allowed to depend on, how that is
enforced, and what a change has to satisfy before it lands.

Build commands are in [build](build.md). The shape of the system and the
reasoning behind it are in [architecture](architecture.md); the published
engine boundary is in [module-api](module-api.md); what the test suite is
evidence for is in [testing](testing.md).

## Layout

```
src/
  core/          backend libraries: no desktop Qt, no UI, no OS adapters
    backend/       the published MihomoBackend contract (header-only)
    component/     the portable object model and the binary module ABI
    config/        YAML composition and profile enhancement
    mihomo/        the engine implementation and its loadable module
    profiles/  backups/  preferences/  telemetry/
  platform/      OS adapters: system proxy, privileged service, hotkeys, browser
  integrations/  host-side module loader and the marshalling shims
  app/           composition-root services lifted out of main.cpp
    lifecycle/  backup/  runtime/  composition/
  ui/            Qt Widgets shell, pages, widgets, theme
  services/macos/ the standalone privileged helper
  main.cpp       the composition root
tests/           suites, fixtures, journeys, architecture checks
cmake/           build modules included by the root CMakeLists.txt
scripts/build/   the doctor project and the engine build script
examples/        a standalone consumer of the public component headers
```

## The libraries and what each owns

Each target owns one responsibility and declares only the dependencies its code
actually has. A target that links what it does not use makes the dependency
graph lie, and the graph is what the architecture check reads.

| Target | Owns |
| --- | --- |
| `clash_types` | Shared value types, header-only. Everything may depend on it; it depends on nothing but Qt Core. |
| `clash_backend` | The published `MihomoBackend` contract. Header-only. The boundary the application codes against; both the real backend and the deterministic fake implement it. |
| `clash_backend_bridge` | The Qt-native view of that contract — signals for the ~66 UI call sites. Deliberately not itself a `BackendObserver`: 23 callbacks share a name with their signal, which would make the member-pointer form ambiguous everywhere. |
| `clashqt_com` | The portable component object model. Links **nothing**, not even Qt Core: it is the foundation every component boundary is expressed in, and a Qt type in a published signature would defeat that. |
| `clash_component_abi` | The binary boundary. No Qt or STL value crosses it — a `QString`'s layout and implicit sharing depend on the exact Qt build, and its memory would be allocated in one module and freed in another. |
| `clash_component_marshal` | The private codecs, compiled into both sides. Symbols stay local; each buffer is destroyed through its producing side's vtable. |
| `clash_qt_backend_module` | The shipped engine supervisor, built as a shared module so what ships is exercised as a module rather than only as a static library. |
| `clash_module_host` | The host-side facade and explicit-path loader. Marshalling lives in one shim rather than spreading across call sites. |
| `clash_mihomo_impl` | The engine implementation. **Component-private** — see the seal below. |
| `clash_yaml` | YAML helpers as their own leaf, so the packaged component never has to drag the configuration library in. |
| `clash_config_compose` | Pure YAML composition: immutable input, no disk, network, process or script engine. |
| `clash_config` | Profile enhancement scripts, run through `QJSEngine`. |
| `clash_profiles` | The profile store. |
| `clash_backups` | The backup store. |
| `clash_preferences` | The one accessor for the preference store. A leaf on Qt Core alone, so the UI, the backup store and the composition root can each read a setting without depending on one another. |
| `clash_telemetry` | Traffic history. The narrowest module in the tree, and the sharpest probe for dependency creep. |
| `clash_platform` | OS adapters. Qt Gui is allowed (`hotkeys.h` publishes `QKeySequence`); Widgets, Quick and Graphs are not. |
| `clash_app_lifecycle` | Startup and shutdown coordination. Links neither platform nor profiles nor Widgets — everything reaches it through the ports in `shutdown_ports.h`, which is what makes the quit gate testable by holding and releasing each condition. |
| `clash_app_backup` | Owns the process's single backup store, so shutdown no longer discovers it by walking the widget tree. |
| `clash_app_runtime` | Reload scheduling, snapshot retention and routing intent. |
| `clash_app_composition` | The composition-root adapters. The only layer permitted to see both sides of the privileged seam. |

The UI is compiled into the `clash-qt` executable for now, through the source
manifests under `src/ui/`. Extracting it into its own target waits until its
test substitution points are explicit.

## The rules a change must not break

Five rules are enforced mechanically. They are declared in
`tests/architecture/architecture.json`, which is the architecture: the checker
reads the *evaluated* CMake target graph and fails on anything not declared
there.

- **No upward include.** A file under `src/core/` or `src/platform/` must not
  include UI or application code. Invert the dependency instead: publish a
  signal or an interface the UI consumes.
- **No desktop Qt in the backend.** `Qt6::Widgets`, `Qt6::Quick`,
  `Qt6::Graphs`, `Qt6::Charts` and friends are forbidden in `src/core/` and
  `src/platform/`. `Qt6::Gui` and `Qt6::Qml` are deliberately **not** forbidden:
  Gui is not Widgets, and Qml is not Quick.
- **Platform does not depend on core.** `src/platform/` may include
  `core/types.h` and nothing else from core. Shared value types belong in
  `clash_types`.
- **Core does not depend on platform.** Core reaches the OS through an
  interface it owns; the composition root adapts the concrete platform type.
- **The engine implementation is component-private.** `src/ui/`, `src/app/` and
  `src/main.cpp` must not include `core/mihomo/**`. The UI uses the published
  contract and its Qt bridge, which exist to displace exactly those includes.

Some targets additionally carry `headless: true`, meaning no desktop Qt module
may appear anywhere in their evaluated closure. That is the enforceable half of
"a backend test must run without a desktop": a checker cannot prove the code
never needs a display, but it can prove no desktop Qt is linked in.

## The architecture check

Three CTest entries, all labelled `architecture`, run in the routine lane:

| Entry | What it does |
| --- | --- |
| `arch-graph` | `tests/architecture/arch_check.py` compares the evaluated CMake graph against `tests/architecture/architecture.json`. |
| `arch-public-headers` | Compiles every published header standalone (see below). |
| `arch-selftest` | Runs synthetic cases proving each rule *fails* when violated. |

`arch_check.py` reads the graph through the CMake File API (codemodel v2)
rather than by parsing `CMakeLists.txt` text. That matters: the File API gives
the link interface with generator expressions already resolved, plus
`add_dependencies()` and generated edges, plus the include path each source was
actually compiled with. An edge added from any `.cmake` file, any function or
any generator expression is visible, and a dependency acquired indirectly
through a broad umbrella target shows up in the compile groups even though it
appears in nobody's `linkLibraries`.

Run it by hand against any configured build tree:

```sh
python3 tests/architecture/arch_check.py --build-dir build/dev --repo-root .
```

```
architecture check: architecture.json
  checked: module edge allowlist
  checked: acyclicity
  checked: external dependency allowlist
  checked: exception ratchet
  checked: exception-site liveness
  checked: sealed-module seals
  checked: project-include scan
  baselined architecture exceptions (1):
    impl-subject-suites-link-component-private  6 site(s)  owner=coordinator removal=never

PASSED
```

Every rule that can fail has its own published violation code
(`FORBIDDEN-EDGE`, `UNDECLARED-EXTERNAL`, `TRANSITIVE-EXTERNAL-CREEP`,
`SEALED-MODULE-UNBASELINED-LINK`, `INCLUDE-<rule id>`, …) so a self-test can
assert that *that* rule fired rather than that something went wrong. A code no
case can provoke is a rule that has quietly stopped checking anything.

The include scan is explicitly a supplement, not a proof: it sees textual
`#include` lines only, not template instantiation, not linker-visible symbol
use, and not runtime coupling through signals, meta-object lookup or dynamic
properties. The target rules read the evaluated graph, which does cover
transitive and generated edges.

### The public-header probe

For every published header of a module, `arch-public-headers` compiles a
translation unit that includes that header **and nothing else** — twice, so a
header must be idempotent and not merely survive one inclusion — into a
consumer that links **only that module**. The project's `src/` directory is not
on the probe's include path: the header must be reachable through the module's
own interface include directories, or a real client could not use it either.
`AUTOMOC` is off, so nothing papers over a header that does not stand on its
own.

If you add a public header, add it to that module's `public_headers` list in
`architecture.json`. If it does not compile alone, that is the finding.

### The exception ratchet

Deviations are not silent. Every entry under `exceptions` requires five fields
— `id`, `kind`, `owner`, `remove_in`, `reason` — so debt always carries an
owner and a removal point. Two classes exist:

- `migration-baseline` is **debt**. It must shrink to zero and carries the
  phase that clears it. It is summarised separately under "must shrink to
  zero", so the count is meant to fall.
- `architecture` with `remove_in: "never"` is a **permanent allowance**, for an
  edge that is correct rather than owed.

Adding an exception is how a needed deviation gets recorded; the ratchet then
fails on the *next* one that is not listed. The one exception in the tree today
is the permanent allowance for the six test suites whose subject is the
component-private engine implementation — a test whose subject is an
implementation must link that implementation. It names one target per site, so
a seventh linker still fails the check and has to be argued for.

Declaring an edge under `consumers[].deps` instead is not the same thing:
`deps` is consulted before `exceptions`, so one line there turns tracked debt
into permanent architecture with no owner and no removal date. That has already
happened once, to eight sites at a stroke.

### Sealed modules

`clash_mihomo_impl` is sealed: it may be reached only through a baselined
exception site. The seal is matched to its exceptions by **id glob** —
`sealed_while: ["*-component-private"]` — so the glob and the exception ids are
one mechanism in two places. Rename the id without the glob, or the glob
without the id, and the seal stops applying with nothing failing to report it.
Keep the suffix.

## Documentation references are checked too

Two further entries, `reference-check` and `reference-selftest`, exist because
a comment citing a document compiles exactly as well when the document is gone.
Two rules:

- **Resolves.** A path named in a shipping file must exist. A pointer at a
  missing document costs the reader a search before they learn there is nothing
  to find.
- **Survives.** A shipping file must not name a path the merge to the published
  branch deletes — working-state directories and the all-caps planning records
  under `docs/`. Such a reference is dangling already, just not yet visibly.

The scan is driven by `git ls-files`, never by globs: a pathspec like
`CMakeLists.txt` matches only the root file and `*.cmake` never matches
`CMakeLists.txt` at all, so glob-driven scans of this repository have reported
a clean tree while a real violation sat in a subdirectory build file.

```sh
python3 tests/architecture/reference_check.py --repo-root .
```

## Adding a test

1. **Put the source with its subject.** `tests/core/…`, `tests/platform/…`,
   `tests/ui/…`, `tests/app/…`, `tests/contracts/…`, `tests/workflows/…`,
   `tests/benchmarks/…`.
2. **Register it.** Add the executable and `add_test(NAME …)` in the nearest
   `tests/**/CMakeLists.txt`. Link the **production** library rather than
   recompiling its sources, so the suite exercises the objects the application
   ships. Substitute through a constructor parameter, not through a second
   compilation of the same translation unit.
3. **Name it by subject.** The CTest name, the target name and the source stem
   should agree: `first_launch_test.cpp` builds `first-launch-tests` and
   registers `first-launch`. A test's subject names it better than its position
   in a list.
4. **Label it.** `clash_qt_label(<name> …)` sets the labels the Make lanes
   select. `unit` is deterministic with no real I/O beyond a temp directory;
   `integration` uses real Qt networking, filesystem or child processes against
   fixtures; `ui` builds widgets. `real-core`, `native`, `privileged` and
   `benchmark` are excluded from the routine lane — `real-core` because it
   drives the locally built engine, the others because they need a machine the
   routine lane does not have.
5. **Isolate it.** Every test gets its own `CLASH_QT_DATA_DIR`. In the top-level
   `tests/CMakeLists.txt` that is applied to every registered test in a final
   loop. **In a subdirectory you must call `clash_qt_isolate_test(<name>)`
   yourself**: CMake does not let a parent scope set properties on a child's
   tests, so naming a subdirectory test in the parent's loop sets nothing and
   says nothing. The same applies to `QT_QPA_PLATFORM=offscreen` (and
   `QT_QUICK_BACKEND=software` for the `QQuickView`-embedding suites).
6. **Record it.** Update `tests/README.md` in the same commit. A registered
   name is how a lane is selected, so renaming a test is a change to the suite.

Two mistakes this tree has already made, both of which look like working code:

- **A guard applied before the thing it guards exists.** The offscreen block
  used to sit above the suite registrations, where `if(TEST …)` was simply
  false for anything not yet defined — so seven widget suites silently ran
  against the real display. A guard that quietly does nothing is worse than no
  guard.
- **A guard applied across a scope boundary.** The same `if(TEST …)` is false
  for a test registered in a subdirectory. Set it where the test is defined.

Preference isolation is harness-level for the same reason: `core::preferences`
falls back to the developer's real native store when `CLASH_QT_DATA_DIR` is
unset — correct for a real user, dangerous for a test. Isolation used to be
opt-in per suite, and a new suite that forgot it wrote into the real store.
Forgetting is no longer sufficient to reach it.

## Conventions

**Name things by subject.** Not by their position in a plan, a phase or a work
queue. A name that encodes a schedule stops being true the moment the schedule
moves, and it tells a later reader nothing about what the thing is.

**Say why, not only what.** Where a build rule, a link edge or a test gate
exists because something broke, the comment states what breaks without it.
Several rules in this tree look arbitrary until you know the failure they came
from; that information belongs beside the rule.

**Declare only the dependencies you use.** `PUBLIC` when it appears in your own
public headers, `PRIVATE` otherwise. A public dependency propagates to every
consumer — that is how the profile store once dragged Qt Gui and WebSockets
across the tree, and how consumers reached the private engine implementation
without asking for it.

**Change a mechanism in all the places it lives.** A name matched by machine —
a CMake target, a CTest name, an exception id and the glob that pairs with it,
a violation code and the self-test that asserts it, a copy destination and the
bundle-relative string that resolves it — is one mechanism written twice.
Change both halves in the same commit.

**Keep references live.** Cite a document only if it exists and will still
exist after the merge. Cite a file by its real path.

**Every command returns non-zero on failure.** An empty test selection is a
failure, not a pass; a skip is unavailable evidence, not a pass; a verification
step that only logs an error is not a verification step.

## Before you commit

```sh
make doctor
make test
```

`make test` includes the architecture, public-header and reference checks, so a
green routine lane is also a statement that the dependency graph, the published
headers and the documentation references are intact. Run
`make test-integration` as well when you have touched the engine, the module
boundary or a journey.
