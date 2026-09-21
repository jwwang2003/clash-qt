# Project structure refactor

Status: proposed, based on the working tree reviewed on 2026-09-20.
This document plans the refactor; no source files have been moved.

Parallel implementation uses the [sub-agent execution plan](PARALLEL_EXECUTION_PLAN.md).
Its ownership and integration gates apply to the exact destinations below.

[Project goals](PROJECT_GOALS.md) additionally require local mihomo builds, the
separate COM/component boundary and a documented Make facade. Preserve the current
source/docs on legacy-v1 before these moves; replace the old docs only at the gated
post-refactor main cutover described in [BUILD_RELEASE_PLAN.md](BUILD_RELEASE_PLAN.md).

## Review findings

The main navigation problem is `src/ui/`: it contains 48 C++ source/header files
directly, plus its source manifest, resource manifest, and a QML subdirectory.
Pages, application shell, reusable controls, graph code, and theme code are peers.
`src/core/` has 13 direct C++ files plus three small subfolders, mixing controller
transport, backup storage, telemetry, shared data, and YAML utilities.

There are also responsibility boundaries worth fixing after the file moves:

| Location | Finding | Consequence |
| --- | --- | --- |
| `src/core/profile/profile_store.cpp` (889 lines) | Persistence, subscription downloads, overrides, enhancements and runtime generation | Profile storage cannot be reused independently of runtime configuration |
| `src/main.cpp` | Construction, runtime reload scheduling, snapshot cleanup, proxy restoration and shutdown | Lifecycle rules are buried in event connections and local captures |
| `src/ui/backup_page.cpp` | Creates BackupStore and coordinates maintenance, pending writes and core shutdown | Application correctness depends on a page's existence |
| `src/main.cpp:182` | Discovers backup services using `window.findChildren<core::BackupStore *>()` | Shutdown discovers its dependencies through the widget tree |
| `src/ui/settings_page.cpp` (495 lines) | Settings widgets and proxy/autostart operation coordination | UI composition and application behavior evolve together |
| `src/core/types.h` | Controller endpoint, proxy, connection, rule, log and config types | Unrelated consumers include one broad header |
| `CMakeLists.txt` | Deployment, QML, service targets and all test definitions | Small changes require navigating a large root build file |
| `src/*/sources.cmake` | All source manifests append to `clash-qt` directly | Folders look modular, but most code has no reusable build target |
| `tests/` | 12 source files in one directory; runtime/UI suites span many features | Test ownership is unclear as features grow |

The current build registers 14 tests on macOS, including two helper self-tests.
Only test inventory was inspected for this plan; it is not a passing test result.

The companion [test redesign plan](TEST_STRATEGY.md) expands this refactor with
architecture checks, feature contracts, application workflows, and native/package
validation. Its test layout is the long-term destination; the moves below are the
initial step that preserves existing suites while source code is relocated.

## Proposed folder structure

Group UI code by feature, and backend code by responsibility. Keep related headers
and implementations together. A small feature may contain only a page pair today;
its models and dialogs can stay beside it as it grows.

```text
src/
  main.cpp                         Entry point and application construction
  app/
    app_context.h
    runtime/                       Reload, routing, snapshot lifecycle (phase 3)
    lifecycle/                     Startup/shutdown coordination (phase 3)
    backup/                        Restore coordination (phase 3)
  core/
    mihomo/                        Controller client, discovery, provider client
      process/                     Existing core process implementation
    config/                        YAML, enhancements, later ConfigComposer
      enhance/
    profiles/                      Profile persistence and subscriptions
    backups/                       Archive storage and WebDAV
    telemetry/                     Traffic history/statistics
    types/                         Focused shared value types (phase 3)
  platform/
    browser/                       Browser discovery/launch
    proxy/                         Existing system proxy implementation/service
    service/                       Privileged helper client and installer
    system/                        Existing autostart and hotkeys
  services/
    macos/                         Standalone privileged helper executable
  ui/
    shell/                         Main window, toolbar, tray, dashboard launcher
    pages/
      overview/                    Home page and its traffic graph/QML
      profiles/                    Profile page, later its model/delegate/editor
      proxies/                     Proxy selection page
      connections/                 Connections page and later its model
      rules/                       Rules page and model
      providers/                   Provider page
      logs/                        Log page
      backups/                     Backup page
      settings/                    Settings page and settings-specific controls
    widgets/                       ComboBox, ToggleSwitch, SettingsSection
    theme/                         Theme and formatting
    resources/                     Resource manifest
tests/
  core/                            Runtime, controller, provider, backup, history
  platform/                        Proxy and privileged-service tests
  ui/                              Data pages, tray, routing, dashboard tests
  support/                         Shared fixtures when actually extracted
cmake/
  TrafficGraph.cmake
  Packaging.cmake
  ResolveQmlLinks.cmake
```

The tree describes the target layout. Create directories when moving existing
files or extracting working code; do not commit empty future modules. Keep
`assets/` for source images and desktop assets. Leave ignored `build*` and `.cache`
directories alone; they are generated artifacts, not source modules.

This is an incremental refinement of [the management architecture proposal](MODULAR_MANAGEMENT_PLAN.md).
It establishes readable folders before the proposed domain/integration abstractions
are needed. Avoid maintaining two competing layouts or adding empty identity,
policy, VPN, plugin or COM implementations during this refactor.

## Exact file movement for the first phase

Names below represent both `.h` and `.cpp` where present. Preserve filenames,
class names, namespaces and signal signatures during relocation.

| Current location | Destination |
| --- | --- |
| `src/app_context.h` | `src/app/app_context.h` |
| `src/ui/main_window.*`, `tray_icon.*`, `tray_proxy_menu.*` | `src/ui/shell/` |
| `src/ui/dashboard_button.*`, `routing_controls.*`, `proxy_environment.*` | `src/ui/shell/` |
| `src/ui/home_page.*`, `traffic_graph.*` | `src/ui/pages/overview/` |
| `src/ui/qml/TrafficGraph.qml` | `src/ui/pages/overview/qml/TrafficGraph.qml` |
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
| `src/ui/resources.qrc` | `src/ui/resources/resources.qrc` |
| `src/core/mihomo_client.*`, `provider_client.*`, `controller_discovery.*` | `src/core/mihomo/` |
| `src/core/process/` | `src/core/mihomo/process/` |
| `src/core/profile/` | `src/core/profiles/` |
| `src/core/enhance/` | `src/core/config/enhance/` |
| `src/core/yaml_util.*` | `src/core/config/` |
| `src/core/backup_store.*` | `src/core/backups/` |
| `src/core/traffic_history.*` | `src/core/telemetry/` |
| `src/core/types.h` | Keep temporarily; split only in phase 3 |
| `src/platform/browser_launcher.*` | `src/platform/browser/` |
| `src/service/macos_helper.mm` | `src/services/macos/macos_helper.mm` |
| `tests/runtime_test.cpp`, `controller_test.cpp`, `provider_test.cpp`, `backup_test.cpp`, `traffic_history_test.cpp` | `tests/core/` |
| `tests/system_proxy_test.cpp`, `system_proxy_async_test.cpp`, `privileged_service_client_test.cpp` | `tests/platform/` |
| Remaining four `tests/*.cpp` files | `tests/ui/` |

Use project-rooted includes such as `ui/pages/profiles/profiles_page.h`, with `src`
as the include root during migration. Update test includes, CMake source paths,
developer documentation and source manifests together. Preserve Git move history
using filesystem moves or `git mv` as appropriate; do not stage unrelated edits.

Keep a small UI source manifest per substantial group (shell, pages, widgets,
theme) with explicit source lists. Do not introduce one CMake file per widget or
use recursive globbing. Existing feature source manifests move with their files;
update their parent includes and remove obsolete entries.

## Build organization and dependencies

Phase 2 moves tests into `tests/CMakeLists.txt`, QML setup into
`cmake/TrafficGraph.cmake`, packaging into `cmake/Packaging.cmake`, and helper
target definitions into `src/services/macos/CMakeLists.txt`. Root CMake retains
project/compiler options, dependency discovery, module assembly and entry targets.

Create static libraries for reusable backend responsibilities first:
`clash_config`, `clash_profiles`, `clash_backups`, `clash_telemetry`,
`clash_mihomo_impl`, and `clash_platform`. The mihomo implementation library is
private to its separately built portable component; the UI links
only the published interface/loader, as required by G2. These internal libraries
must not become a way to bypass the component boundary. Keep the UI in the executable initially;
extract UI targets only after its test substitution points are explicit. Make
source manifests accept their owning target rather than hard-coding `clash-qt`.
Tests should link production targets except for documented test variants.

Target dependencies must reflect actual code before enforcing ideal boundaries.
For example, profiles currently calls controller discovery to seed runtime data,
and CoreProcess uses the privileged-service client. Preserve those dependencies
at first, then remove the profiles-to-mihomo dependency when runtime generation
moves out. Do not create target cycles or require every library to link all Qt
modules. Use public dependencies only for types appearing in public headers.

The intended dependency direction after extraction is:

```text
main/composition -> UI -> application services -> core components / platform
core components -> focused value types and required infrastructure adapters
platform -> operating-system APIs
privileged helper -> native frameworks + its explicit IPC contract
```

Core/platform code must not depend on widgets or application services. Application
services emit state and requests; the UI renders dialogs. The composition root
owns services explicitly. A folder is not automatically a library or plugin.

## Responsibility extraction after relocation

1. **ConfigComposer and runtime preparation:** move YAML transformation and
   precedence out of ProfileStore into `core/config/`. Inputs are immutable values;
   loading files, geo-data seeding, runtime snapshot writing and cancellation stay
   in an I/O coordinator. Preserve the different legacy enhancement/runtime merge
   semantics, application-owned fields, validation and generation guards.
2. **RuntimeCoordinator:** move reload debounce, start/restart requests, readiness
   and snapshot retention from main/window into `app/runtime/`. Keep CoreProcess
   as the existing mihomo execution backend. Treat external controllers separately
   from owned processes; never stop a core merely because a view is destroyed.
3. **ShutdownCoordinator and BackupCoordinator:** give BackupStore an application
   owner, inject it into its page, and remove widget-tree discovery. Preserve
   maintenance gates, pending-save completion, restore preparation, confirmed core
   stop, owned-proxy restoration and warning acknowledgement. Quit dialogs remain
   in the shell; the coordinator waits for their results without owning widgets.
4. **RoutingController:** centralize proxy/TUN actions used by settings, toolbar,
   tray and hotkeys. Widgets become consumers of shared confirmed/pending/error
   state. Keep endpoint-generation checks and confirmed TUN readback.
5. **Settings and page internals:** separate runtime, core, startup and proxy
   settings sections where their state is independent. Move ProfileModel and
   ProfileDelegate beside ProfilesPage, and ConnectionModel beside ConnectionsPage.
   Keep short local helpers private; avoid a global models/utils dumping ground.
6. **Focused value headers:** replace `core/types.h` with endpoint, proxy,
   connection, rule, log and config headers under `core/types/`. Move Provider and
   Profile value declarations out of service headers only where consumers benefit.
   Keep a temporary umbrella include if necessary during migration, then remove it.

Do not split the 814-line macOS helper in the file-move phase. Installation,
validation, IPC and process supervision can be separated within `services/macos`
later, preserving its security boundaries and both self-test entry points. Likewise,
separate BackupStore's archive codec and WebDAV transport only in focused follow-up
changes, rather than splitting large files mechanically by line count.

## Refactor-specific traps

- `tests/system_proxy_test.cpp` includes a production `.cpp` to substitute commands
  and inspect internal state. Fix its relative path during relocation; do not also
  link a second copy of that implementation. Replace this with an injected command
  runner in a later behavioral change.
- `tests/dashboard_async_test.cpp` supplies BrowserLauncher definitions. Linking a
  new platform library can defeat that substitution or duplicate symbols. Introduce
  an injected browser operation before consolidating this test's dependencies.
- Preserve the QML URI `ClashQt`, type `TrafficGraph`, resource alias and both app
  and test resource generation. Current code uses QQuickView and Qt Quick; use
  current CMake requirements instead of copying older QuickWidgets documentation.
- Moving the QRC file changes filesystem-relative asset paths. Preserve the public
  `:/ui/chevron-down-*.png` URLs used by theme code. When later placing resources in
  static libraries, verify their registration/linking in every consuming executable.
- Update both helper build targets, bundle copy paths and install/deployment logic.
  Keep macOS helper code out of non-Apple builds and retain its native framework
  links. Preserve platform-specific browser, proxy and hotkey linkage.
- Keep settings keys, data directories, backup layouts, controller endpoints and
  helper IPC schema unchanged. A source-tree refactor requires no user-data migration.
- Existing uncommitted changes include CMake, README, profiles UI and traffic graph
  work. Rebase the movement map on the live tree before execution and preserve all
  pending work. No cleanup/reset of the user's working tree is part of this plan.

## Delivery and verification

| Change | Scope | Completion criteria |
| --- | --- | --- |
| 0. Baseline | Inventory live edits, configure/build and run existing tests | Record failures/skips before any movement; current inventory alone is not a baseline pass |
| 1a. UI folders | Move shell/pages/widgets/theme/resources and update references | Build app/UI tests; all existing tests pass; resources and QML load; native graph/tray smoke check |
| 1b. Backend/test folders | Move core/platform/helper/test files; keep APIs stable | Fresh configure/build, full suite, helper tests and packaging smoke check |
| 2. Build targets | Extract CMake concerns and reusable backend libraries | Tests link the intended implementation, no duplicate symbols, app installs and starts from an isolated prefix |
| 3a. Configuration | Extract composer and runtime I/O coordination | Legacy output parity, invalid-input preservation, stale-result cancellation and snapshot lifecycle tests |
| 3b. Application ownership | Extract runtime/shutdown/backup/routing coordinators | Behavior tests cover shutdown/restore ordering, disconnects, shared controls and pending-operation cancellation |
| 3c. Local cleanup | Split selected settings/models/value headers | UI regression suites and focused dependency review; no unrelated feature changes |

For relocation, use existing tests and build verification. Follow
[TEST_STRATEGY.md](TEST_STRATEGY.md) for the broader feature and workflow coverage,
including ordinary success paths and architectural boundaries. Preserve existing
CTest names during relocation; subsequent suite splits document their replacements.
Keep offscreen, native, privileged and benchmark environments explicitly separate.

Use a fresh ignored build directory to catch stale generated paths. Run native
checks against an isolated data directory, without installing/removing the live
privileged helper or changing the user's network configuration. Validate Windows
and Linux builds in their own environments; report unavailable checks explicitly.

Success means a page and its private implementation are easy to locate, shared UI
controls are distinct, backend libraries can be tested without the GUI, and service
lifetime no longer depends on widget construction. This also leaves a clear future
home for a separately built MihomoBackend in `core/mihomo`, using a portable
COM-style module/loader on all three OSes, following the
user's fxcom reference without its FX naming. A native Windows bridge would be an
optional separate integration, as defined by the [COM plan](COM_MODULE_PLAN.md).

Monitoring/MITM features introduce `core/capture`, `app/capture`, `integrations/capture`,
`platform/capture`, `platform/trust`, `services/capture` and `ui/pages/capture` under
`src/`, plus repository-root `services/monitor` and `packaging/capture`. Exact ownership
and tests follow [CAPTURE_MODULE_PLAN.md](CAPTURE_MODULE_PLAN.md). These are new modules,
not reasons to move existing backend/UI code again. Keep `3rdparty/ref/fxcom` as a
design reference and `3rdparty/mitmproxy` as a source dependency, both outside project
implementation ownership unless a separate task explicitly changes them.
