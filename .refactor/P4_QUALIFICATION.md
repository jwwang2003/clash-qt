# P4 qualification record

Status: OPEN. Intermediate greens below do not close the fresh-clone or audit gate.
Coordinator-owned; updated with final artifact hashes and measured results.

## Target and revisions

- Host: macOS 26.6.2 (25G83), arm64; AppleClang 17.0.0 (clang-1700.6.3.2).
- CMake 4.4.2, Qt 6.11.1. Engine source pinned to
  ab405bad5beeeac8b003bb01f60f134f6df54471 (v1.19.31).
- Object model component-r1; semantic facade backend-r4; module ABI 1, wire 3.
- Windows/Linux: zero C++ compile/run evidence. Other target ABIs unqualified.

## Gates

| Gate | Current evidence | State |
| --- | --- | --- |
| Config, preset UI, recovery and backup | Independent config recheck GO; 15 suites and 15 detected inversions; actual central wiring 10/10 | verified subset |
| Four-way shared backend assertions | 60 common rows plus specialists; later transport audit found uncovered defects | OPEN, fixing |
| Activated module/sample and actual unmap | Real engine and actual RTLD_NOLOAD absence; shared Qt runtime retained | verified subset |
| Lifetime, timestamp and malformed-frame contract | Three independent NO-GO findings repaired; Release zero-return semantics under final correction/recheck | OPEN |
| W02 fixture and pinned engine | Both arms green externally; final transport changes require integrated rerun | OPEN |
| G2 debt | Both migration exceptions removed; permanent six implementation-subject sites preserved | needs final graph check |
| Fresh clone make test / integration / package | Not run at final revision | OPEN |
| Packaged app, installed module path | New helper/controller isolation guards require rebuilt artifact | OPEN |
| Independent whole-P4 read-only audit | Not yet dispatched at final revision | OPEN |

## Preferences and safety record

Initial SHA-256:
681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c

All recorded before/after hashes still match; plist has only window.geometry.
Per-run logs live under evidence/p4 and scratch check-runs.jsonl; final entries
will be copied into this record's evidence set.

Early executable smoke runs could reach the installed helper's status socket: the
pre-existing oversized-config guard protected startCore, not startup status. Those
isolation claims were withdrawn. The root and smoke harness now choose isolated
helper sockets, and local fixture routing was tested. Generic isolated launches
also no longer discover foreign controllers or seed geo-data from another client.
No install/uninstall/restart, system proxy/VPN/trust-store change was requested.
See the incident and replacement evidence in PROGRESS_LEDGER.md.

## Intermediate registration checks

The routine snapshot-codec entry explicitly selects all three correctness slots,
including the wire-3 timestamp-representation case. The benchmark entry selects
only timing cases. W02 routine and real-core entries select their respective slots,
so lane-specific exclusions are not reported as successful skips.
