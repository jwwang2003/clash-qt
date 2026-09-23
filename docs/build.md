# Building clash-qt

From a fresh clone to a running application. Every command here is run through
the `Makefile` at the repository root, which is a thin facade over CMake
presets, the Go toolchain and CTest. Nothing in this document requires you to
read a `CMakeLists.txt`.

Related: [packaging](packaging.md) for producing a distributable bundle,
[development](development.md) for working in the tree, [testing](testing.md)
for what the test suite covers.

## What you need

| Tool | Minimum | Why |
| --- | --- | --- |
| GNU Make | 3.81 | Runs the command facade. |
| CMake | 3.21 | Configures the build and builds the engine. |
| Ninja | any | The generator every preset selects. |
| Go | recent | The mihomo engine is built from source, not downloaded. |
| Git | any | The engine source is a submodule, read at its recorded commit. |
| Qt | 6.9 | Core, Gui, Network, WebSockets, Qml, Concurrent; plus Widgets, Quick and Graphs for the desktop application. |
| yaml-cpp | any | Configuration composition. |
| C++ compiler | C++20 | |
| Python 3 | 3.x | **Tests only.** The architecture and reference checks are Python. The application itself needs no interpreter. |

GNU Make 3.81 is the supported floor because it is what macOS ships and Apple
will not ship GPLv3. Every recipe in the `Makefile` is therefore a single
`&&`-chained command: 3.81 silently ignores `.ONESHELL` and has no
`.SHELLFLAGS`, and Windows has no `/bin/sh` to fall back on. Anything portable
is delegated to CMake rather than reimplemented in Make. On Windows, use GNU
Make from an initialised MSVC/Qt environment — not NMake.

On macOS with Homebrew:

```sh
brew install qt yaml-cpp cmake ninja go
```

This document was written against macOS 26.6.2 on arm64, with Qt 6.11.1
(Homebrew), CMake 4.4.2, Ninja 1.13.2, AppleClang 17.0.0, Go 1.26.5, Git 2.50.1
and Python 3.11.16. The commands were run under GNU Make 3.81 (the macOS stock
`/usr/bin/make`); `make`, `make help` and `make doctor` were additionally run
under GNU Make 4.4.1 and produced identical output. No command in this document
is known to depend on which Make you use.

## First run

```sh
make help      # the command list
make doctor    # check prerequisites, change nothing
make setup     # initialise the recorded engine submodule
make build     # configure and build everything
make test      # build and run the portable test lanes
```

### make help

`make help` is the default goal, so a bare `make` prints it. It lists every
command, the three presets, the four variables and the prerequisite line. It is
the one place that has to stay true, so it is deliberately short.

### make doctor

`make doctor` is read-only: it installs nothing, configures no build tree you
will use, and builds nothing. On a healthy machine it prints:

```
-- Host
--   system   macOS 26.6.2 arm64
--   cmake    4.4.2
--   compiler AppleClang 17.0.0.17000603
--   make     3.81 (supported floor: GNU Make 3.81)
-- Toolchain
--   ok       go: go version go1.26.5 darwin/arm64
--   ok       git: git version 2.50.1 (Apple Git-155)
--   ok       ninja: 1.13.2
--   ok       python3: 3.11.16 (architecture tests)
-- Qt and libraries
--   ok       Qt6 base: 6.11.1  [/opt/homebrew/lib/cmake/Qt6]
--   ok       Qt6 desktop: Widgets, Quick, Graphs
--   ok       yaml-cpp [/opt/homebrew/lib/cmake/yaml-cpp]
-- Recorded sources
--   ok       3rdparty/mihomo at ab405bad (clean)
--
-- doctor: all prerequisites present.
```

Missing prerequisites are counted and reported individually as
`MISSING <what>: <detail>`, and the check then fails with
`doctor: N prerequisite(s) missing.` Two findings are notes rather than
failures, because neither stops you building the application: an absent
`python3` (the architecture tests cannot run) and absent Qt desktop modules
(backend-only builds still work — configure with `PRESET=headless`).

A third finding is a warning, and it is the most useful line this command
prints:

```
--   ok       yaml-cpp [/opt/anaconda3/lib/cmake/yaml-cpp]
--   WARNING  Qt and yaml-cpp come from different prefixes:
--            Qt       /opt/homebrew
--            yaml-cpp /opt/anaconda3
--            Mixing them can fail at link time. Set CMAKE_PREFIX_PATH.
```

That is a real configuration on the development machine, reproduced by asking
for a prefix that supplies neither library. It builds, configures cleanly, and
fails only at link.

`make doctor` reuses `build/doctor/` between runs, and a CMake cache remembers
which package directories it resolved. If you change your Qt or dependency
prefix, remove `build/doctor` before re-running, or you will be shown the
previous answer.

Three details of this check exist because of specific failures:

- **It reports which copy of each dependency it found**, not merely that one
  exists, and warns when Qt and yaml-cpp resolve to different prefixes. A
  machine can easily carry two copies of a library. On the development machine,
  Anaconda ships a yaml-cpp that shadows Homebrew's; linking it against a Qt
  from a different prefix fails at *link* time with undefined `YAML::` symbols
  and passes configure cleanly. "yaml-cpp: present" is exactly how a broken
  configuration passes a prerequisite check.
- **It is a real (tiny) CMake project under `scripts/build/doctor/`, not a
  `cmake -P` script.** `find_package(Qt6)` is not scriptable: Qt's config files
  call `add_library()`, which script mode rejects. Probing the way the real
  build probes is the only way for the check to mean anything. It enables the
  `CXX` language rather than `NONE` because `Qt6Config` pulls in `FindThreads`,
  which refuses to run without a language enabled.
- **It creates its own output directory.** It redirects into `build/doctor.log`,
  and `build/` is ignored by Git, so a fresh clone does not have one. This
  command once failed on a fresh clone for exactly that reason; earlier testing
  had masked it by creating the directory by hand. A prerequisite check that
  runs before anything else has to stand up before anything else exists.

### make setup

```sh
make setup
# setup: recorded submodules initialised.
```

This runs `git submodule update --init --recursive 3rdparty/mihomo`. It is the
explicit dependency-acquisition step; nothing else in the build will fetch
anything from the network on your behalf.

## Presets and variables

`CMakePresets.json` defines three configure presets. All three use the Ninja
generator, write to `build/<preset>/` and export `compile_commands.json`.

| Preset | Build type | Tests | Desktop application |
| --- | --- | --- | --- |
| `dev` (default) | Debug | on | on |
| `release` | Release | on | on |
| `headless` | Debug | on | **off** (`CLASH_QT_BUILD_APP=OFF`) |

`headless` exists because the backend libraries need no desktop Qt. The root
`CMakeLists.txt` requests Core, Gui, Network, WebSockets, Qml and Concurrent
unconditionally, and Widgets, Quick and Graphs only when the application is
being built. Discovering the desktop components unconditionally would make a
backend build impossible on a machine that has only the base modules.

The presets deliberately contain no local paths, so they can be checked in and
used on any machine.

Four variables adjust any command:

| Variable | Default | Purpose |
| --- | --- | --- |
| `PRESET` | `dev` | Which preset to configure and build. |
| `BUILD_DIR` | `build/$(PRESET)` | Where to put the build tree. Absolute paths work. |
| `JOBS` | `8` | Parallelism passed to `cmake --build`. |
| `CMAKE_PREFIX_PATH` | `brew --prefix` | Where to find Qt and yaml-cpp. |

`CMAKE_PREFIX_PATH` is resolved by the `Makefile`, not by the presets. Choosing
a dependency prefix is local adaptation, and local paths do not belong in a
checked-in preset. If `brew` is absent, or you want a specific Qt, set it
yourself:

```sh
make build CMAKE_PREFIX_PATH=/opt/Qt/6.9.0/macos
```

Configuring is implicit: every build command depends on
`$(BUILD_DIR)/CMakeCache.txt` and configures the preset if it is missing. Run
`make configure` explicitly only when you want to re-run configure without
building.

```sh
make configure PRESET=headless          # -> build/headless
make build     PRESET=release           # -> build/release
make build     BUILD_DIR=/tmp/scratch   # anywhere you like
```

`make clean` removes the build directory for the selected preset and nothing
else — never source, never your user data, never the submodule checkout.

## The mihomo engine

clash-qt supervises a mihomo process. That engine is **always built from
`3rdparty/mihomo` at the commit the submodule records**. There is no fallback
to a binary download, to a copy on `PATH`, or to another Clash installation on
the machine — not at build time, and not at run time.

```sh
make core
```

```
-- Building mihomo v1.19.31 (ab405bad5beeeac8b003bb01f60f134f6df54471, clean)
-- mihomo -> <build dir>/core/mihomo
--   sha256 6f53b2e18687b26c9948a804ab9e7b1b4421e8d0d3a391c11e8468dd1685b544
--   provenance <build dir>/core/mihomo-provenance.json
```

### How the pin is enforced

`cmake/Mihomo.cmake` registers a custom command whose only source is the local
checkout, and whose dependency is the **recorded submodule pointer** —
`.git/modules/3rdparty/mihomo/HEAD`, falling back to the checkout's `go.mod`
when that file is absent. Moving the submodule to a different revision
therefore rebuilds the engine instead of silently reusing the previous
revision's binary.

The build itself is `scripts/build/build_mihomo.cmake`, run in CMake script
mode. It is CMake rather than Python or a shell script because CMake is already
a hard prerequisite on every supported platform and Python is not present on a
clean Windows machine. It refuses to start if `3rdparty/mihomo/go.mod` is
missing, and tells you to run `make setup`.

If Go is not installed, the `clash-qt-core` target does not quietly do nothing:
configure adds a dependency that prints
`Go toolchain not found. The managed engine is built from source; install Go and re-run 'make doctor'.`
and fails.

### Controlled build metadata

Upstream's own Makefile derives its version from `git branch --show-current`,
which is empty in a detached submodule, and stamps the build time from
wall-clock `date`. Both are replaced with controlled values, so two builds of
the same commit produce the same bytes:

- `-tags with_gvisor`, `CGO_ENABLED=0`, `-trimpath`, `-mod=readonly`.
- `-ldflags` pins `constant.Version` to the tag (or the commit, when the
  checkout has no tag) and `constant.BuildTime` to the literal `source-build`,
  and strips symbols and the build id.
- `-buildvcs=false`. Without it Go walks *up* out of the submodule and stamps
  the **superproject's** revision and dirty state into the engine — recording
  clash-qt's commit where mihomo's belongs, contradicting the provenance
  manifest, and changing the artifact hash on every unrelated commit to this
  repository.
- When cross-building for `amd64`, `GOAMD64=v1` is pinned. Several of
  upstream's generic amd64 targets select `v3`; `v1` is the broad desktop
  baseline.

Building the same commit into two different build directories produces byte
-identical output: both runs recorded sha256
`6f53b2e1…85b544` for mihomo v1.19.31 at `ab405bad`.

### Provenance

Every engine build writes `mihomo-provenance.json` beside the binary:

```json
{
  "source_path": "3rdparty/mihomo",
  "source_commit": "ab405bad5beeeac8b003bb01f60f134f6df54471",
  "source_describe": "v1.19.31",
  "source_state": "clean",
  "go_toolchain": "go version go1.26.5 darwin/arm64",
  "build_tags": "with_gvisor",
  "build_env": ["CGO_ENABLED=0"],
  "artifact": "…/core/mihomo",
  "artifact_bytes": 56406850,
  "artifact_sha256": "6f53b2e18687b26c9948a804ab9e7b1b4421e8d0d3a391c11e8468dd1685b544"
}
```

A **release** build refuses to proceed when the submodule checkout has
uncommitted changes. A developer build proceeds and records `"source_state":
"dirty"`, with a warning that the build is not reproducible. Developer builds
may carry local engine edits; releases may not.

Where the manifest ends up in a distributable bundle, and how the application
checks it, is in [packaging](packaging.md).

## The loadable engine module

The engine supervisor ships as a separate shared library, not as code linked
into the executable, so that what is packaged is exercised as a real module.

```sh
make module
```

builds `clash_qt_backend_module` into `$(BUILD_DIR)/modules/` and then builds
the engine it supervises. `make build` builds it too, and the application's
post-build step copies it into the bundle. The boundary it implements is
described in [module-api](module-api.md); an independent consumer that links
only the public headers lives in `examples/component_consumer/README.md`.

## Building and running

```sh
make build     # everything: libraries, module, application, tests
make run       # build if necessary, then launch
```

`make run` launches the application with `CLASH_QT_CORE_BINARY` pointing at
`$(BUILD_DIR)/core/mihomo`. On start-up it prints the module it loaded:

```
Engine component loaded: …/clash-qt.app/Contents/Frameworks/libclash_qt_backend_module.dylib
```

`make build` does **not** build the engine — `clash-qt-core` is deliberately
outside the `all` target, so an ordinary edit-build cycle does not re-run a Go
build. Run `make core` once (or `make run` after `make core`) to have an engine
to supervise. `make package` and `make test-integration` both depend on it
explicitly.

### How the engine is resolved at run time

`src/core/mihomo/process/engine_discovery.h` resolves the *managed* engine in
exactly two places, in order:

1. beside the application executable (where packaging stages it), then
2. `$CLASH_QT_CORE_BINARY` (this project's own build output, which `make run`
   exports).

Nothing else is ever resolved automatically. An executable named `mihomo` on
`PATH`, or another Clash installation on the machine, is *offered* in Settings
as an explicit user choice and reported as such — never silently adopted. This
rule exists because it was once broken: discovery fell through to
`QStandardPaths::findExecutable("mihomo")` and then to a hardcoded Clash Verge
path, so on a machine with Clash Verge installed the application supervised
*that* binary while reporting its own provenance.

When nothing resolves, the application says so and tells you to run `make core`
or set `CLASH_QT_CORE_BINARY`.

### Where the executable is

On macOS the build produces `build/<preset>/clash-qt.app`. A symlink at
`build/<preset>/clash-qt` points into it
(`clash-qt.app/Contents/MacOS/clash-qt`), so the plain path keeps working and
is refreshed on every build. On Windows and Linux, the executable sits directly
in the build directory.

Two command-line options matter during development:

```sh
build/dev/clash-qt --data-dir /tmp/clash-qt-session --no-autostart
```

`--data-dir` isolates profiles, enhancements, runtime overrides and preferences
(it sets `CLASH_QT_DATA_DIR`). Two instances cannot share one data directory.
`--no-autostart` stops the selected profile's core from starting on launch.
Use both when you do not want a development run touching your real settings.

## Running the tests

```sh
make test              # the routine lane
make test-integration  # real component and engine workflows
make test-native       # opt-in; needs a disposable host
```

- **`make test`** builds everything and runs CTest with
  `--label-exclude "native|privileged|benchmark|real-core"`. This is the lane
  that must be green before every commit. On the reference machine it completes
  in about three minutes.
- **`make test-integration`** builds the engine first, then runs
  `--label-regex "real-core|integration"`. The `real-core` entries drive the
  locally built mihomo and fail — rather than skip — if the engine or its
  provenance is missing, so the lane cannot pass vacuously.
- **`make test-native`** refuses to run until you set `CLASH_QT_NATIVE_HOST=1`.
  It prints why and exits non-zero:

  ```
  test-native runs privileged and network tests that require an exclusive,
  disposable host. Set CLASH_QT_NATIVE_HOST=1 to confirm this is not your
  working machine, then re-run.
  ```

  There are currently no registered entries in that lane. The label is reserved
  so a future native test cannot be mistaken for one that already passed.

Both `--no-tests=error` and `--output-on-failure` are always passed: an empty
selection is a failure, not a pass.

The journey suites are serialised because each drives a real core over a real
loopback controller. The profile store writes the controller port into every
generated configuration, so a journey cannot pick one per request; it reads
`CLASH_QT_CONTROLLER_PORT` instead, and the registered suites claim ports of
their own through it. Unset, the store uses the shipped default `29097`, so
nothing about a normal run changes.

A journey that cannot bind its port **fails**, and says which port and which
process holds it. It does not skip. It used to: a held port made four journeys
report success in about a tenth of a second each, having run nothing, because a
suite that skips every case still exits zero and CTest scores that as a pass.
That is the worst failure a test lane can have — this is the only lane that sees
a component which builds and links but that nothing wires up, and it was
silently proving nothing whenever the port was busy.

Every registered test gets its own `CLASH_QT_DATA_DIR`, and every widget suite
runs offscreen. See [testing](testing.md) and `tests/README.md` for the full
index of entries, labels and what each one is evidence for.

Use a build directory of your own if you want to run tests without disturbing
your working tree:

```sh
make test BUILD_DIR=build/local
```

## Troubleshooting

**Undefined `YAML::` symbols at link time.** Two copies of yaml-cpp, resolved
from a different prefix than Qt. `make doctor` warns about this by name. Fix it
with `CMAKE_PREFIX_PATH=/opt/homebrew` (or wherever your Qt lives).

**`mihomo source not found … The submodule is not initialised. Run: make setup`.**
Exactly that.

**A release build refuses with "uncommitted changes".** The engine submodule is
dirty. Commit or discard the edits, or build a developer preset instead.

**`make doctor` reports Qt desktop modules absent.** You can still build and
test the backend: `make build PRESET=headless`.

**Ninja says nothing to do but the application is stale.** The engine is not in
`all`. Run `make core`.
