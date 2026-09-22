# Migration

This is for anyone reading clash-qt code, commits, issues or notes written
before the restructuring. It says where things went, what is genuinely different
now, and which behaviours were changed on purpose rather than by accident.

If you are looking for how the system is shaped today, read
[architecture.md](architecture.md) instead. This document is only about the
difference.

## What the application used to be

One executable. `src/main.cpp` constructed everything and also carried the
runtime reload scheduling, snapshot cleanup, proxy restoration and shutdown
sequencing as event connections and local lambdas. `src/ui/` held forty-eight
source and header files in a flat directory. `src/core/` mixed the controller
client, the child-process owner, profile persistence, configuration
transformation, backups and telemetry at one level. Source manifests appended
everything to the `clash-qt` target, so the folders looked modular but almost
none of the code had a reusable build target. The mihomo engine was compiled
directly into the application.

Several consequences followed from that, and they are the reason for the change:
application correctness depended on a page existing (shutdown discovered the
backup store by walking the widget tree); lifecycle rules were buried in
connections; unrelated consumers included one broad header; and there was no
boundary that a test could substitute at.

## Where the files went

Names cover both `.h` and `.cpp` where both existed. File names, class names,
namespaces and signal signatures were preserved across the moves.

| Was | Is |
| --- | --- |
| `src/app_context.h` | `src/app/app_context.h` |
| `src/ui/main_window.*`, `tray_icon.*`, `tray_proxy_menu.*` | `src/ui/shell/` |
| `src/ui/dashboard_button.*`, `routing_controls.*`, `proxy_environment.*` | `src/ui/shell/` |
| `src/ui/home_page.*`, `traffic_graph.*` | `src/ui/pages/overview/` |
| `src/ui/qml/TrafficGraph.qml` | `src/ui/pages/overview/qml/` |
| `src/ui/profiles_page.*` | `src/ui/pages/profiles/` |
| `src/ui/proxies_page.*` | `src/ui/pages/proxies/` |
| `src/ui/connections_page.*` | `src/ui/pages/connections/` |
| `src/ui/rules_page.*` | `src/ui/pages/rules/` |
| `src/ui/providers_page.*` | `src/ui/pages/providers/` |
| `src/ui/logs_page.*` | `src/ui/pages/logs/` |
| `src/ui/backup_page.*` | `src/ui/pages/backups/` |
| `src/ui/settings_page.*`, `service_settings.*`, `hotkey_settings.*`, `chain_editor.*` | `src/ui/pages/settings/` |
| `src/ui/combo_box.*`, `toggle_switch.*`, `settings_section.*` | `src/ui/widgets/` |
| `src/ui/theme.*`, `formatting.*` | `src/ui/theme/` |
| `src/ui/resources.qrc` | `src/ui/resources/` |
| `src/core/mihomo_client.*`, `provider_client.*`, `controller_discovery.*` | `src/core/mihomo/` |
| `src/core/process/` | `src/core/mihomo/process/` |
| `src/core/profile/` | `src/core/profiles/` |
| `src/core/enhance/` | `src/core/config/enhance/` |
| `src/core/yaml_util.*` | `src/core/config/` |
| `src/core/backup_store.*` | `src/core/backups/` |
| `src/core/traffic_history.*` | `src/core/telemetry/` |
| `src/platform/browser_launcher.*` | `src/platform/browser/` |
| `src/service/macos_helper.mm` | `src/services/macos/` |

Two directories are new rather than moved. `src/app/` holds the application
services lifted out of the entry point. `src/integrations/component/` holds the
host side of the module boundary — the loader, the host-side backend facade and
the private marshalling codecs. There is no `src/integrations/` tree for
anything else: no identity, management, network, capture, policy or extension
directories exist, whatever an old note may propose.

**`src/core/types.h` is still one header.** Splitting it into focused value-type
headers was planned and has not happened. The include rule that lets platform
adapters reach shared value types already allows a `core/types/**` directory, so
the split can happen without a rule change, but today there is exactly one file.

## From one executable to declared libraries

Backend responsibilities are now static or interface libraries, each owning one
thing and declaring only the dependencies its code actually has. The full list
and the reason for each boundary is in [architecture.md](architecture.md); the
graph itself is `tests/architecture/architecture.json`, which is checked against
the evaluated CMake graph on every run.

The rule that matters when reading old code: **a target that links what it does
not use makes the dependency graph lie.** Several link edges that looked
harmless were removing information. One example is worth knowing because its
shape recurs: the profile store used to call controller discovery to work out
where to seed geo data from. That single path string made the profiles library
link the component-private engine, and because link edges propagate, every
consumer of profiles — the application included — was handed the private
implementation and Qt WebSockets along with it. The call moved to the
composition root, which already knows which engine this process is managing, and
the edge is gone rather than baselined.

**The UI is still not a library.** It is compiled into the executable from
per-directory source manifests, and widget test suites name the specific `.cpp`
files they need. Extracting a UI target waits until its test substitution points
are explicit.

## The engine moved behind a binary boundary

This is the largest single change, and the one most likely to invalidate an
assumption in old code or old notes.

**Then:** the engine implementation was linked into the application. UI code
included engine headers directly and called the client, the provider client and
the process owner.

**Now:** the engine is built as a separate shared library that the application
loads at run time across a C ABI. The application does not link it. Between them
sits a published, header-only, Qt-typed backend contract that both the real
engine and a deterministic fake implement, plus a Qt bridge that turns the
contract's plain observer callbacks into signals for the UI.

What a reader of old code needs to know:

- **Engine headers are off limits above the boundary.** Nothing in `src/ui/`,
  `src/app/` or `src/main.cpp` may include `core/mihomo/**`. This is checked.
  The replacement is the published contract and its Qt bridge; they exist to
  displace exactly those includes.
- **The engine implementation library is sealed.** Only the six test suites
  whose subject *is* that implementation may link it. Everything else, including
  every journey suite and the application itself, reaches the engine through the
  loaded module. See "The seal" in [testing.md](testing.md).
- **The application will not start without the module.** There is no fallback to
  an in-process engine. If the module cannot be found, fails the handshake or
  cannot create a session, the process prints a diagnostic and exits.
- **Module discovery is explicit.** The loader takes either a path the caller
  chose or the installation-relative one. It never consults `PATH`, a build
  directory or a source tree. `CLASH_QT_MODULE_PATH` selects a module
  explicitly; tests use it to point at the freshly built artifact.
- **Identity is validated.** A loader that asked for the shipping module refuses
  the test double, even though both export the same entry symbol.
- **Nothing Qt or standard-library crosses the boundary.** Data crosses as
  reference-counted buffers owned and freed by the producing side. A container
  allocated by one module and freed by another is a crash waiting for a
  different compiler, runtime or allocator.

The privileged-execution seam runs the other way and is worth a separate note,
because getting it wrong has already cost this project a shipped defect. The
engine publishes the privileged-service interface itself, in the header-only
contract rather than in its private implementation, because the composition root
is what supplies the concrete platform client. Without that injection the
backend falls back to a null service whose `isSupported()` is false, service
mode is silently unavailable, and a saved preference is discarded without a
word.

## What left the entry point

`src/main.cpp` now does two things: it constructs long-lived objects and hands
them to whoever needs them, and it owns the startup work for which no service
class exists — command line, single instance, settings bootstrap.

| Was in `main.cpp` or a page | Is now |
| --- | --- |
| Reload debouncing, runtime-config readiness, snapshot retention | `app::runtime::RuntimeCoordinator` |
| System proxy, TUN and mode intent | `app::runtime::RoutingController` |
| Quit gating, busy conditions, shutdown sequencing, warnings | `app::lifecycle::QuitGuard`, `ShutdownCoordinator` and its ports |
| Constructing and owning the backup store, maintenance ordering | `app::backup::BackupCoordinator` and its store session |
| Adapting the platform privileged client to the engine's seam | `app::composition` |

Each of those is a library with its own suite, and each reaches what it cannot
own through a port the composition root binds. That is what makes the quit gate
testable by holding and releasing each condition rather than by waiting for one.

Two discoveries-by-widget-tree are gone. Shutdown no longer finds the backup
store with `findChildren`, and the "shutting down" application property is
replaced by a lifecycle state query. If you see either idiom in old code, it has
no counterpart now.

## The test suite

Two monolithic suites were partitioned so that a lane could select them and a
failure could name a subject. The former runtime suite became the
config-generation, profile-store, core-process and runtime-maintenance entries.
The former data-pages suite became the seven per-page entries plus the two
graphics benchmarks, which had been reporting "Passed" while silently skipping
inside the larger suite. The per-case preservation map is `tests/README.md`;
historical mappings remain retrievable in Git.

Two naming details will confuse a reader of old scripts. A journey's executable
target still carries its source file's stem — `first_launch_test.cpp` builds
`first-launch-tests` — because the architecture check declares CMake target
names, while the *registered* CTest name is what selects the test. And journeys
are named for their subject rather than for a position in a list; the ordinal
prefixes some registered names once carried are gone.

## Behaviours that were deliberately changed

These are not incidental. Each was changed on purpose, and each will surprise
someone who remembers the old behaviour.

**Preference access goes through one accessor.** Production code must never
construct the settings object itself. On macOS the organization/application
constructor is hard-wired to the native format — it ignores both the default-
format and the path override — so a bootstrap that appeared to redirect settings
left the real store live, and test processes wrote into the developer's own
preferences. With no data-directory variable set, the accessor is *exactly* the
previous native store: same format, same scope, same location, same keys, so an
existing installation sees no change. With the variable set, it is an INI file
at an explicit absolute path beneath that directory and the native store is
never opened. The organization and application names are constants rather than
parameters, because changing either would orphan every existing installation's
preferences.

**Every registered test gets its own data directory by default.** Isolation used
to be opt-in per suite, and a suite that forgot it reached the real store. A
suite that wants something narrower overrides it; forgetting is no longer
sufficient.

**Geo-data seeding is injected, not discovered.** The composition root supplies
the compatibility import location, and only when no data directory has been set.
The profile store no longer works it out by calling into the engine.

**The managed engine is always built from the recorded source commit.** There is
deliberately no fallback to `PATH`, to another Clash installation, or to a
binary download. The build depends on the recorded submodule pointer, so a
dependency change rebuilds the engine rather than silently reusing another
revision's binary. An explicit user setting selects the executable;
`CLASH_QT_CORE_BINARY` is a development fallback that lets a local run point at
the locally built engine without writing to the user's settings.

**The privileged helper socket can be selected explicitly.**
`CLASH_QT_SERVICE_SOCKET` names it, and when a data directory is set the socket
defaults to a file inside it. A relative path is rejected outright. A data
directory alone is not permission to query the machine-wide helper — the
settings page queries helper status on startup, so an isolated run needs an
isolated socket too.

**Superseded backend events are filtered once, in the bridge, with one
exemption.** Uniform filtering is wrong here: it would drop the unconfirmed stop
that blocks quit, and wedge quit forever. The managed engine's terminal outcomes
are therefore exempt from the filter on purpose.

**Routing errors have exactly one channel and one consumer.** The routing
controller's error signal is the only carrier of the "could not restore the
system proxy" message, and the composition root is its single consumer. Neither
the routing controls nor the settings page republishes it — reporting it exactly
once is why.

**Liveness is edge-triggered.** The version fetch is the process's sole liveness
probe and the only call that marks the backend connected. The periodic poll
lives in the composition root, on the stack, so it stops when the entry point
returns rather than firing at a half-destroyed backend during teardown.

**Gated lanes are honestly not run.** The graphics benchmarks carry a label the
routine lane excludes, and they must *not* receive the offscreen platform: with
their measurement flag set, offscreen turns a skip into a failure. Reserved
lanes for native and privileged work carry zero registered entries rather than
being quietly satisfied by unrelated tests.

## Parity with Clash Verge Rev

clash-qt is **not** a complete replacement for Clash Verge Rev, and nothing in
the restructuring changed that. The shared mihomo core supplies proxy protocols
and routing; it does not supply the service management, profile workflows,
update distribution, desktop integration, diagnostics or recovery interface
around them. An external dashboard is useful access to core functionality, not
evidence of native parity.

The parity position, unchanged by the restructuring:

**Working, in a basic form:** attaching to an existing controller; managed
engine start, stop and restart with validation before replacement; proxy modes;
local and URL subscriptions with a persisted index; the global merge and script
enhancement chain; proxy and rule providers with quota and expiry metadata; the
rules list; live logs; the connections view with filtering, sorting, close and a
bounded closed history; the traffic graph and overview diagnostics; tray
controls including dynamic profile and proxy-group menus; login registration;
global shortcuts; scoped local backup, export, import and restore with
checksums, rollback and a WebDAV round trip; and, on macOS, a privileged helper
with an owner-authenticated socket lease and TUN read-back.

**Partial, with a known gap:** scheduled subscription refresh (no per-request
user agent, per-subscription proxy choice or provider-supplied update policy);
profile content editing (plain YAML, no structured rules/proxies/groups
editors); enhancement compatibility (a different script engine, no per-profile
merge or script binding, no sequence-edit workflow); latency testing;
cross-platform system-proxy validation; packaging and install.

**Absent:** PAC and the system-proxy guard; application and engine updates with
verification and rollback; the subscription URL scheme handler and QR sharing;
runtime proxy chaining; media and AI availability checks; platform diagnostic
tools; shipped translations; backup schedules and remote history; Verge Rev
archive compatibility.

Two statements deserve to stay attached to any parity claim. The audit that
established this matrix recorded *source coverage*, not a guarantee that every
feature works on every platform — and macOS is the only validation environment
that exists. Windows and Linux carry conditional code but zero compile or run
evidence; the presence of a platform branch is not proof of installed desktop
behaviour.

## Reading old notes

A number of planning documents guided this work and are being removed. If you
find one in an old branch or an old commit message, treat it as history rather
than as a description of the system, and prefer the code. Specifically:

- The proposed folder structure never included the integrations tree that the
  module boundary actually lives in.
- Descriptions of the loader's location, of which library links which, and of
  where the engine is built changed more than once during the work. The
  authoritative statement is `tests/architecture/architecture.json`, which is
  checked; prose is not.
- Test-suite counts in old documents describe a suite roughly a fifth of the
  current size. `tests/README.md` carries the current registry.
- Acceptance claims for Windows and Linux in old plans are proposals, not
  results.
- Old links into `src/` almost all predate the moves in the table above.

Comments in shipping code no longer identify anything by its position in a plan,
and no shipping file may name a document the merge deletes. Both rules are
checked; see the reference check in [testing.md](testing.md).

## Related documents

- [architecture.md](architecture.md) — the system as it is now.
- [testing.md](testing.md) — the lanes and the checks.
- [module-api.md](module-api.md) — the published contracts and their revisions.
- [build.md](build.md), [packaging.md](packaging.md),
  [development.md](development.md), [configuration.md](configuration.md).
