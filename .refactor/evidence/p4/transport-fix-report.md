# TRANSPORT GENERATION FIX — registrations, proofs, evidence

Subject: the coordinator's working tree at `f04647f` + uncommitted stable ABI
final repair (wire 3), snapshotted to `snap/` and built at `build/`
(Ninja, Debug, `BUILD_TESTING=ON`, app ON, Qt 6.11 homebrew). The repository was
never built. `snap-base/` is the same snapshot with this lease's twelve files
restored to their pre-fix contents (`git show HEAD:<path>`, plus the ABI
worker's `pendingNativeWork()` retained in `mihomo_backend.h`) and is the A/B
baseline. `sync.sh` copies ONLY the files this lease owns into `snap/`; every
inversion was applied to `snap/` and undone by re-running it.

Every run: `run_check.py` (isolated `CLASH_QT_DATA_DIR`, `QT_QPA_PLATFORM=offscreen`,
`QT_QUICK_BACKEND=software`), preferences hashed before and after. Every value,
every run, before and after:
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`
(`logs/*.txt`, `/tmp/clash-qt-p4.w0Y8Uo/check-runs.jsonl`). No privileged
helper, no system proxy/VPN/network settings, no trust store, no real
subscription, no Clash Verge discovery: loopback fixtures and the compiled
fixture core only. No submodule touched. No build file touched.

## Build registrations required: NONE

Every new case lives in an already-registered target and no file was added:

| new coverage | target already registered at |
| --- | --- |
| 2 shared scenarios x 4 rows | `tests/core/mihomo/CMakeLists.txt:10` (`backend-real-contract`), which already compiles `backend_common_cases.cpp` |
| 2 stream cases | same target |
| 2 client cases | `tests/CMakeLists.txt:27` (`controller`) |
| 1 provider case | `tests/CMakeLists.txt:31` (`provider`) |

`ctest -N` still registers 57 in this snapshot; the counts inside four suites
move (see below).

## Public API added (no facade/bridge/ABI signature changed)

* `core::MihomoClient::SessionParticipant` + `addSessionParticipant` /
  `removeSessionParticipant` — explicit binding, replacing ProviderClient's
  `endpointChanged` subscription. Registration order can no longer decide
  whether a participant retires before or after the owner's bump.
* `core::MihomoClient::retireSession(QString reason)` — owner-driven boundary
  that does NOT emit `invalidating()`.
* `core::MihomoClient::sessionEpoch()` — observable session identity.
* `core::ProviderClient::retireSession()` (the participant override) and its
  destructor; `sameEndpoint()` REMOVED.
* private `core::MihomoBackendImpl::retireTransport(QString)`.

`cb::MihomoBackend`, `BackendBridge`, `clashqt_com` and wire 3 are untouched;
the ABI final repair's `pendingNativeWork()` / `probeCompletionsAfterCancel()`
getters are retained verbatim.

## One invalidation path

`MihomoClient::beginSession(change, announce, streams, reason)` is the only
place a boundary happens, in this order: epochs (`sessionEpoch_`,
`requestEpoch_`, `endpointEpoch_` when the address moved, `streamEpoch_` when
the streams are retired) -> `invalidating()` (owner bumps) -> participants ->
TUN cancellation -> aborts. Callers:

| caller | change | announce | streams |
| --- | --- | --- | --- |
| `setEndpoint` same address | Session | Announce | Retire + `restartStreams()` |
| `setEndpoint` new address | Endpoint | Announce | Retire + `restartStreams()` |
| `detach` | Endpoint | Announce | Retire, closed not reopened |
| `setConnected(false)` | Session | Announce | Keep (the sockets are this session's; their own reconnect heals them) |
| `retireSession` (start/stop/managed failure) | Session | **Silent** | Keep |

Silent is what stops the recursive bump: `invalidating()` is what makes
`MihomoBackendImpl` bump, and the lifecycle paths have already bumped.
`retireTransport()` sets `tunSuperseded_` before calling, so B2's
"supersession is never a protocol error" holds on the silent path too.

`streamEpoch_` is deliberately neither `endpointEpoch_` (which a replacement
does not move — the F3 defect) nor `requestEpoch_` (which moves on a plain
disconnect, where gating would leave a recovered stream ignored forever).

### Why a lifecycle retirement keeps the streams

Measured, not assumed. Retiring them there too closes and re-dials a live
subscription on every start, stop and failure. It changes nothing observable:
a lifecycle bump moves no address, and every replacement of the process behind
an address is announced by the `attach()` that a new core's readiness produces
(`CoreProcess::ready` -> `attach`), which IS a Retire boundary. Inversion I3
shows that is where the hazard lives.

## Results (snapshot `build/`)

| suite | before | after |
| --- | --- | --- |
| `controller` | 21 | **23** |
| `provider` | 5 | **6** |
| `backend-contract` (frozen fake suite, not owned) | 41 | 41 |
| `backend-real-contract` | 95 | **105** (2 stream cases + 2 shared scenarios x 4 rows) |
| `engine-discovery` | 29 | 29 |
| full `ctest -E real-core` | — | **57/57** (`logs/ctest-final.txt`) |

No skips, no expected-failure markers, no QSKIP. All four rows
(direct-fake / direct-real / module-fake / module-real) run every shared
scenario; both module artifacts were supplied and loaded.

## Inversions — all on `snap/`, restored afterwards

| id | mutation | result |
| --- | --- | --- |
| I1 | `ProviderClient::retireSession()` returns early (the audited state: nothing moves the provider epoch) | **DETECTED** `logs/inv-I1-*.txt`: 4 real rows of both new scenarios fail on `freshProviders != retiredProviders`; `provider` fails the new case AND the pre-existing `staleResponseIsIgnoredWhenReturningToSameEndpoint` |
| I2 | stream callbacks gated on `endpointEpoch_` again (the original F3 gate), sockets still replaced | **SURVIVED** `logs/inv-I2-*.txt` (105 + 29 pass). Recorded, not papered over: once the retired socket is disconnected and closed the epoch gate is a genuine redundancy. Kept as the second line of defence for any future path that moves `streamEpoch_` without replacing a socket |
| I3 | `restartStreams()` dropped from the same-address boundary | **DETECTED** `logs/inv-I3.txt`: both stream cases fail — "the replacement never re-opened the stream, so the retired session's socket WAS the live subscription" |
| I4 | `retireTransport()` dropped from `start`/`stop`/managed failure | **DETECTED** `logs/inv-I4.txt`: `sharedLifecycleBumpRetiresOutstandingWork` fails on direct-real and module-real |
| I5 | `FakeBackend::attach()` same-endpoint early return restored (F4) | **DETECTED** `logs/inv-I5-*.txt`: `sharedReplacementAtTheSameAddressOpensANewSession` fails on direct-fake and module-fake at "re-attaching to a replaced engine left the generation where it was" |
| I6 | `FakeBackend::completeRequest()` supersession downgrade removed | **DETECTED** `logs/inv-I6-*.txt`: both fake rows of `sharedLifecycleBumpRetiresOutstandingWork` fail — "work owed by the retired session settled as live data" on BOTH `providers/rules` and `version` |
| I7 | `MihomoBackendImpl::publish()` downgrade removed (`stale = false`) | **SURVIVED** `logs/inv-I7.txt`. **Explicitly unverified by test.** With `retireTransport` in place the abort marks everything first, so this guard is unreachable through the facade. I4+I7 together (`logs/inv-I4I7.txt`) still fail, but at the earlier coalescing assertion, so they do not isolate it either. Kept as the stated invariant ("no path publishes retired work as Ok") and reported as redundancy, not as proven behaviour |
| I8 | epochs moved AFTER the abort in `beginSession` (the ordering hazard) | **DETECTED** `logs/inv-I8-*.txt`: 8 real-contract failures including the pre-existing `anAbortedCompletionIsMarkedSuperseded...` and `aReplacementAtTheSameAddressCannotClearTheNewSession`, plus 3 controller failures — one of them the new "announced an invalidation of its own", because the un-retired reply reaches `setConnected(false)` |
| I9 | provider coalescing key made unique per submission | **DETECTED** `logs/inv-I9.txt`: 7 failures, including both new scenarios' "same-generation duplicate still coalesces" arm and the pre-existing coalescing cases |

Satisfiability of "a fresh id is really ISSUED" is carried by the completion,
not by a counter: the post-boundary request is asserted to SETTLE (Ok, admitted)
after the gate is released. An implementation that minted a fresh id and issued
nothing never settles it, and an implementation that coalesced fails the id
assertion first (I1, I4, I5 all land there).

## Honest findings that are NOT this lease's to fix

`w05-recovery :: aCoreThatRefusesToTerminateIsKilledAndTheQuitStillCompletes`
is a load-dependent flake, PRE-EXISTING. It asserts the quit took at least the
3000 ms termination wait, and measures 2850-2960 ms when it fails — the case's
clock starts after the terminate timer does, so the margin is ~2-5%.

Measured both ways, same machine, interleaved:
`snap-base` (pre-fix) **7 failures in 20 runs**; `snap` (fixed) 7 in 16 with
streams retired on lifecycle bumps, 3 in 8 with them kept; the final full
`ctest` run passed it. The failure rate tracks machine load, not the change.
The file belongs to the W02 lease; the assertion needs a tolerance or a
witness of the kill rather than a wall-clock lower bound.
