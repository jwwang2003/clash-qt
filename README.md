# clash-qt

A Qt 6 / C++ desktop client for the [mihomo](https://github.com/MetaCubeX/mihomo)
(Clash.Meta) proxy core. Pure C++ — Qt Widgets for every view, no QML, no
JavaScript, no web runtime.

Feature reference is [Clash Verge Rev](https://github.com/clash-verge-rev/clash-verge-rev);
this is an independent implementation, not a fork.

## Why

Verge Rev is a Tauri app: ~47k lines of React/MUI over ~36k lines of Rust, and it
ships a WebView. mihomo does the actual proxying in both cases — the app around
it is a manager. This is that manager, in C++.

## Build

Requires Qt 6.5+, CMake 3.21+, and yaml-cpp.

```sh
brew install qt yaml-cpp cmake ninja          # macOS
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt
cmake --build build
./build/clash-qt
```

## Running against a core

On startup the app looks for a mihomo external controller in this order:

1. `CLASH_QT_CONTROLLER` / `CLASH_QT_SECRET` environment variables
2. Clash Verge Rev's generated `config.yaml`, if that app is installed
3. `127.0.0.1:9090`

Step 2 means that with Verge Rev installed and running, clash-qt attaches to the
already-running core and works immediately — useful before stage 2 lands.

## Layout

```
src/
├── core/       mihomo client, config, profiles, enhancement pipeline
├── platform/   system proxy, autostart, global hotkeys
└── ui/         tray, main window, pages
```

Each module owns its own `sources.cmake`, so modules can be built and worked on
independently.

`src/core/mihomo_client.h` and `src/core/types.h` are the contract between core
and ui. Extend them; do not reshape them.

## Roadmap

| Stage | Scope | State |
|---|---|---|
| 0 | Walking skeleton: endpoint discovery, proxy groups, node switching, traffic stream, tray | done |
| 1 | Read-only parity: connections, logs, rules, mode switching, memory | done |
| 2 | Core lifecycle (spawn/stop/restart mihomo) + profile store and subscriptions | done |
| 3 | Config pipeline: merge chains + `QJSEngine` script enhancement | done |
| 4 | Platform: system proxy, autostart, global hotkeys (PAC deferred) | done |
| 5 | WebDAV backup, updater | planned |

### Notes on the hard parts

**TUN without sudo** is not solved in the GUI. It needs a privileged helper.
Options, in preference order: talk to the existing `clash-verge-service` over its
socket/named pipe; `setcap cap_net_admin` on the mihomo binary (Linux) or a
launchd helper (macOS); or write a small helper of our own.

**Global hotkeys** have no Qt API. Either [QHotkey](https://github.com/Skycoder42/QHotkey)
or per-platform `RegisterHotKey` / `XGrabKey` / `RegisterEventHotKey`.

**Profile enhancement scripts** run on `QJSEngine` (the Qt6::Qml module),
replacing Verge Rev's `boa_engine`. The exposed surface there is one global
log callback and a JSON round-trip, which maps over directly.
