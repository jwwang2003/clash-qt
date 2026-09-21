# Implementation and visual audit

This audit compares clash-qt with the local Clash Verge Rev checkout, not a feature list from a different release. See [FEATURE_PARITY.md](FEATURE_PARITY.md) for the source-linked comparison and outstanding replacement requirements. Full feature parity is **not** established.

## Correctness fixes

- Profile selection, edits, subscription refresh and enhancement changes now regenerate the managed runtime. Enhancement scripts run with the profile name, log output and a timeout. Application-owned controller settings are applied last.
- Core validation is asynchronous and preserves the existing process after an invalid edit. Hanging controller readiness requests time out, and pending validation/probes are cancelled on stop or replacement.
- Controller changes and disconnects invalidate outstanding replies, clear stale state and refresh runtime data. Failed mode/node changes reconcile with the actual core state. IPv6 controller URLs are encoded correctly.
- System proxy changes snapshot prior settings, roll back partial failures and restore settings on normal exit/stop only while still owned. HTTP and SOCKS ports are handled separately. Restoration errors are reported; unrelated external changes are preserved. These paths are covered with a command mock, not privileged changes to the user's network settings.
- Profile reload cancels pending subscription downloads so a restore cannot be overwritten by an old response. Subscription URLs are validated and errors avoid displaying their tokens. YAML preserves quoted string types and rejects excessive/cyclic recursion.
- Backup restoration validates checksums, indexes, paths, size limits and cross-platform filename collisions before stopping the core. Replacement uses staging and rollback. WebDAV redirects are not followed with credentials.
- Enhancement files now open in an in-app editor instead of invoking the operating system's handler for JavaScript files. A single-instance lock prevents concurrent writers to one data directory.
- Fixed login-item path quoting/escaping and Windows const-string compilation; browser selection falls back to the system browser if a saved browser is unavailable.

## Added application workflows

Home overview/traffic/DNS diagnostics; provider refresh and health checks; local profile creation and editing; runtime override settings; local backup/export/import/restore and manual WebDAV transfers; richer tray controls; searchable/exportable logs; proxy filtering/sorting and automatic-group pinning; connection speeds, details/copy and bounded closed history.

These additions do not supply the missing privileged service, app/core updater, full per-profile Verge enhancement compatibility, PAC/guard, release distribution or all remaining workflows in the parity matrix.

## Visual findings and changes

Live macOS inspection covered the original Profiles and Settings pages, then the revised Home, Profiles, proxy list and YAML editor using an isolated local profile. The audit found:

- The original status bar said “core stopped” while displaying a live external core. The revised status distinguishes external and managed cores and disables unavailable controls.
- The dark navigation highlight had poor contrast. Accent colors and selected text were adjusted; navigation icons now distinguish the added pages.
- “ACTIVE” on a selected profile implied that its configuration was running. It now says “SELECTED”, and profile changes actually reload a managed core.
- The first Home implementation overlapped chart/totals and DNS controls at the default window size. Home now uses scrollable cards with explicit chart/result heights; a widget layout regression covers compact and default page widths.
- Fixed-font editors were too small relative to surrounding UI. Editors/logs inherit at least the UI font size. Enhancement actions use two rows to fit smaller windows.
- Added visible empty states, profile actions, persistent connection selection/scrolling, and meaningful error feedback.

The Mac locked during the first final native visual pass. A later follow-up found the original `build/clash-qt` executable was stale after switching to an app bundle. The build now replaces it with a link to the current executable. The updated native macOS app was reopened and its Home, Providers, Backups and expanded Settings pages were inspected in dark mode. Light mode, compact native windows, provider/backup dialogs and tray interaction still need further visual coverage. Synthetic offscreen Home renderings at compact/default page sizes were inspected and show the layout overlap resolved. These renderings and layout tests are additional evidence, not a substitute for native visual verification. Generated images are in `build/audit-layout/`.

## Validation

- Built the macOS application bundle with Qt 6.11.1 and Apple Clang.
- All six CTest suites passed in the final run (6.01 seconds): runtime/process, controller HTTP, providers, backup/restore/WebDAV, data-page widgets/layout and mocked macOS proxy transactions.
- In a disposable data directory, launched the installed mihomo binary against a local fixture on controller port 29097 and proxy port 27890. Saving its YAML changed the running mode from Rule to Direct and reconnected the UI. A local HTTP request through that proxy returned the expected response.
- Used only local HTTP/WebDAV fixtures and a proxy-command mock for mutation tests. No user's subscription was edited, WebDAV service contacted, or system proxy/login item changed during testing.
- No native Windows/Linux or privileged TUN verification was performed. The macOS bundle remains a development build, not a signed standalone release.

## Routing toggle follow-up

Added native System Proxy and TUN switches in the top toolbar, plus matching checked tray actions. System Proxy shares the existing Settings state and implementation. TUN reads the current config, patches only its enable flag and absent defaults, then reads back actual core state before confirming and persisting the change. This matters because mihomo can return HTTP 204 even when interface creation fails. The failure leaves the switch off and reports the problem; no privileged service installation is implied.

Managed runtime defaults enable automatic routing/interface detection only when unspecified. Explicit profile values remain authoritative. All eight CTest suites passed (9.66 seconds), including mocked successful TUN enable/disable, silently rejected interface setup, cancellation, stale reads and switch synchronization. Native macOS toolbar appearance and the System Proxy switch were verified; System Proxy was restored on and TUN left off. A real HTTPS request through macOS proxy settings succeeded after reopening the updated app. Actual privileged TUN activation was not performed.

## Expanded menu follow-up

User screenshots exposed two gaps in the first dropdown styling pass: native narrow Mode popups still elided Global/Direct, and selectors did not match Dashboard menus. Selectors now use a shared QMenu-backed ComboBox with content-sized popups and native menu styling. Mode, profile intervals, and Dashboard share menu font, rounded panels, checkmarks and arrow assets. Selection signals, cancellation, disabled options and stale model indexes are preserved.

An isolated native macOS preview verified all three expanded menus with the real application widget: Global and Direct appear in full at an 80-pixel control width, and all seven interval labels fit a 140-pixel control. Native widget renders are retained under `.cache/dropdown-preview/native-images/`. The desktop capture tool omits these separate popup windows, so the preview also recorded actual menu visibility, dimensions and action text geometry. All eight existing test suites passed; the final UI suite passed again after the long-menu scrolling guard. The rebuilt app was reopened and System Proxy restored on.

## Qt Graphs traffic follow-up

Replaced the hand-painted traffic chart with Qt Graphs 2D area series embedded through QQuickWidget. Upload and download have separate visibility controls, 1/5/15-minute windows, automatic IEC rate scaling, grid and axis labels, hover inspection, and observed-sample averages and peaks. Pause freezes the displayed history while collection continues. History is bounded by age and count; missing observations do not become invented zero samples.

All nine CTest suites passed, including the new traffic-history tests and the Home layout/QML-load checks. An isolated native macOS preview verified area fills, grid, axes, hover, pause/resume and series visibility. The installed app was then reopened and its graph rendered real core traffic successfully, with System Proxy on and TUN off. Offscreen software rendering cannot verify Qt Graphs shader-based grid lines, so native inspection was used for that check.

The build now requires Qt 6.9 or newer with QuickWidgets and Graphs. macOS installation deploys QML imports and resolves Homebrew's external relative module symlinks before relocating the runtime. The installed audit bundle launched successfully and contained no broken symlinks; this remains an unsigned development bundle.

## Traffic animation follow-up

Moved live area geometry updates to Qt Quick FrameAnimation instead of one-second sample/timer redraws. Incoming samples update cached history; statistics and expiry run separately from animation. The graph stops animation when paused, hidden or empty. The render target requests four-sample MSAA for area edges; whether it is applied depends on the graphics backend. Automatic scale contraction waits for ten seconds of sustained headroom to avoid repeated rescaling. A retained predecessor clips the segment crossing the left window boundary, preventing the filled area from snapping to the next sample when data expires; it is excluded from sample statistics.

The data-page regression verifies that geometry scrolls between incoming samples while preserving their rates, freezes on pause, performs no geometry updates while hidden, resumes from retained history and stops after clearing. Both the data-page and traffic-history suites pass.

A native Metal stress run with 914 retained synthetic points and one-second data sampling measured 60.96 FrameAnimation ticks and series commits per second over ten seconds (mean 16.4 ms, 95th-percentile 25.9 ms). Paused and hidden checks each recorded zero animation ticks, point commits and render passes after settling. Offscreen render-pass counts were higher than animation ticks and are not treated as presentation FPS. Evidence is retained in `.cache/graphs-preview/benchmark.json`. The installed app was reopened with real traffic and System Proxy restored on. Also repaired Homebrew QML metadata links in the development bundle's post-build step; the same repair already applied during installation.

## UI responsiveness follow-up

The audit found blocking OS proxy commands, core termination waits, browser discovery, YAML/script generation, and archive/file operations reachable from UI actions. System Proxy now uses a serialized background service with cached readback, pending states, rollback/error feedback and restore-last shutdown. Background status refreshes keep a previously confirmed toggle usable. Launch-at-login reads/writes run asynchronously and check atomic write results.

Core stop/restart uses process signals and a terminate/kill timer; a replacement waits for the previous child to exit. Runtime YAML and enhancement scripts run on workers with cancellation and generation guards. Each launch validates an immutable configuration snapshot. Profile/enhancement import, validation, saving and editor reads run off the GUI thread with bounded editor content. Backups wait for file writers and actual core shutdown before committing a restore. Browser discovery and launch, and log exports, also use workers.

Proxy/provider pages skip unchanged or hidden rebuilds. Connection capture continues while hidden, but rendering is coalesced; closed history stays bounded. Text filtering is debounced. Normal widget updates, model commits and short preference/index updates remain on the GUI thread; asynchronous network replies already used Qt's event loop. There is no blanket latency guarantee for arbitrary input sizes or a hung operating-system/filesystem call.

The application intercepts quit while the event loop is active, rejects new operations, cancels downloads/runtime generation, drains accepted saves and their GUI completion callbacks, restores the system proxy, and waits for core/validator exit. It then removes unused generated snapshots and exits. This avoids moving the original UI stall into shutdown.

All eleven CTest suites passed in the final full run (18.04 seconds), including injected slow proxy/browser operations with GUI-heartbeat assertions, shutdown ordering and cancellation, script cancellation, a SIGTERM-resistant core, asynchronous backups, and hidden-page rendering. The final runtime suite passed again after the restart-pending guard. Native verification of this follow-up is pending because the Mac was locked when the app-control tool was invoked.

## macOS TUN permission follow-up

The reported `configure tun interface: Connect: operation not permitted` came from a directly managed mihomo process running as the normal macOS user. clash-qt still has no privileged service installer; a TUN toggle alone does not provide that capability. Added a preflight explanation for known unprivileged managed cores whose endpoint matches the connected controller. It blocks enable requests before PATCH while allowing disabling an already enabled interface and leaving external controllers eligible for normal readback verification.

Controller and routing-control tests pass, including blocked enable with no PATCH or persisted success, allowed disable, and restored eligibility when the restriction clears. The rebuilt native app was reopened and the permission explanation verified. The preceding asynchronous shutdown completed, and System Proxy was reapplied through its pending-state path.

## Privileged service integration

Added a macOS-only native helper, administrator-authorized install/repair/removal, and Settings service controls. The helper pins root-owned copies of itself and the approved mihomo binary, authenticates the configured user over a protected Unix socket, and binds core ownership to the initiating connection. Read-only status observers do not own or interrupt that lease. The Qt client and CoreProcess backend remain asynchronous, confirm controller readiness, stream bounded logs, and wait for actual stop acknowledgement. Losing confirmation returns an explicit terminal error instead of hanging shutdown.

Service configuration forces an authenticated loopback controller and private runtime paths. HTTP(S)/inline providers are supported; local file providers and external certificate/key paths are rejected. This is a trusted-owner privileged-core model, not a sandbox for hostile configurations. The installer uses deprecated Authorization Services execution for this development build; signed release registration and Windows/Linux services remain outstanding. See [MACOS_SERVICE_PROTOCOL.md](MACOS_SERVICE_PROTOCOL.md).

The actual helper socket/process loop also runs in an unprivileged compile-time-only fixture. Tests cover observers, lease ownership, rejected mutations, framing, timeouts, process exit/reaping and disconnect cleanup. This caught and fixed an inherited SIGTERM-handler race between fork and exec. Production builds exclude fixture commands.

The user installed the service through the native administrator flow during verification; launchd and root-owned helper/core files were confirmed. The first privileged core launch exposed Foundation's escaped URL slashes (`\/`), which mihomo's YAML parser rejects. Configuration serialization now uses unescaped slashes; the failure was independently reproduced with a synthetic config and real unprivileged mihomo validation. The updated privileged helper still requires Repair Service and live TUN verification.

Final automated verification passed all fourteen CTest suites (20.59 seconds), plus real unprivileged mihomo validation of the helper's serialized configuration. The first install/repair status probe raced launchd's socket creation; the Settings card now retries that startup window for up to ten seconds before reporting an unavailable service. The bundled helper and installed helper are compared during acceptance so a stale installed copy cannot be mistaken for the tested build.

The live first service startup also needed geodata in its separate private home. The helper now seeds only missing fixed-name databases from the authorized owner's existing clash-qt cache, using bounded descriptor-based copies and rejecting symbolic links, unsafe permissions, oversized files and FIFOs. Privileged startup allows 60 seconds of silence for first-time downloads while retaining the three-minute hard limit. The frozen final build passed all fourteen suites (20.52 seconds) and real unprivileged mihomo configuration validation before the final authorized repair.

### Live macOS acceptance result

After the user approved the final Repair Service prompt, the installed helper's SHA-256 matched the frozen bundled helper (`a6d94c4767d6d59a5c4d026f2dc5d80ae4ef84e6f9711ab728a31ffe89c76825`). launchd ran the root-owned helper and its pinned mihomo child as UID 0. The seeded profile initialized in approximately 10 ms and passed controller readiness.

The native TUN switch enabled successfully; authenticated controller readback reported `enable=true`, device `utun9`. A request to `https://www.gstatic.com/generate_204` using an opener with all configured proxies bypassed returned HTTP 204, and `route -n get 8.8.8.8` selected `utun9`. Restart produced a new privileged child PID and preserved enabled TUN and HTTP 204 forwarding. Stop left only the helper running and restored the route to `en0`; starting again restored `utun9` and HTTP 204. The app was left running in service mode with TUN on and System Proxy off. Removal, crash/power-loss recovery, other macOS machines and signed distribution were not exercised.
