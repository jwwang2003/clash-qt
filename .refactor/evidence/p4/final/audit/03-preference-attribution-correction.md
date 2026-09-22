# P4 final audit — preference-attribution correction

Bounded follow-up. Candidate `be851f5`, qualified clone `/tmp/clash-qt-p4.w0Y8Uo/fresh-6`.
Repository READ-ONLY throughout. No preference write, no restoration, no app launch,
no helper/network/OS-setting operation was performed for this correction. No test was
re-run (the 3× accused-row and 5× suite rechecks already on record are unchanged).
Writes confined to `/tmp/clash-qt-p4.w0Y8Uo/preference-recheck/**` and to §8 of
`/tmp/clash-qt-p4.w0Y8Uo/final-audit/notes/02-runtime-evidence-and-mutations.md`.

This document corrects **attribution only**. Every measured artifact, mutation,
suite and packaging result in the prior notes stands unmodified.

## 1. Evidence read for this correction

| Source | Path |
| --- | --- |
| User confirmation + recorded hashes/pids | `/tmp/clash-qt-p4.w0Y8Uo/preference-attribution.json` |
| Human-readable app log, 110 lines | `/tmp/clash-qt-p4.w0Y8Uo/preference-change-app-log.txt` |
| Same log, structured (110 events) | `/tmp/clash-qt-p4.w0Y8Uo/preference-change-process.json` |
| Prior report being amended | `/tmp/clash-qt-p4.w0Y8Uo/final-audit/notes/02-runtime-evidence-and-mutations.md` §8 |
| Prior read-only inspection | `/tmp/clash-qt-p4.w0Y8Uo/final-audit/notes/01-readonly-inspection.md` |
| My own run logs (mtimes used as timeline) | `/tmp/clash-qt-p4.w0Y8Uo/final-audit/logs/*.log` |
| Live plist | `~/Library/Preferences/com.clash-qt.clash-qt.plist` |

## 2. User confirmation

> "Yes, I used a separate clash-qt window"

given in answer to whether they opened/moved/closed a clash-qt window around 16:29.

## 3. Facts established from the captured log

The captured window contains **exactly one process**, and it is neither mine nor the
coordinator's:

- pid **57802**, image `/private/tmp/clash-qt-p4.w0Y8Uo/fresh-6/build/p4/clash-qt.app/Contents/MacOS/clash-qt`
  — the **unstaged GUI app bundle**. All 110 events belong to it (verified by counting
  `processID`/`processImagePath` over the JSON).
- The coordinator's staged smoke was pid **51881**, image `…/build/p4/stage/clash-qt.app/…`,
  ended **16:23:23** — a different pid, a different binary path, finished ~5½ minutes earlier.
- My own §3 packaged-app launch was also the **staged** binary (`…/stage/clash-qt.app/…`),
  and its logs (`logs/applaunch.log` 16:36:05, `logs/applaunch-cocoa.log` 16:37:45)
  postdate the whole window below.

So pid 57802 is a third, independent, UI-capable clash-qt process — consistent with,
and now confirmed by, the user's statement.

## 4. Timeline (all 2026-09-22, +0800)

| Time | Event | Source |
| --- | --- | --- |
| 16:23:23 | coordinator staged smoke pid 51881 ended | `preference-attribution.json` |
| 16:28:47.994 | pid 57802 — `Loading Preferences From User CFPrefsD` (a **read**) | app log line 2 |
| 16:28:50.2–50.5 | pid 57802 — `NSPersistentUIManager flushAllChanges` … `writing records` (AppKit **state restoration**, not CFPreferences) | lines 3–6 |
| 16:28:57.339 | pid 57802 — `NSApplication._react(to:) dock` | line 10 |
| 16:29:15.4–20.9 | pid 57802 — text-input/cursor XPC activity (window in use) | lines 15–28 |
| **16:29:20.934** | pid 57802 — `RECEIVED:(aevt,quit) {aevt,quit target=Dock}`, `Handling Quit AppleEvent` | lines 29–30 |
| 16:29:21.173–21.270 | pid 57802 — `terminate:`, exit handler, last event | lines 64–111 |
| 16:29:33 | **my** first `ctest` finished — guard tripped; hash observed `82a0d1e4…` | `logs/baseline-ctest.log` |
| 16:30:38–16:31:21 | my 3× accused row + 2 suite reruns — hash stable | `logs/prefprobe-{1,2,3}.log`, `logs/brc-rerun-{1,2}.log` |
| 16:33:51 / 16:35:06 | my restored suites, all green | `logs/restored-ctest.log`, `logs/restored2-ctest.log` |
| **16:35:57** | plist on-disk mtime of the **current** contents | `stat` (recorded below) |
| 16:36:05 / 16:37:45 | my staged-app launches (after the last plist change) | `logs/applaunch*.log` |

## 5. Current plist state, read now

```
path   /Users/junweiwang/Library/Preferences/com.clash-qt.clash-qt.plist
sha256 204612a89e487d9f3307438f3f2ca97e60056c66919bcebe113a89f863428ffc
size   133 bytes      mtime 2026-09-22 16:35:57 +0800      mode -rw------- junweiwang:staff
keys   exactly one: "window.geometry" => 66-byte data, tail 0x…0000070700000451
```

Unchanged since the value recorded in `preference-attribution.json`. **Left exactly as
found**: no write and no restoration was attempted, deliberately — these bytes are the
user's own live window geometry, and rewriting them would itself be an unrequested
mutation.

Historical hashes preserved:

| Stage | sha256 |
| --- | --- |
| Baseline at my start (matched `.refactor/P4_QUALIFICATION.md`) | `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c` |
| Intermediate, observed ~16:29 | `82a0d1e4531f9b55cdcaa986f3d2a37519d3acd34186eff35190f96a5f744f04` |
| Current, on disk since 16:35:57 | `204612a89e487d9f3307438f3f2ca97e60056c66919bcebe113a89f863428ffc` |

Key count never varied: one key, `window.geometry`, 66 bytes, only the trailing
geometry bytes differing (`…0453` → `…0451`, i.e. 1107 → 1105 — a small window
move/resize). **No user key was added, removed, reordered or corrupted at any point.**

## 6. Corrected attribution

### Confirmed
1. A **separate, user-driven clash-qt GUI window existed and was in use**, pid 57802,
   overlapping my first `ctest` (app quit 16:29:21; my run logged 16:29:33). Confirmed
   by the user and independently by the process log.
2. That process is a **full UI build capable of authoring `window.geometry`**. My audit
   build was configured `CLASH_QT_BUILD_APP=OFF` and contains no UI, so nothing in it
   can author a geometry value. It remains the only identified process in the window
   that can.
3. The delta is a **geometry-only, user-scale change**, consistent with opening,
   moving/resizing and closing a window — exactly the activity the user described.

### Withdrawn as unsupported
The prior report's closing claim — *"I caused the first of the two transitions"*, via the
harness's `QSettings("clash-qt","clash-qt")` lookup provoking a `cfprefsd` flush — **is
withdrawn.** It was a mechanism hypothesis reported as a finding. The evidence does not
support it:
- The captured log contains **no preference-write event by any process**. The single
  CFPreferences event in the whole capture is a *read* by pid 57802 at 16:28:47.994.
- No `cfprefsd` process events were captured at all; nothing instruments which client
  set the value.
- `tests/support/scoped_environment.cpp:187` does open the native domain on every scope
  — that remains a true and relevant observation about the harness — but *opening a
  domain* and *causing this write* are different claims, and only the first is proven.

### Not established either way
That pid 57802 performed the write is **consistent and by far the best-fitting
explanation, but is not directly observed**: no write event was captured for it either,
and the capture window is narrow (16:28:47–16:29:21; the process's earlier life and any
later flush are outside it). The on-disk mtime **16:35:57** falls when *neither* pid 57802
(exited 16:29:21) *nor* any test process of mine (last finished 16:35:06) was running —
`cfprefsd` owns the file and defers flushes, so **mtime does not identify the setter**.
The second transition is therefore recorded as **unattributed**, not assigned to anyone.

### What the guard actually proves
`environment_->realPreferencesUnchanged()` compares a before/after hash of a shared,
daemon-owned file. It detects a **hash change concurrent with a test**, never causality.
Its failure message ("this test wrote into the developer's own preference store")
over-claims. This case is a concrete demonstration: a confirmed third-party GUI session
was running during the flagged scope, and the guard cannot distinguish that from a test
write — nor from a deferred daemon flush. The header already concedes the guard is
"best-effort" and that a delayed flush "can escape it"; this is the same weakness in the
opposite direction, and it is a **test-infrastructure caveat, not a product defect**.
No product code wrote: `src/core/preferences/preferences.h:3` forbids the native domain
and `preferences.cpp:45-57` returns an IniFormat `QSettings` inside `CLASH_QT_DATA_DIR`;
no `src/**` call site constructs `QSettings("clash-qt","clash-qt")`.

`.refactor/P4_QUALIFICATION.md:72` (`After: 681784…`) remains **stale for this machine** —
the current value is `204612a8…` — but it should **not** be read as an audit-caused drift.

## 7. Installed-helper caveats — preserved, with scope stated precisely

- The installed privileged helper `pid 89247` (`/Library/PrivilegedHelperTools/org.clash-qt.service/helper --serve 501`,
  started 2026-09-21 08:30:37) was, **in my audit**, never installed, uninstalled,
  restarted, or connected to; my consumer and app runs created no helper socket at all
  (notes/02 §3). **This assertion covers my audit only.**
- **Historical, still documented and not superseded:** early coordinator executable smoke
  runs could contact the installed helper for status
  (`.refactor/P4_QUALIFICATION.md:96`, notes/01 §H). The repair is
  `src/main.cpp:147-153` — with `CLASH_QT_SERVICE_SOCKET` unset but `CLASH_QT_DATA_DIR`
  set, the socket resolves inside the isolated data dir, never the installed default —
  plus `tests/workflows/app_smoke_test.cpp:166-183`. Those early claims stay withdrawn
  and recorded; nothing here relitigates them.

## 8. Standing limitations carried forward unchanged

1. **Runtime build tag ≠ same physical Qt image.** `abi::CheckHandshake` validates the Qt
   *build* tag, not the identity of the loaded Qt image; my first consumer attempt
   accepted the handshake while loading two distinct QtCore images and never became
   ready (notes/02 §2, `logs/audit-consumer.log`). Configuration error, not a contract
   violation — but the duplicate-image case is undetectable by the handshake.
2. **Sample's default include is the source tree.** `examples/component_consumer/CMakeLists.txt:9`
   defaults `CLASH_QT_COMPONENT_INCLUDE_DIR` to `${CMAKE_CURRENT_LIST_DIR}/../../src`,
   which also carries the private engine headers; header isolation is literally true only
   when pointed at the installed SDK, as I did (notes/01 §B, notes/02 §2).
3. **Redundant host-queue mutant survives.** The `commandDepth_ > 0` term in
   `ModuleBackend::notify` (`src/core/mihomo/module/../module_backend.cpp:436-446`) is a
   known surviving mutant, documented in place as redundancy for today's modules rather
   than a coverage claim (notes/01 §H).

## 9. Verdict

The prior **GO stands.** Nothing in this correction touches the artifact checks,
the SDK-consumer run, the three killed mutations, the 68-row four-way shared set, the D8
measurement, or the fresh-clone gates. The only change is that a causal claim I could not
support has been withdrawn and replaced with what the evidence shows: a confirmed,
concurrent, user-driven GUI session; a geometry-only change no headless build of mine
could author; and a guard that measures concurrency rather than causality.
