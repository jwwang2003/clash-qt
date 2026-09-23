# P4 qualification record

Source candidate: `be851f5edf34a25d216df62fcd3d35ab52a08e9b` on `agent/refactor-v2`.
Fresh clone: `/tmp/clash-qt-p4.w0Y8Uo/fresh-6`.
Status: **P4 GO on macOS arm64**. Implementation/artifact gates and the independent
whole-P4 audit passed. Qualified production source is the candidate above; later
closure commits change records only.

## Measured target

macOS 26.6.2 (25G83), arm64; consumer/module AppleClang 17.0.0
(clang-1700.6.3.2), CMake 4.4.2, Qt runtime 6.11.1. Qt's binary reports an
Apple LLVM 21 build. Object model component-r1, facade backend-r4, module ABI 1,
wire 3, connection snapshot format 2. Windows/Linux have zero C++ compile/run
evidence; other architectures are not qualified.

## Gates

| Gate | Result |
| --- | --- |
| Fresh `make test BUILD_DIR=build/p4` | **54/54**, no required skips/XFAIL/XPASS |
| Fresh `make test-integration BUILD_DIR=build/p4` | **20/20**, including W02 and pinned real core |
| Fresh `make package BUILD_DIR=build/p4` | **PASS**, no deployment ERROR, final deep/strict ad-hoc signature verification |
| Independent SDK consumer | **PASS**, built from installed public headers, no project implementation library, real readiness/confirmed stop/actual unmap |
| Packaged native Cocoa launch | **PASS**, bundle-relative module, no developer runtime paths, isolated data/helper/controller behavior |
| Evaluated graph and public headers | **PASS**; both migration G2 exceptions deleted, permanent six implementation-subject sites retained |
| Config/UI/backup independent recheck | **GO**, adversarial recovery/composition/preview/backup probes and inversions |
| ABI/lifetime/marshalling independent recheck | **GO**, Debug/Release and independently loaded real module |
| Whole-P4 independent read-only audit | **GO**, independent artifact runs plus transport/ABI/config mutation proofs |

All 59 registered entries explicitly receive CLASH_QT_DATA_DIR. Native/privileged
labels remain a reserved empty lane; helper tests are local fixtures. Native GPU
rendering, Developer-ID signing/notarization and Windows/Linux are not claimed.

## Artifacts

Engine source: `ab405bad5beeeac8b003bb01f60f134f6df54471`, v1.19.31, clean,
Go 1.26.5 darwin/arm64, `with_gvisor`, CGO_ENABLED=0.

| Artifact | SHA-256 |
| --- | --- |
| Built engine | `6f53b2e18687b26c9948a804ab9e7b1b4421e8d0d3a391c11e8468dd1685b544` |
| Staged engine | `6f53b2e18687b26c9948a804ab9e7b1b4421e8d0d3a391c11e8468dd1685b544` |
| Built module | `c98c32cb2bd8c950a1fbe87bac071376f0f9d4d22d17d7ffe0e7b44347ab455b` |
| Deployed/signed module | `5061ab32cb719279bdd9a4170d725a5a411100c791bf4b2bf9d27db10b6e9de0` |

The module exports only `clashqt_component_module_entry`. The app's symbol table
contains no MihomoBackendImpl, MihomoClient or CoreProcess implementation. Deployment
rewrites/signs the module, so its two hashes intentionally differ; the pinned engine
is copied after nested signing and its hash remains identical.

## D8 marshalling measurement

Production codec, 500 connections × 200 round trips, this macOS arm64 host:

| Build | Packed | Naive | Packed bytes | Naive bytes |
| --- | --- | --- | --- | --- |
| Debug | 1370.59 ms | 1553.10 ms | 114182 | 136336 |
| Release | 662.88 ms | 738.12 ms | 114182 | 136336 |

These are measured workload results, not a universal speed guarantee. Correctness
and bounds run in the routine codec lane; timing cases are benchmark-labelled.
Headless configuration and the shared module also built successfully.

## Preferences and safety

For every final qualification run (routine, integration, package, artifact
inspection, native packaged launch and installed-SDK consumer):

Before: `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`

After: `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`

Later, the independent auditor's first backend-only run overlapped separate app
use and detected this transition:

Before: `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`

After: `82a0d1e4531f9b55cdcaa986f3d2a37519d3acd34186eff35190f96a5f744f04`

The current hash subsequently became
`204612a89e487d9f3307438f3f2ca97e60056c66919bcebe113a89f863428ffc`.
The user confirmed: “Yes, I used a separate clash-qt window.” macOS records a
Dock quit event for development-app PID 57802 at 16:29:20; the qualification's
staged-app PID 51881 had exited at 16:23:23. The file still contains exactly
`window.geometry`. The guard proves concurrent change, not the writer's identity;
the audit's original claim that its QSettings probe caused the change is withdrawn.
The first transition overlaps confirmed user activity, the best-supported
explanation; no write was directly traced. The later transition has no identified
writer. No restoration or alteration of the user's geometry was made. Subsequent
auditor row/suite probes retained stable hashes (some manual probes had unrelated
fixture-path failures; restored registered suites passed).

Paired values for final qualification runs are in
`evidence/p4/final/qualification-runs.json`; the complete coordinator run ledger
is `evidence/p4/final/check-runs.jsonl`. Attribution and independently corrected
notes are archived alongside them. Earlier worker/probe logs remain retained.

Early executable smoke runs could contact the installed helper for status: the
old oversized-config guard covered start, not startup status. Those isolation
claims were withdrawn. Root/harness socket isolation was implemented and proven
against local fixtures; isolated controller discovery and automatic foreign
geo-data seeding were also removed. Historical uncertainty is not erased by later
green tests. No install/uninstall/restart or system proxy/VPN/trust change was
requested or performed by this task. See the incident ledger.

Final evidence lives under `evidence/p4/final/`; rejected/intermediate findings
remain elsewhere in `evidence/p4/` with their corrections in PROGRESS_LEDGER.md.

## Audit limits retained

The runtime handshake fingerprints the Qt build; it cannot prove that host and
module use the same mapped Qt image. Consumers must share one Qt runtime. An
auditor's mismatched deployment (Homebrew host Qt plus the staged bundle's Qt)
passed the fingerprint but failed readiness; the supported shared-runtime consumer
and packaged app passed. The sample's default source include root is broad; the
qualified independent consumers used only the installed public SDK. A redundant
host notification guard mutation survives and is not claimed as proven coverage.

The auditor independently killed same-address-session, runtime-tag and
profile-preset inversions. The shared backend set contains 17 assertions across
four subjects (direct fake/real and module fake/real), 68 shared rows. See
`evidence/p4/final/audit/` for corrected read-only findings and raw runtime logs.
