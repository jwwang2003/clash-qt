# P4 final audit — read-only source/contract/registration inspection

Auditor: independent, Opus. Repo + git READ-ONLY. Writes confined to
/tmp/clash-qt-p4.w0Y8Uo/final-audit/**.

Plist baseline confirmed by me before any run:
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`
(matches the value named in my brief and in .refactor/P4_QUALIFICATION.md).

## A. Module boundary (D8 / module-r1)

| Claim | Where | Verdict |
| --- | --- | --- |
| Engine is an actual SHARED library | cmake/Component.cmake:38 `add_library(clash_qt_backend_module SHARED ...)` | TRUE |
| Exported C factory + handshake | src/core/component/abi/module_entry.h:63 `ModuleEntryFn`, src/core/mihomo/module/mihomo_module_entry.cpp:55 | TRUE |
| Identity/version/target/runtime/wire all checked | abi/module_entry.h:130-163 `CheckHandshake` (structSize, abiVersion, wireRevision, interfaceRevision, targetAbiTag, runtimeTag, moduleId) | TRUE — all six |
| Response size validated before EVERY write | src/core/mihomo/module/module_export.cpp:18-21,69-71 | TRUE |
| Null written first on all failure paths | module_export.cpp:62-64 | TRUE |
| No Qt/STL allocating values across the boundary | abi/backend_abi.h — only `IObject*`, `void*`, `std::size_t`, `std::uint32_t`, `IBuffer**`; payloads are raw wire bytes | TRUE |
| Thin supervisor, no proxy traffic marshalled | backend_abi.h:13-18; module wraps MihomoBackendImpl which still spawns a child engine (core_process.cpp) | TRUE |
| Qt bridge / MihomoBackend unchanged | src/core/CMakeLists.txt:23 clash_backend_bridge unchanged; backend-r4 interfaceRevision still 1 (wire.h:86) | TRUE |
| App links no private impl | root CMakeLists.txt:41-58 links clash_module_host, not clash_mihomo_impl; architecture.json consumers["clash-qt"].deps has no clash_mihomo_impl | TRUE |
| Only the factory is exported | CXX_VISIBILITY_PRESET hidden on the module target; CLASHQT_MODULE_EXPORT on the one entry | needs binary proof (deferred to runtime) |
| Both G2 migration exceptions deleted | architecture.json now has exactly ONE exception, `G2-impl-subject-suites-link-component-private` (remove_in "never", 6 sites). `git show c42caf9:tests/architecture/architecture.json` had 3 (the two `remove_in: P4` ones are gone) | TRUE — deleted, not re-baselined |
| Permanent six-entry allowance intact | same entry, 6 sites, sealed_modules[].sealed_while "G2-*" still active | TRUE |

Loader protocol (module-r1): release root → keep IModuleLifetime → PrepareUnload
→ on refusal restore root via QueryInterface and stay usable → on success drop
lifetime last, then close handle. src/integrations/component/module_loader.cpp:246-330.
Matches the contract verbatim. No source/build/PATH fallback: module_loader.cpp:100-118
(installation-relative candidates only), :140-146 (isFile() checked before dlopen).

Deferred final Release: backend_abi.h:89-108 states zero only on actual
destruction; nonzero when a cleanup reference is retained. Matches
P4_ABI_CONTRACT.md "Qualified implementation details".

## B. Independent sample consumer

examples/component_consumer/CMakeLists.txt is a standalone project: `Qt6::Core`
+ `${CMAKE_DL_LIBS}` only, no add_subdirectory of the desktop, no project
library on the link line. install() of the public SDK exists
(cmake/Component.cmake:60-63, COMPONENT component-sdk).

FINDING (minor, to be tested at runtime): the default
`CLASH_QT_COMPONENT_INCLUDE_DIR` is `${CMAKE_CURRENT_LIST_DIR}/../../src`
(examples/component_consumer/CMakeLists.txt:9) — the whole source tree, which
also contains the private engine headers. The header-isolation claim is only
literally true when the variable is pointed at the installed SDK. I will build
the sample against the INSTALLED headers, not the source tree.

## C. Shared assertion set across four backends

tests/support/backend/common_contract_driver.h:71-79 — Subject{DirectFake,
DirectReal, ModuleReal, ModuleFake}; `addSubjectRows()` is the single row builder
"so no case can quietly run against three of four".
tests/core/mihomo/backend_real_contract_test.cpp:1776-1784 — an unavailable
subject is `QVERIFY2(unavailable.isEmpty())`, i.e. a FAILURE, not a skip. No
missing-module skip, no XFAIL/QEXPECT_FAIL anywhere in tests/.
17 shared cases (backend_common_cases.h) x 4 subjects.

Newly-covered transport cases are present as shared cases:
- `aReplacementAtTheSameAddressOpensANewSession` (same-address providers)
- `aDuplicateProviderRequestIsCoalesced` (coalescing)
- `aLifecycleBumpRetiresOutstandingWork` (global-generation supersession)
- retired streams: src/core/mihomo/mihomo_client.cpp:124-155 `restartStreams()`
I will mutation-test the same-address session boundary myself.

## D. Registration completeness (no producer left unconsumed)

- W02 has 5 slots; 4 registered as `w02-subscription-update`, the 5th
  (`theRealSourceBuiltCoreRefreshesAndRejectsAnInvalidUpdate`) as `w02-real-core`
  (tests/workflows/CMakeLists.txt:24-28, :103-107). None unregistered.
- connection_snapshot_marshal_test has 6 slots; 3 correctness in
  `connection-snapshot-codec`, 3 timing in `connection-snapshot-marshal`
  (benchmark label). None unregistered.
  (tests/contracts/component/abi/CMakeLists.txt:28-37)

## E. Benchmark honesty (D8 constraint 2)

tests/benchmarks/connection_snapshot_marshal_test.cpp:287-299 records the
measured packed-vs-naive ratio with `qInfo` and explicitly does NOT assert a
speedup ("D8 requires a measured cost, not a universal speedup over this
particular comparator"). No assumed-faster claim. The lane is excluded from
`make test` (Makefile test target excludes label `benchmark`), so it must be run
separately — I will run it.

## F. Safety guards in the CURRENT code

- src/main.cpp:147-153 — `CLASH_QT_SERVICE_SOCKET`; and :148 when it is unset
  but `CLASH_QT_DATA_DIR` is set, the socket resolves to `helper.socket` INSIDE
  the isolated data dir, never the installed default. This is the repair for the
  withdrawn early-P4 smoke claims.
- tests/workflows/app_smoke_test.cpp:166-183 `childEnvironment()` removes
  CLASH_QT_CORE_BINARY / CLASH_QT_CONTROLLER / CLASH_QT_SECRET and always
  inserts an isolated CLASH_QT_DATA_DIR and CLASH_QT_SERVICE_SOCKET.
- tests/CMakeLists.txt:13-21 + :367-376 — every registered test gets
  CLASH_QT_DATA_DIR.
- Engine is built only from the pinned submodule (cmake/Mihomo.cmake:1-3,
  "deliberately no fallback to PATH, to another Clash installation"). W02's
  real-core arm re-verifies commit, sha256, size, clean state and the binary's
  own `-v` output (w02_subscription_update_test.cpp:742-800). Not Clash Verge.
- Termination grace is a precise monotonic deadline, re-armed while time
  remains: src/core/mihomo/process/core_process.cpp:527-539. Not coarse-early.

## G. Config / UI

- Pure composition extracted: src/core/config/config_composer.{h,cpp},
  target clash_config_compose (src/core/CMakeLists.txt:68-72).
- ProfileStore uses the real composer on BOTH paths: profile_store.cpp:1055
  (preview) and :1180 (runtime generation).
- Preview is side-effect free: buildPreview (profile_store.cpp:1021-1087) has no
  writeFile and no launch; the write path is a different function.
- Last-good recovery: profile_store.cpp:797-893; backup compatibility carries
  `presets.json` + `presets.last-good.json` (backup_store.cpp:90, :314-327).

## H. Honest self-reported gaps found in the records (not concealed)

- ModuleBackend::notify's `commandDepth_ > 0` term is a KNOWN SURVIVING MUTANT,
  documented in place (module_backend.cpp:436-446) as a redundancy for today's
  modules, not a coverage claim.
- Ledger records the withdrawn early smoke runs that could reach the installed
  helper's status socket, and the two surviving defensive checks from the
  transport repair.
