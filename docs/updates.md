# Shipping updates to clash-qt

Whether an in-app updater is possible for this application, what it can and
cannot reach, which framework to use, and what has to be true before the feed
goes live.

Related: [packaging](packaging.md) for what the bundle contains and how it is
signed today, [build](build.md) for the engine pin and its provenance manifest,
[service-protocol](service-protocol.md) for the privileged helper this
application installs.

Facts about external projects were checked on 2026-09-23 and carry their
version and date. Things that could not be checked are listed at the end
rather than smoothed over.

## The short answer

Yes, an in-app updater is possible, and Sparkle is the framework for it. But
clash-qt is not one program; it is three things with three different update
stories, and only one of them can be replaced without asking the user for
anything.

| What ships | Where it lives after installation | Can an updater replace it silently? |
| --- | --- | --- |
| The application, Qt, the engine supervisor module, the bundled engine, the bundled helper | inside `clash-qt.app` | Yes, when the bundle is in a location the user can write |
| The installed privileged helper | `/Library/PrivilegedHelperTools/org.clash-qt.service/helper` | No. Root-owned; needs authorization |
| The root copy of the engine | `/Library/PrivilegedHelperTools/org.clash-qt.service/mihomo` | No. Same |
| The LaunchDaemon | `/Library/LaunchDaemons/org.clash-qt.service.plist` | No. Same |

Replacing a bundle is ordinary file work. Replacing anything the installer put
under `/Library` is a privileged operation, and macOS will ask. No updater
framework changes that; the only thing that changes it is not putting files
there — see *Making the helper updatable* below.

Two things must be fixed before an updater is even worth wiring up, and both
are prerequisites for a signed release with or without one:

1. The bundle is **ad-hoc signed**, so it cannot be notarized and Gatekeeper
   treats it as unidentified on any other machine. [packaging](packaging.md)
   states this plainly.
2. Signing the engine with a real identity **breaks the provenance check**.
   That conflict is the largest single obstacle in this repository and is
   described in its own section.

## What is in the tree today

Nothing. A search for update, upgrade, appcast and sparkle across `src/` finds
only the subscription refresh timer in `src/core/profiles/profile_store.cpp`
(`autoUpdate_`, a sixty-second tick that re-fetches due profiles) and ordinary
`updateState` / `updateRouting` lambdas in the user interface. There is no
updater class, no setting, no feed URL, and no release channel.

That matches what the published documents already say. `README.md` lists
"app/core updater" among the major remaining gaps, and
[packaging](packaging.md) says "There is no DMG, no installer, no ZIP and no
auto-update channel."

Three more absences matter for the work below:

- There is no `Info.plist` template. Bundle keys come only from the
  `MACOSX_BUNDLE_*` target properties set in `CMakeLists.txt`. Sparkle needs
  keys CMake has no property for.
- There is no `CMAKE_OSX_DEPLOYMENT_TARGET` anywhere, so the minimum macOS the
  bundle claims is whatever the SDK defaults to rather than a decision.
- There is no continuous integration, no git remote and no tag in the working
  tree. Every release step described here is currently a thing a person would
  run by hand.

And one duplication becomes a correctness bug the moment an updater exists.
The version string is written twice:

```
CMakeLists.txt:72   MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
src/main.cpp:91     QApplication::setApplicationVersion("0.1.0")
```

Sparkle compares the feed against the `Info.plist` value while the interface
shows the other. If they ever diverge the application will offer an update it
has already installed, or hide one it has not. Give `project()` a `VERSION` and
derive both from it before anything else here is built.

## The three parts, and what each one needs

### The bundle

This is the easy part and it is genuinely easy. Everything in
[packaging](packaging.md)'s bundle layout — the executable, the Qt frameworks,
the QML imports, `Contents/Frameworks/libclash_qt_backend_module.dylib`,
`Contents/MacOS/mihomo`, `Contents/Resources/mihomo-provenance.json` and
`Contents/Helpers/clash-qt-service-helper` — travels as one directory. An
updater downloads a new bundle, verifies it, swaps it into place and relaunches.
Nothing about the module boundary, the engine or the helper makes that harder
than it is for any other application.

The one qualification is the installed location. An application dragged into
`/Applications` by its owner is owned by that user and can be replaced without
a prompt. An application installed by an administrator for all users can end up
root-owned, and then even a plain bundle replacement raises an authorization
dialog. Sparkle's documentation is explicit that authorization "can also be
required for regular updates on certain systems (e.g. updating a bundle in
/Applications on standard user accounts or managed systems)". This is a
property of the user's machine, not of the framework.

### The engine

The engine ships inside the bundle, so it updates with the bundle. What does
not update with the bundle is the **root-owned copy** the privileged installer
made at `/Library/PrivilegedHelperTools/org.clash-qt.service/mihomo`. When
service mode is in use, that copy is the one carrying traffic. A new bundle
with a newer engine does not change it.

### The privileged helper

This is the part that does not update, and the current behaviour is worse than
merely not updating: nothing notices.

`inspectInstallation()` in
`src/platform/service/privileged_service_installer.cpp` checks that the helper,
the root core copy and the LaunchDaemon plist exist, are regular files, are
owned by uid 0, are executable, and carry no group or world write and no set-ID
bits. It does not compare any of them with what is in the bundle. After an
update the application will report the service as installed, and the user will
keep talking to the previous release's helper.

The only guard that exists is a protocol-version mismatch. The client refuses a
response whose `protocol` is not 1 and says "The privileged service protocol is
incompatible. Repair the service using this app version."
(`src/platform/service/privileged_service_client.cpp`). That catches a
deliberate wire-format break and nothing else. A helper change that fixes a
routing bug, tightens the configuration filter or corrects a privilege check
without touching the protocol integer is invisible to this test, and the user
keeps running the old one.

[service-protocol](service-protocol.md) already records the underlying fact —
"Rebuilding the app does not replace an already-installed privileged helper;
installation must be repeated through its explicit administrator-authorized UI
action to deploy a helper fix." An updater turns that sentence from a
development inconvenience into a shipped defect, because now the bundle changes
on users' machines without anyone present to remember it.

**This needs fixing before an updater ships, not after.** The cheapest fix is
identity, not mechanism: have the helper report what it is, and have the
application compare. The `status` response already carries `protocol`; adding
an opaque build identity beside it — the helper's own sha256, or the cdhash of
its code signature — lets the settings page compare the running helper with
`Contents/Helpers/clash-qt-service-helper` and say "Repair Service" when they
differ. That turns a silent mismatch into one visible, explicable
administrator prompt after an update that changed the helper, and no prompt at
all after one that did not.

### Making the helper updatable

If the goal is an update that does not prompt at all, the helper has to stop
living outside the bundle. macOS 13 introduced `SMAppService`, which registers
a LaunchDaemon whose plist and executable stay **inside the application
bundle**, in place of the `SMJobBless` and `AuthorizationExecuteWithPrivileges`
approaches. The user approves the background item once; the daemon then runs
from inside the bundle, so replacing the bundle replaces the daemon's code.

That is the structural answer to the helper question, and it is work this
project already intends to do for other reasons: both `README.md` and
[packaging](packaging.md) name migration off the deprecated Authorization
Services execution API as pending distribution work. It requires Developer ID
signing, a macOS 13 floor, and a rewrite of the install path in
`src/platform/service/`. It is not v0.1.0 work.

What could not be verified: whether an already-registered `SMAppService`
daemon picks up a replaced bundle automatically or needs re-registration after
an update. Apple's own documentation page did not render for automated
retrieval. Check that before committing to the design.

## The signature, and the problem it creates here

Everything below assumes a Developer ID signing identity, because without one
there is no useful update channel. macOS 15 removed the Control-click Gatekeeper
bypass; a user who downloads an unidentified application must now go to System
Settings, Privacy & Security, and choose Open Anyway. That is a tolerable
one-time cost for a preview. It is not tolerable on every automatic update.

Notarization, which is what removes that dialog, requires a Developer ID
certificate (Apple Developer Program, USD 99 per membership year), the hardened
runtime, a secure timestamp, **every** executable in the bundle signed with the
same identity, and a stapled ticket.

"Every executable" is where this repository has a genuine conflict.

`make package` today reseals the finished bundle with `codesign --force
--sign -` and deliberately **without** `--deep`, precisely so the pinned engine
keeps the bytes the provenance manifest describes. [packaging](packaging.md)
spells out why: "a deep re-sign at that point would rewrite the pinned engine's
signature, changing its bytes and breaking the one thing the manifest asserts
about it."

Notarization does not permit that exemption. `Contents/MacOS/mihomo` must carry
a Developer ID signature with a secure timestamp, and signing it changes its
bytes. `engineProvenance()` in
`src/core/mihomo/process/engine_discovery.h` hashes the binary it is about to
report on and compares the result with `artifact_sha256`. After a real signing
pass every shipped copy would report "no recorded provenance (sha256 … does not
match the manifest beside it)" in the user interface. The check would be doing
exactly its job, on a binary that is exactly what it should be.

The fix is small and belongs to packaging, not to the engine build. The
build-time hash stays what it is: a reproducibility record for an unsigned
artifact, proving that the same submodule commit produces the same bytes.
Release packaging then signs the engine and records a second field — call it
the shipped-artifact hash — computed after signing, and `engineProvenance()`
accepts a match against either. A shipped bundle then still cannot lend a stale
manifest's provenance to a different binary, which is the property that
mattered, and a signed bundle stops lying about itself.

Do this first. It blocks the first signed release whether or not an updater is
ever built.

The same applies to `libclash_qt_backend_module.dylib`, the helper, and
everything `macdeployqt` places in `Contents/Frameworks/`. The existing signing
order in `cmake/Packaging.cmake` already keeps Qt's tool from signing anything
(`-no-codesign`) and owns the order itself; the release change is which identity
it signs with, plus `--options runtime --timestamp`, plus `notarytool submit`
and `stapler staple` after the last signature.

## The options

### Sparkle

Version **2.10.0**, published 2026-09-13, MIT licence, minimum macOS 12.0 as of
that release. It is the standard macOS update framework and it is not
Cocoa-only: the project's own site says it "Works with any macOS app, whether
it uses Cocoa, SwiftUI, Qt, Xamarin, or otherwise".

There is a working existence proof for this exact combination. OBS Studio is a
Qt 6 application built with CMake that ships Sparkle on macOS, with CMake
variables for the appcast URL and public key, and a release job that generates
deltas and a new appcast when a notarized build is produced.

What it costs to set up:

- An EdDSA keypair, generated once with Sparkle's `generate_keys`. The private
  key goes into the keychain and must be backed up and mirrored into CI
  credential storage; the public key goes into `Info.plist` as `SUPublicEDKey`.
- `Sparkle.framework` embedded in `Contents/Frameworks/`.
- `SUFeedURL` in `Info.plist`, pointing at an appcast served over HTTPS.
- A small Objective-C++ file driving `SPUStandardUpdaterController` and a
  "Check for Updates" menu item. This is not new ground for the project: Apple
  builds already `enable_language(OBJCXX)` and already compile
  `src/services/macos/macos_helper.mm`.
- A release step running `generate_appcast`, which signs each archive and emits
  the appcast, including binary delta updates between releases.

What it requires from the developer: a Developer ID identity and notarization
for a good experience, though Sparkle's own verification does not depend on
them — it accepts an update when the EdDSA signature validates, and uses a
matching Apple signature to authorise a later EdDSA key rotation. Hosting can
be anything that serves files over HTTPS; GitHub Releases for the archives and
GitHub Pages for the appcast is sufficient and free.

What it does about the privileged helper: nothing, and it should not pretend to.
Sparkle can install a `.pkg` payload instead of a bundle, which would let a
release touch `/Library`, but its own documentation lists what that costs:
authorization is always required, silent automatic installs become impossible,
delta updates are unavailable, and there is no fallback for rotating signing
keys. Do not take that trade to solve a helper problem that identity reporting
and a Repair prompt solve better.

### Qt's own offerings

There is no first-party Qt auto-update framework for a deployed application.

Qt Installer Framework (4.11.0) ships a maintenance tool that can add, update
and remove components from a repository. It is an installer and
maintenance-tool model aimed at SDK-shaped installations, obtained through a Qt
Account, and it does not do in-place replacement of a macOS application bundle.
Adopting it would mean abandoning the `.app` distribution model to gain
something Sparkle already does better on this platform.

QtAutoUpdater, the third-party library people usually mean when they ask this
question, last released 3.0.0-4 for Qt 5.14.1 on 2020-02-27 and has had a
"Looking for a Maintainer" issue open since 2023-03-04. There is no Qt 6
release. It is not a candidate.

### A hand-rolled check-and-download

Tempting, because half of it is trivial. `QNetworkAccessManager` is already
linked, a JSON manifest on GitHub Releases is one file, and comparing it with
the application version is a few lines.

The trap is that the easy half is the *notification*. The hard half is the
installation: verifying a signature rather than a checksum, replacing a bundle
while it is running, relaunching cleanly, handling quarantine attributes on the
downloaded archive, and not corrupting an installation when the machine loses
power halfway through. Sparkle is ten thousand lines because those problems are
real.

A checksum published beside the artifact it describes proves nothing an
attacker who can replace the artifact cannot also forge. If a hand-rolled path
is taken, it must verify a signature made with a key that never touches the
server, which is exactly what Sparkle's EdDSA step already is.

The defensible hand-rolled option is to build only the notification half and
send the user to a download page. That is worth considering on its own merits
and is covered in the recommendation.

### Homebrew cask

No in-app code at all. The user runs `brew upgrade`, or `brew` upgrades it as
part of a normal maintenance pass.

What it requires: an immutable, versioned, checksummed release artifact, and a
bundle that passes Homebrew's Gatekeeper checks — which today's ad-hoc signed
bundle does not. Homebrew's acceptable-cask policy requires that artefacts
Gatekeeper can assess must pass those checks and must not require System
Integrity Protection or Gatekeeper to be disabled or bypassed. A cask in a
project-owned tap sidesteps any question of whether the application is notable
enough for `homebrew/cask`.

It is worth noting that this is **not an alternative to Sparkle**. Homebrew's
cask `livecheck` has a `:sparkle` strategy that reads a Sparkle appcast to
discover new versions, and casks using it are expected to carry
`auto_updates true`. The appcast built for Sparkle is also the thing that keeps
a cask current. Doing both costs almost nothing extra once the first is done.

### Comparison

| | Setup cost | Needs signing identity | Needs a server | Handles the privileged helper |
| --- | --- | --- | --- | --- |
| Sparkle 2.10 | One to two days of build and release work | Strongly wanted, not strictly required | Static HTTPS hosting; GitHub Pages is enough | No, and says so |
| Qt Installer Framework 4.11 | High; changes the distribution model | Yes | Yes, a component repository | No |
| QtAutoUpdater | Unmaintained since 2020, no Qt 6 | — | — | — |
| Hand-rolled installer | Deceptively high | Yes | Static hosting | Only if you build it |
| Hand-rolled notification only | Half a day | No | Static hosting | Not applicable |
| Homebrew cask | Low, once releases are signed | Yes, for Gatekeeper checks | No, uses GitHub Releases | No |

Windows and Linux are out of scope here. `README.md` is explicit that neither
is supported, and a cross-platform update story is a question for whenever they
are.

## Recommendation

**Adopt Sparkle 2, and publish the same artifacts as a cask in a
project-owned tap. For v0.1.0, run the feed in informational mode so the
application tells the user about a new version and links to it, rather than
installing it.**

The reasoning, in the order it matters:

**The updater is not the blocker; the signature is.** The work that unblocks
distribution — a Developer ID identity, notarization, stapling, and a
provenance check that survives signing — is the same work whether or not an
updater exists, and it has to be done first either way. Wiring Sparkle in
before that is wiring in a channel that cannot carry anything.

**Sparkle costs almost nothing extra once signing exists.** The keypair, the
framework, the plist keys and `generate_appcast` are the small end of the work.
Choosing it now and switching it on later is cheaper than shipping a
hand-rolled notifier and replacing it at 1.0.

**At v0.1.0 the cost of a bad release is a message, not a recall.** With few
users, a broken version is fixed by telling people. With automatic installation
enabled, a broken version is pushed to everyone before anyone reads the bug
report. The asymmetry runs the other way at 1.0, when most users will never
read a release note and an unpatched security fix is the larger risk.

**This application can break a user's networking.** It manages the system proxy
and, in service mode, a root-owned core doing TUN routing. A bad automatic
update here does not merely crash an application; it can leave a machine
without working networking, which is also the state in which the user cannot
easily download a fix. That argues for the first few releases being a decision
the user makes rather than one made for them.

**The safety switch belongs in the feed, not in the binary.** Sparkle's
`sparkle:informationalUpdate` is an appcast property: an item marked with it
offers a download link instead of installing. Shipping the full updater with
informational items means the first release already carries the code path, and
turning on real installation later is a change to one XML file rather than a
new release. Combine it with `sparkle:phasedRolloutInterval` when installation
is switched on, so the first real automatic update reaches a fraction of
installations before all of them.

**Do not hand-roll, and do not use Qt Installer Framework.** The first costs
more than it looks like and gets the security part wrong by default; the second
is the wrong shape for a macOS bundle.

What this means concretely for v0.1.0: ship a Developer ID signed, notarized,
stapled bundle with Sparkle compiled in, automatic checking on, automatic
installation off, and every appcast item marked informational. Publish a cask
in a project tap against the same artifacts. Turn off the informational flag
for v0.1.1 or v0.2.0, once a signed release and the update path have both been
exercised on a machine that has never seen the source.

## Setting it up

### Build

1. Give `project()` a `VERSION` in `CMakeLists.txt` and derive both
   `MACOSX_BUNDLE_SHORT_VERSION_STRING` and the value
   `QApplication::setApplicationVersion()` receives from it. Sparkle compares
   against `CFBundleVersion`; the interface must agree with the feed.
2. Set `CMAKE_OSX_DEPLOYMENT_TARGET` explicitly. Sparkle 2.10 requires macOS
   12.0, and the bundle should state its floor rather than inherit the SDK's.
3. Add an `Info.plist` template via `MACOSX_BUNDLE_INFO_PLIST`, carrying
   `SUFeedURL`, `SUPublicEDKey` and the automatic-check defaults. CMake has no
   target property for Sparkle's keys, so the existing `MACOSX_BUNDLE_*`
   properties are not enough.
4. Acquire `Sparkle.framework`. A pinned submodule under `3rdparty/` matches
   how the engine is handled and keeps the build from fetching anything on its
   own; [build](build.md) is explicit that nothing else in the build fetches
   from the network. A checked-in release XCFramework is the alternative. Do
   not add a configure-time download.
5. Copy the framework into `Contents/Frameworks/` in the same POST_BUILD step
   in `cmake/Packaging.cmake` that places the module and the helper — that is,
   **before** Qt deployment and before the deep signature. Sparkle contains its
   own nested executables and XPC services, and they must be in place when the
   bundle is deep-signed.
6. Add one Objective-C++ translation unit owning an `SPUStandardUpdaterController`
   and a "Check for Updates" action in the shell menu.

### Release

```
codesign (inside out, Developer ID, --options runtime --timestamp)
  -> engine, module, helper, Sparkle's nested executables, then the bundle
record the shipped-artifact hash into mihomo-provenance.json
archive        (zip or dmg)
notarytool submit --wait
stapler staple
generate_appcast   (EdDSA-signs each archive, emits deltas, writes the appcast)
publish        (archives to GitHub Releases, appcast to HTTPS hosting)
```

Nothing here can run on a developer machine as a matter of habit; it needs a
release job, and there is no continuous integration in the tree yet. Build
releases with `PRESET=release`, which already refuses to proceed when the
engine submodule has uncommitted changes — see [build](build.md).

### Keys and identities

| | What it is | Where it lives | What happens if it is lost |
| --- | --- | --- | --- |
| Developer ID Application certificate | Apple's identity for you | CI credential storage | Re-issue from Apple; users see Gatekeeper warnings meanwhile |
| Notarization credential | App Store Connect API key or app-specific password | CI credential storage | Re-issue |
| EdDSA private key | Sparkle's update-signing key | Offline backup plus CI credential storage | **The update channel is dead.** Existing installations will reject every future update |

[packaging](packaging.md) already states the rule these follow: signing secrets
for a real release belong in CI credential storage, never in the tree.

The EdDSA key is the one to be most careful with. Apple's certificate decides
whether macOS will launch the application; the EdDSA key decides what code runs
on a machine that has already trusted you. Back it up somewhere that is not the
machine that signs releases.

### What is verified, and by whom

Four independent checks, not one:

1. **Sparkle** verifies the EdDSA signature over the downloaded archive against
   `SUPublicEDKey` in the running application's `Info.plist`. This is the check
   that matters: it is made with a key that never goes near the server, so
   compromising the hosting does not compromise the update.
2. **Sparkle** also checks that the new bundle's Apple code signature is valid
   and consistent with the running one, which is what lets an EdDSA key be
   rotated safely in a later release.
3. **Gatekeeper** checks the stapled notarization ticket when the updated
   bundle first launches.
4. **The application itself** checks the engine against
   `mihomo-provenance.json` at run time, after the shipped-artifact hash change
   described above. That is the check specific to this project, and it is the
   one that means a compromised release cannot quietly swap the engine for
   something else while still reporting a clean provenance line.

A checksum is not one of these. A sha256 published beside the file it describes
authenticates nothing; it detects transmission damage. Every claim of integrity
in this chain rests on a private key.

### Rollback

Sparkle offers only versions newer than the one running, so there is no
downgrade. Rollback is roll-forward, in two steps:

1. **Remove the bad item from the appcast immediately.** The feed is a static
   file, so this is one push and it stops every installation that has not
   already updated. Do this first, before diagnosing anything.
2. **Publish the previous code under a higher version.** Users who already took
   the bad build can only be reached by an update they will accept, and that
   means a larger version number carrying the older, working code.

Two limits worth writing down now rather than discovering later. An
installation that the bad release left unable to launch cannot be rescued by
the updater at all; those users need a download link and a message. And an
update that changed the helper leaves the previous helper installed under
`/Library` until the user runs Repair, so a rollback of the bundle does not
roll back the privileged side — which is another reason the application needs
to be able to see that mismatch.

`sparkle:phasedRolloutInterval` limits how many installations can reach a bad
release before step 1 happens. Use it from the first non-informational release.

## What must be true before this is switched on

An auto-updater is a remote code execution channel, built deliberately.
Whoever controls the appcast, the archives, or the EdDSA private key can run
code as the user on every installation, without any further interaction.

For this application the boundary reaches further than that. The privileged
installer copies whatever core binary the application hands it into a
root-owned location and runs it as root, after one administrator authorization
the user grants through an explicit action. A compromised update that persuades
a user to press Repair obtains a root-owned core. [service-protocol](service-protocol.md)
is already clear that this is a trusted-owner model and not a sandbox for
hostile configurations; an update channel extends who counts as the trusted
owner to include whoever can write the feed. The update channel and the
privileged install path are one trust boundary, and should be reviewed as one.

The checklist, in order:

1. `make package` produces a Developer ID signed, notarized, stapled bundle
   that passes `spctl -a -t exec -vv` on a machine that has never seen this
   source tree. Today the bundle is ad-hoc signed and does not.
2. The engine provenance check passes on that signed bundle — the
   shipped-artifact hash change. Without it the first signed release ships a
   user-visible provenance failure.
3. The EdDSA keypair exists, the private key is backed up offline, and neither
   it nor the Apple credentials are anywhere in the repository.
4. The application can tell that the installed privileged helper differs from
   the one in the bundle, and says so in Settings. Until this exists, every
   update that touches the helper silently leaves the previous one running as
   root.
5. There is a test of a *relocated* install — a bundle copied outside the build
   directory, with no build-tree environment and no developer SDK paths.
   [packaging](packaging.md) already notes this is not automated; an updater
   makes it mandatory, because relocating the application is what an updater
   does.
6. Releases are built by a job, not by hand, from the `release` preset, so that
   what is signed is what the tag says.
7. Each release publishes the three facts [packaging](packaging.md) asks for:
   the engine's source commit and sha256, the application's revision, and the
   Qt version deployed into the bundle.
8. The rollback procedure above has been rehearsed once on a throwaway feed,
   before it is needed on a real one.

Items 1, 2 and 4 are hard prerequisites. An updater shipped without them is a
channel that either cannot deliver anything, delivers something that reports
itself as untrustworthy, or updates half of a privileged system and does not
mention it.

## What could not be verified

- Whether an `SMAppService`-registered LaunchDaemon picks up a replaced
  application bundle automatically, or requires re-registration. Apple's
  documentation page did not render for automated retrieval; the bundle layout
  and approval flow were confirmed from secondary sources only.
- Whether `AuthorizationExecuteWithPrivileges`, which the current installer
  uses, still behaves correctly under the hardened runtime and notarization on
  current macOS. It is deprecated and this project already plans to leave it.
- Sparkle's exact behaviour when an application's signing identity changes
  between releases. The first signed release is precisely that transition —
  ad-hoc to Developer ID — and Sparkle's documentation covers rotating a
  Developer ID and an EdDSA key together but not this case.
- Homebrew's current numeric notability thresholds. Both the acceptable-cask
  and acceptable-formula pages now describe the criteria in prose and publish
  no star, fork or watcher counts.
- The size of a fully staged bundle, which determines whether delta updates are
  worth configuring on day one. No stage directory existed in the tree while
  this was written. For scale: the engine alone is 56,406,850 bytes according
  to the provenance sample in [build](build.md), and Qt deployment adds the
  frameworks on top of that.
