# clash-qt

A Qt 6 / C++ desktop manager for the [mihomo](https://github.com/MetaCubeX/mihomo)
proxy core. The app uses Qt Widgets, with a native Qt Graphs 2D area graph
embedded through QQuickView and a native window container. There is no WebView.
Profile enhancement scripts run in Qt's JavaScript engine.

Clash Verge Rev is the feature reference. **This is an independent client with
partial feature parity, not a complete replacement.** Implemented workflows and
remaining gaps are listed below. [qClash](https://github.com/josephpei/qClash) is
an additional native Qt reference.

## Build and test

Requires Qt 6.9+ with Widgets, Network, WebSockets, Qml, Quick, Graphs and
Concurrent, CMake 3.21+, a C++20 compiler, yaml-cpp and Go (to build the proxy
engine from source).

```sh
brew install qt yaml-cpp cmake ninja go
make doctor     # check the toolchain before building anything
make build      # the application and every runtime dependency
make test       # the portable feature, architecture and UI suites
make run        # launch what you just built
```

`make` on its own prints the full list of commands, which is also what `make
help` does. The engine is compiled from the `3rdparty/mihomo` submodule at
a pinned revision and recorded in a provenance file beside the binary; it is
never downloaded at build time and never taken from `PATH`. See
[docs/build.md](docs/build.md) for the presets, the individual targets and how to
build without Qt Test, and [docs/packaging.md](docs/packaging.md) for producing a
signed bundle.

### Display refresh rate

The traffic graph uses a native `QQuickView` in a widget window container, with
vsync enabled. On macOS, Qt Quick uses Metal and its threaded render loop by
default, allowing animation to follow a 120 Hz display without a fixed FPS timer.
Scrolling uses elapsed time, so its speed stays the same on other refresh rates.
Statistics update separately every 250 ms; paused, hidden, or empty graphs stop
animating. The rest of the Qt Widgets interface continues to repaint on demand.

To measure the graph on a native macOS display using synthetic traffic:

```sh
QT_QPA_PLATFORM=cocoa QSG_INFO=1 CLASH_QT_MEASURE_FRAMES=1 \
  ./build/dev/tests/traffic-frame-pacing-tests trafficNativeFrameTiming
```

Keep the test window visible. The output reports the render loop, graphics API,
screen refresh rate, and five seconds of `frameSwapped` timing (FPS, mean, median,
and p95 interval). At 120 Hz the target mean is about 8.33 ms. This measures frame
submissions, not physical scanout; ProMotion, display settings, power policy, and
system load can change the result. Headless CTest checks behavior, not refresh rate.
For a display-move check, repeat on each monitor or move the test window while it
runs. No refresh rate is cached or hardcoded in the application.

The native GPU regression test below checks successive frames for colored fill
above zero-traffic segments. It covers a Qt Graphs path-closing issue that could
produce flashing diagonal bands when consecutive samples were zero. Render-only
baseline coordinates avoid the issue; recorded rates and readouts remain exact.

```sh
QT_QPA_PLATFORM=cocoa CLASH_QT_VERIFY_GRAPH_FRAMES=1 \
  ./build/dev/tests/traffic-graph-frames-tests trafficNativeFillStaysBelowOutline
```

## Running

Import a local YAML file or HTTP(S) subscription, select it, then choose
**Start Core**. Settings can select the mihomo executable and configure whether
the selected profile starts when the app opens. Executable discovery checks the
application directory, PATH and a local Clash Verge Rev installation.

For an existing core, discovery uses `CLASH_QT_CONTROLLER` (`host:port`) and
`CLASH_QT_SECRET`, then Verge Rev's configuration, then `127.0.0.1:9090`.
The status bar distinguishes an external core from a managed process.

A managed core uses controller `127.0.0.1:29097` with a generated secret and
mixed proxy port `27890` by default. Runtime Settings can override the proxy
port and other mihomo settings. Selecting, editing or updating the active
profile regenerates the managed runtime. Enabled enhancements are applied
before runtime overrides and application-owned controller settings.

**System Proxy** is an explicit control. Settings are captured before this app
changes them and restored on normal shutdown or managed-core stop when they
still match this app's changes. An unrelated application's proxy settings are
not cleared merely because clash-qt exits. Process crashes/power loss and native
platform privilege behavior still require further recovery validation.

On macOS, stop the managed core and open **Settings → Privileged Service**.
Install Service requests administrator authorization and installs a root-owned
copy of the selected mihomo binary. It enables service mode for the next start;
start the core, then enable TUN. Repair updates the pinned core copy, and Remove
uninstalls the service. The service trusts your account and subscription content;
its controller credential grants access to a privileged core. Local file providers
and external certificate/key files are not supported in this first integration.

The development installer uses Apple's deprecated Authorization Services execution
API. Signed/notarized release packaging and migration to modern service registration
remain separate distribution work. Administrator installation/repair, privileged
startup, TUN forwarding, restart persistence and stop-time route restoration were
verified on the development Mac. Removal and other machines still need acceptance
validation; unit/mock tests alone do not establish privileged networking.

For an isolated test session:

```sh
CLASH_QT_CONTROLLER=127.0.0.1:29097 \
  build/dev/clash-qt.app/Contents/MacOS/clash-qt \
  --data-dir /tmp/clash-qt-test --no-autostart
```

`--data-dir` isolates profiles, enhancements, runtime overrides and app
preferences. Multiple instances cannot share the same data directory.

## Features

- Background OS proxy/login-item operations, configuration generation, file and backup tasks, with pending/error feedback and asynchronous core shutdown.
- Overview with Qt Graphs 2D upload/download area plots, 1/5/15-minute history,
  automatic IEC rate scaling, hover details, pause and average/peak statistics.
- Memory, connection totals and core DNS diagnostics.
- Profile import/create/edit, subscriptions, quota/expiry and scheduled refresh.
- Proxy selection, filtering, sorting and node/group latency tests.
- Rules and proxy/rule provider inspection, refresh and health checks.
- Connection inspection, rates, bounded closed history, details and controls.
- Bounded, searchable live/core logs with pause and export.
- Global YAML/JavaScript enhancement chain with in-app editing and script timeout.
- Toolbar and tray switches for System Proxy and TUN, with confirmed core state
  and rollback after rejected changes. macOS Settings includes privileged service
  installation/repair/removal and service-backed core lifecycle for TUN.
- Runtime configuration overrides, login startup and hotkeys.
- Local backup/export/import/restore and manual WebDAV upload/download.
- Tray node/profile/mode controls, POSIX/PowerShell proxy commands, browser dashboard,
  persistent geometry and native theme support.
- In-place subscription URL editing and core-managed GEO database updates.

Backups contain subscription credentials and scripts and are not encrypted.
Restore only trusted archives. WebDAV passwords are used for the session and
are not included in backups.

## Major remaining gaps

Windows/Linux privileged service integration, app/core updater, PAC/proxy
guard, full Verge per-profile enhancement/sequence compatibility, service
availability/region tests, scheduled/remote-history backups, translations and
release packaging remain incomplete. Windows/Linux and privileged networking
need native validation before claiming support equivalent to Verge Rev.

## Layout

- `src/core/`: the portable centre, with no dependency on the application or the
  UI. `component/` is the object model modules are built on, `backend/` is the
  published contract the application codes against, `mihomo/` implements it over
  the proxy engine, and `config/`, `profiles/`, `backups/` and `telemetry/` each
  own one responsibility.
- `src/integrations/`: the glue that carries the contract across the module
  boundary, including marshalling between the process and the loaded module.
- `src/app/`: composition and coordination — the context the root assembles,
  plus `runtime/`, `lifecycle/`, `backup/` and `composition/`.
- `src/platform/`: OS adapters — `browser/`, `proxy/`, `service/` and `system/`.
- `src/services/macos/`: the standalone privileged helper executable.
- `src/ui/`: `shell/` (main window, tray, toolbar), `pages/<feature>/` (each page
  beside its own implementation), `widgets/`, `theme/` and `resources/`.
- `tests/`: one directory per lane — `contracts/`, `workflows/`, `architecture/`,
  `app/`, `core/`, `platform/`, `ui/` and `benchmarks/`, with `support/` and
  `fixtures/` shared between them.

The dependency direction is one-way: core knows nothing of the application, and
the application knows the backend only through its published headers. That rule
is enforced by a check, not by convention — see
[docs/architecture.md](docs/architecture.md) for the boundaries and
[docs/module-api.md](docs/module-api.md) for the contract itself.
[docs/development.md](docs/development.md) covers working in the tree and
[docs/testing.md](docs/testing.md) covers what each test lane proves.
