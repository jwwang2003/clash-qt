# Packaging clash-qt

Producing a distributable bundle from a build tree: what `make package` stages,
in what order it signs, what it verifies, and what provenance travels with the
engine.

Prerequisites and the build commands themselves are in [build](build.md).

## The command

```sh
make package                       # -> build/dev/stage
make package PRESET=release        # -> build/release/stage
```

```
…
-- Running Qt deploy tool for clash-qt.app in working directory 'build/dev/stage'
build/dev/stage/clash-qt.app: replacing existing signature
-- Installing: …/stage/clash-qt.app/Contents/MacOS/mihomo
-- Installing: …/stage/clash-qt.app/Contents/Resources/mihomo-provenance.json
build/dev/stage/clash-qt.app: replacing existing signature
package: staged in build/dev/stage
```

The target is `build core` followed by
`cmake --install $(BUILD_DIR) --prefix $(BUILD_DIR)/stage`.

It depends on `core` as well as `build` **on purpose**. The `clash-qt-core`
target is deliberately not part of `all`, so that ordinary rebuilds do not
re-run a Go build. Without the explicit dependency, a package built from a tree
that had never run `make core` would install successfully and ship with no
managed engine at all.

You can stage anywhere:

```sh
make package BUILD_DIR=build/local
cmake --install build/local --prefix /somewhere/else   # same result, no facade
```

## What lands in the bundle (macOS)

```
clash-qt.app/
  Contents/
    Info.plist
    _CodeSignature/
    MacOS/
      clash-qt                        the application
      mihomo                          the engine built from 3rdparty/mihomo
    Frameworks/
      libclash_qt_backend_module.dylib the engine supervisor module
      Qt*.framework, lib*.dylib        Qt and its transitive dependencies
    Helpers/
      clash-qt-service-helper          the privileged helper
    PlugIns/                           Qt platform, image and QML plugins
    Resources/
      qml/                             deployed QML imports
      mihomo-provenance.json           manifest for the engine above
```

Three of those are placed by rules worth knowing about.

**The engine goes beside the application executable.** `Contents/MacOS/mihomo`
is the first location the application's engine discovery checks, so nothing at
run time had to change to find it. Its manifest goes to `Contents/Resources/`,
because a bundle's `MacOS` directory is for executables; discovery looks in
both places.

**The module is copied into `Contents/Frameworks/` before Qt deployment.** That
ordering is what lets Qt resolve the module's own runtime dependencies inside
the bundle, and that copied location is the one the loader uses. The copy is
also wired as a `LINK_DEPENDS` of the application target, not merely as a build
-order dependency: a build-order edge alone would leave an older copied module
sitting beside an unchanged executable.

**The helper reaches the bundle only through a post-build copy.** There is no
`install(TARGETS)` rule for it. `privileged_service_installer.cpp` resolves it
bundle-relatively as `../Helpers/clash-qt-service-helper`, so the copy
destination and that string are one mechanism in two places.

On Windows and Linux the same install rules place the module and the engine in
the binary directory, the provenance manifest under the data directory, and —
on Linux — a `.desktop` entry and a scalable icon.

## Signing order

macOS signing here is **ad-hoc** (`codesign --sign -`). It needs no
certificate, no keychain and no trust-store change, and it is what makes a
locally built bundle launchable and verifiable. It is not a release signature;
see *What this is not* below.

The order is owned deliberately, because two tools both want to write into the
bundle and one of them must not touch the engine:

1. **Post-build.** The module is copied into `Contents/Frameworks/` and the
   helper into `Contents/Helpers/`. QML metadata symlinks are materialised
   (below), and the development bundle is signed with `--force --deep --sign -`
   so it is runnable straight out of the build tree.
2. **Qt deployment (install time).** Any *older* staged engine is deleted
   first, then QML imports are deployed, symlinks materialised again in the
   staged copy, and `macdeployqt` runs over the application **and explicitly
   over the module** — with `-no-codesign`. Qt is not allowed to sign anything;
   signing belongs to one place in a known order. Qt's tool then signs the
   result via the explicit `--force --deep --sign -` at the end of that step.
3. **Engine install.** Only now is `mihomo` copied to `Contents/MacOS/` and its
   manifest to `Contents/Resources/`. This is after Qt deployment on purpose:
   `macdeployqt` rewrites and signs what it finds in a bundle, and the engine is
   a self-contained Go binary that must not be processed as a Qt executable.
4. **Reseal.** `codesign --force --sign -` over the completed bundle —
   **without** `--deep`. That re-signs the main executable and re-seals the
   resource hashes, which is what the newly added engine and manifest require,
   while leaving the already-signed nested binaries byte-for-byte alone.
5. **Verify.** `codesign --verify --deep --strict`, with
   `COMMAND_ERROR_IS_FATAL ANY`. A verification failure fails `make package`.
   A packaging step that only logs an error produces a broken bundle and a
   green build.

The reason step 4 drops `--deep` is the reason the whole order exists: a deep
re-sign at that point would rewrite the pinned engine's signature, changing its
bytes and breaking the one thing the manifest asserts about it.

### Verified on a staged bundle

```sh
$ shasum -a 256 build/dev/stage/clash-qt.app/Contents/MacOS/mihomo
6f53b2e18687b26c9948a804ab9e7b1b4421e8d0d3a391c11e8468dd1685b544

$ python3 -c "import json;print(json.load(open('build/dev/stage/clash-qt.app/Contents/Resources/mihomo-provenance.json'))['artifact_sha256'])"
6f53b2e18687b26c9948a804ab9e7b1b4421e8d0d3a391c11e8468dd1685b544

$ codesign --verify --deep --strict --verbose=2 build/dev/stage/clash-qt.app
build/dev/stage/clash-qt.app: valid on disk
build/dev/stage/clash-qt.app: satisfies its Designated Requirement
```

The staged engine still carries Go's own linker-produced ad-hoc signature
(`flags=0x20002(adhoc,linker-signed)`), not one written by `codesign`, while
the module and the helper each carry their own ad-hoc signature with their own
identifier, and `--verify --deep` validates all of them as nested code. That is
the whole claim of the signing order, checked rather than asserted.

### Qt, Homebrew and QML symlinks

Qt's QML installer preserves file symlinks, and Homebrew's are relative links
into the Cellar. `macdeployqt` cannot inspect and relocate a binary it reaches
through one, so `cmake/ResolveQmlLinks.cmake` materialises them into real files
before deployment. It runs against the **development** bundle as well as the
staged one — the development bundle also receives QML imports at post-build
time, and repairing only the installed copy would leave the tree you actually
run broken. When a deployed plugin symlink cannot be resolved to exactly one
source, it is a fatal error rather than a silently skipped file.

Modular Qt installations keep plugin frameworks outside QtCore's prefix, so the
deploy tool is given an explicit `-libpath` for the Qt library directory.

## Provenance

The engine that ships is the exact build output described in
`mihomo-provenance.json`: source path, commit, `git describe`, clean/dirty
state, Go toolchain, build tags, build environment, byte size and sha256. How
that file is produced, and why the build metadata is controlled rather than
taken from upstream's Makefile, is in [build](build.md).

The application does not trust the manifest by position. `engineProvenance()`
in `src/core/mihomo/process/engine_discovery.h` hashes the binary it is about to
report on and compares it with `artifact_sha256`. On a mismatch it reports
`no recorded provenance (sha256 … does not match the manifest beside it)`
rather than the manifest's contents. A stale manifest cannot lend its
provenance to a different binary, and a packaging step that rewrote the engine
would be visible in the user interface rather than silent.

For a release, the same three facts should be published alongside the artifact:
the engine's source commit and sha256, the application's own revision, and the
Qt version deployed into the bundle.

## The component SDK

The public component headers install separately, under the `component-sdk`
install component:

```sh
cmake --install build/dev --component component-sdk --prefix /somewhere
# -> /somewhere/include/clash-qt/core/component/*.h
```

That header set is what an independent consumer compiles against without
linking the Qt-facing backend or the private engine implementation. A working
example, including how to point it at an installed header set, is in
`examples/component_consumer/README.md`. The boundary itself is described in
[module-api](module-api.md).

## What this is not

Current qualification is **macOS arm64** only. Windows and Linux have
conditional code and registered tests but no compile or run evidence; that is
not support.

The bundle is a **developer artifact**:

- Signatures are ad-hoc. There is no Developer ID signing, no notarization and
  no stapling. A bundle produced here will be treated as unidentified by
  Gatekeeper on another machine.
- There is no DMG, no installer, no ZIP and no auto-update channel.
- There is no macOS universal binary. Do not advertise one until every embedded
  component — the Qt frameworks and the Go engine included — is built for both
  slices and tested on both.
- The privileged helper is installed at run time through Apple's deprecated
  Authorization Services execution API. Migration to modern service
  registration is separate distribution work.

Signing secrets for a real release belong in CI credential storage, never in
the tree. An unsigned developer package can be tested before those credentials
exist; it cannot stand in for a signed-release gate.

## Checking a stage by hand

```sh
codesign --verify --deep --strict --verbose=2 build/dev/stage/clash-qt.app
shasum -a 256 build/dev/stage/clash-qt.app/Contents/MacOS/mihomo
build/dev/stage/clash-qt.app/Contents/MacOS/mihomo -v
```

The strongest automated check is the `app-smoke` test, which launches the
shipped executable and exercises module selection, isolated helper and
controller routing, single-instance behaviour and data-directory precedence. It
runs in the routine lane; see [testing](testing.md).

Anything that must be true of a *relocated* install — no build-tree
environment, no developer SDK paths — has to be tested from a copy outside the
build directory. That is not yet automated.
