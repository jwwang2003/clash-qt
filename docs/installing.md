# Installing, upgrading and removing

This covers the three things people usually conflate — building, packaging and
installing — and then the part nobody writes down: getting rid of it again.

If you only want to run what you just built, you do not need any of this. Use
`make run`.

## The three steps, and what each one leaves behind

| Command | What it produces | Where |
| --- | --- | --- |
| `make build` | the application and every runtime dependency | `build/dev/clash-qt.app` |
| `make package` | a staged, self-contained bundle with Qt and the engine inside it | `build/dev/stage/clash-qt.app` |
| `make install` | the installed application | `/Applications/clash-qt.app` |

The distinction that matters: **staging is not installing**. `make package`
produces a bundle *inside the build tree*, beside the developer build. It does
not replace anything and it does not clean up after itself, so a machine that
has packaged a few times ends up with several complete applications that
Spotlight and "Open With" will happily offer you. That is why `make install`
exists, and why it removes the copy it replaces.

### Build

```sh
make doctor     # check the toolchain before anything else
make build
make run        # launch what you just built, with the staged engine
```

`make build` does not compile the engine, because compiling Go on every build
would be paid by everyone who never runs the application. `make run` depends on
it, so the first `make run` after a clean checkout takes longer.

### Build and package

```sh
make package
```

This builds, compiles the engine from the pinned submodule revision, deploys Qt
and the QML plugins into the bundle, signs it, verifies the signature, and
checks that the bundle can actually launch on the macOS version it claims. The
result is at `build/dev/stage/clash-qt.app`.

`make clean` removes the whole build tree including staged bundles. Use it when
you want the extra copies gone.

### Build, package and install

```sh
make install                         # into /Applications
make install APPDIR="$HOME/Applications"   # or somewhere you own
```

`/Applications` needs write access; if you do not have it, install into
`~/Applications` rather than reaching for `sudo`. The script refuses to replace
a running copy — a bundle replaced underneath a running process keeps working
until it next touches a resource and then fails in ways that look like
corruption. Quit it first, or pass `--force` to have the script quit it for you.

`make install` re-stages first, so it always installs what the current source
produces rather than whatever was staged last. Staging re-runs Qt's deployment
tool over every framework and QML plugin, which takes several minutes on a warm
build and longer the first time.

When you have already staged and only want to install that bundle, skip the
re-stage and copy it directly:

```sh
scripts/install.sh --from build/dev/stage/clash-qt.app --to /Applications
```

The copy uses `ditto` rather than `cp`, because a bundle's code signature is
computed over extended attributes that `cp` does not carry; a `cp`-copied
application can fail to verify even though it was signed correctly when built.
The script reports the signature state after installing, so you find out then
rather than the first time Gatekeeper refuses to open it.

## Why build artefacts used to show up as installed applications

A built `.app` and a staged `.app` are complete applications as far as macOS is
concerned, so Spotlight indexed them and LaunchServices registered them. They
appeared in Spotlight, in Launchpad and in "Open With" beside real software,
several at a time — and they outlived the directories they came from, because
deleting a folder does not unregister what was inside it. Registrations for four
long-deleted build directories were still being offered.

Configuring is enough to cause it. CMake writes the bundle's `Info.plist` during
configure, and a directory holding an `Info.plist` and an empty `MacOS/` is
already an application to Launchpad: a four-kilobyte one that cannot launch.

The build tree now carries a `.metadata_never_index` marker, written before any
target is declared, so nothing under it is indexed or registered. If you have
older entries left over from before this, `scripts/uninstall.sh --app` clears
registrations whose files no longer exist, and `make installed` lists them under
*Known to macOS* with anything missing marked `<- registered but GONE`.

## What an installation actually consists of

Four separate things, in three places with three different lifetimes. This is
the reason "drag it to the Trash" is not enough.

| Part | Where | Removed by |
| --- | --- | --- |
| The application, Qt, the engine | `/Applications/clash-qt.app` | deleting the bundle |
| Profiles, backups, enhancement scripts | `~/Library/Application Support/clash-qt` | `--data` |
| Preferences and window state | `~/Library/Preferences/*clash-qt*.plist` | `--data` |
| The privileged helper and its own copy of the engine | `/Library/PrivilegedHelperTools/org.clash-qt.service/` and `/Library/LaunchDaemons/` | `--service`, with authorization |

The last row only exists if you enabled the privileged service for TUN. It is
root-owned, it runs as a launch daemon, and **it survives deleting the
application**. A machine where the application has been thrown away can still
have a root-owned engine loaded and running.

To see exactly what is present on your machine:

```sh
make installed
```

It reads and prints; it changes nothing. It also flags bundles that are build
artefacts rather than installations, which is how you spot the extra copies.

## Upgrading

```sh
make install
```

The previous bundle is removed and replaced in one move, so there is no window
where a half-copied application sits where a working one used to be.

**The privileged helper is not upgraded with the application.** It is a separate
root-owned component installed through an authorization prompt, and replacing
the application leaves the old helper in place. If a new version expects a
different helper, open **Settings → Privileged Service** and choose **Repair**.
The installer reminds you of this when it detects a helper.

## Removing

```sh
make installed                  # look first; this changes nothing

scripts/uninstall.sh --app      # the application bundles
scripts/uninstall.sh --data     # profiles, backups, preferences  (permanent)
scripts/uninstall.sh --service  # the privileged helper           (asks for sudo)
scripts/uninstall.sh --all
```

`make uninstall` is `--app` alone: it removes installed application bundles and
leaves your data and the helper untouched, then tells you they are still there.

If you installed somewhere other than `/Applications` or `~/Applications`, add
`--dir` so it looks there too — `make installed` and `make uninstall` pass
`APPDIR` for you. Bundles inside a build tree are listed but never deleted; they
belong to `make clean`, and removing them here would take your build with them.

Two things worth knowing:

- **`--data` is not recoverable.** It holds imported profiles, enhancement
  scripts and local backups, and backups contain subscription credentials.
- **`--service` does not delete files directly.** It calls the helper's own
  uninstall, which stops the launch daemon before unlinking anything and refuses
  to remove a file that is not a root-owned regular file. Deleting those files by
  hand leaves the daemon running against images that no longer exist. If the
  helper binary is already missing, the script stops the daemon and cleans up the
  remains itself.

Preferences are cached by a system daemon, so removing the files is not enough on
its own; `--data` flushes that cache, which is why settings do not reappear on
the next launch.

## Current limitations

**The bundle is ad-hoc signed.** It is not signed with a Developer ID and not
notarized, so on any machine other than the one that built it, Gatekeeper treats
it as unidentified software. Distributing it properly is blocked on a conflict
described in [updates.md](updates.md): notarization requires signing the bundled
engine, and signing rewrites the binary that the provenance check verifies at
run time.

**Packaging refuses when the bundle cannot run where it claims.** The project
targets macOS 14, but the Qt frameworks and QML plugins deployed into the bundle
carry their own minimum, and the real floor is the highest of them. Qt from a
package manager is built for whatever macOS was current when it was built, so
with such a Qt the check will refuse to produce a bundle. That refusal is
correct: the alternative is an application that advertises macOS 14 and fails to
start on it. To package for older systems, build against a Qt whose deployment
target is at or below the version you intend to support.

To package for your own machine only, declare the version you actually have.
`MACOS_MIN` sets it, and the claim then matches what the bundle can do:

```sh
make clean
make package MACOS_MIN=26.0     # or whatever `sw_vers -productVersion` reports
make install  MACOS_MIN=26.0
```

This is not a way around the check — it is the check working. The bundle really
does require that version, and now it says so.

**There is no updater.** Installing a new version is the manual replacement
described above. The options for changing that are in [updates.md](updates.md).

## Related documents

- [build.md](build.md) — presets, individual targets, how the engine is built
- [packaging.md](packaging.md) — what the package contains and how it is signed
- [updates.md](updates.md) — shipping updates to users
