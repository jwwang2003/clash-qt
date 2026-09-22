# W02 — build registrations the coordinator must make

Worker: W02 (claude-opus-5). Writable sources only; no build file touched.
Written before implementation, updated as the lease progressed.

## 1. `tests/workflows/CMakeLists.txt`

### 1.1 The journeys no longer link the engine implementation

```cmake
set(clash_qt_workflow_libs
    clash_test_support clash_module_host clash_app_runtime clash_app_lifecycle
    clash_app_backup clash_app_composition clash_profiles
    clash_config clash_backups clash_platform clash_preferences clash_telemetry)
```

* `clash_mihomo_impl` **removed**: after this lease no file under
  `tests/workflows/**` includes or names anything from `core/mihomo/**`. This is
  the workflow half of the migration exception the ABI contract clears.
  (`grep -rn "core/mihomo\|MihomoBackendImpl\|CoreTimings" tests/workflows` is
  empty.)
* `clash_module_host` **added**: `workflow_support.h` now includes
  `integrations/component/module_{loader,backend}.h`.

### 1.2 Every workflow target needs the module artifact

For `w01-first-launch`, `w02-subscription-update`, `w03-routing-controls`,
`w04-restore`, `w05-recovery` and `app-smoke`:

```cmake
add_dependencies(<target> clash-qt-fake-core clash_qt_backend_module)
set_tests_properties(<test> PROPERTIES ENVIRONMENT
    "...;CLASH_QT_MODULE_PATH=$<TARGET_FILE:clash_qt_backend_module>")
```

`CLASH_QT_MODULE_PATH` is **required**, not optional: `wf::AssembledApp`
refuses to guess an artifact (`ModuleLoader::installedModulePath()` is an
installation-relative path a build tree does not have), and every journey fails
with `wf::moduleRequirementFailure()` naming the missing variable rather than
skipping. `app-smoke` needs it for the same reason — `src/main.cpp` exits 2
when it cannot load a module, so without the variable every smoke case fails.

### 1.3 The new suite

```cmake
foreach(wf w01_first_launch w02_subscription_update w04_restore w05_recovery)
```

i.e. add `w02_subscription_update` to the existing loop (it needs no UI
sources), then:

```cmake
clash_qt_label(w02-subscription-update workflow integration app)
clash_qt_isolate_test(w02-subscription-update)
```

and add `w02-subscription-update` to the `clash_qt_isolate_test` foreach list.
`RUN_SERIAL TRUE` and `TIMEOUT 300` come from the shared loop; the suite needs
both — it takes the fixed controller port 29097, and the real-core arm below is
bounded by the module's published 10 s readiness budget.

### 1.4 The real-core arm (opt-in, off by default)

`w02_subscription_update_test.cpp` contains a second, environment-gated arm.
It is **skipped when `CLASH_QT_W02_REAL_CORE` is unset** and, when set to `1`,
requires all three of:

```cmake
CLASH_QT_W02_REAL_CORE=1
CLASH_QT_CORE_BINARY=<build>/core/mihomo
CLASH_QT_CORE_PROVENANCE=<build>/core/mihomo-provenance.json
```

Suggested registration (a separate test name over the same executable, so the
default board stays fake-core only):

```cmake
add_test(NAME w02-subscription-update-real-core
         COMMAND w02-subscription-update-tests)
set_tests_properties(w02-subscription-update-real-core PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 600 ENVIRONMENT
    "CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>;CLASH_QT_MODULE_PATH=$<TARGET_FILE:clash_qt_backend_module>;CLASH_QT_W02_REAL_CORE=1;CLASH_QT_CORE_BINARY=${CLASH_QT_CORE_DIR}/mihomo;CLASH_QT_CORE_PROVENANCE=${CLASH_QT_CORE_DIR}/mihomo-provenance.json")
clash_qt_label(w02-subscription-update-real-core workflow integration app core)
clash_qt_isolate_test(w02-subscription-update-real-core)
```

With the flag set, an absent binary, an absent manifest, a manifest that does
not pin `ab405bad5beeeac8b003bb01f60f134f6df54471`, a checksum/size that does
not match the executable, or a version the executable does not report is a
FAILURE, never a skip. An occupied controller port 29097 fails too, naming the
gate it leaves open.

## 2. `tests/CMakeLists.txt`

`clash_test_support` gains no new source file: `support/loopback_server.{h,cpp}`
grew `LoopbackServer::Reply::headers` / `withHeader()` in place, and
`loopback-server-tests` covers it from the existing registration. No change.

## 3. Nothing else

No new target, no new source directory, no packaging change, no
`architecture.json` edit requested by this lease. `tests/workflows` still has
no library of its own: `workflow_support.h` and `composition_root_audit.h`
remain header-only.

---

# Addendum — safety-correction lease (resumed W02 worker)

## 4. No new build registration is required by this lease

`app_smoke_test.cpp` now names `platform::PrivilegedServiceClient::
defaultSocketPath()` and `platform::PrivilegedServiceInstaller::isSupported()`.
Both come from `clash_platform`, which `clash_qt_workflow_libs` already lists,
so `app-smoke-tests` links unchanged. Verified by an actual link in the
external snapshot build, not by inspection.

`tests/workflows/composition_root_audit.h` stays header-only.

## 5. One registration is now belt-and-braces rather than load-bearing

```cmake
CLASH_QT_SERVICE_SOCKET=${CMAKE_BINARY_DIR}/test-data/app-smoke/helper.socket
```

Keep it. It is no longer what isolates the children: `childEnvironment()`
OVERRIDES it for every launch with a per-test path inside the suite's own
`ScopedEnvironment::socketDir()`, independent of what the registration, the
developer's shell or an earlier case exported. The registration still protects
anything the test process itself might construct, and it documents the
requirement at the place a reader looks first.

The same is true of `clash_qt_isolate_test`'s `CLASH_QT_DATA_DIR`: the harness
now always hands its own value to the child.

## 6. w02-real-core

Already registered by the coordinator (`tests/workflows/CMakeLists.txt:95-99`).
Confirmed to work against `CLASH_QT_CORE_BINARY` + `CLASH_QT_CORE_PROVENANCE`:
five consecutive green runs, `4 passed, 3 skipped` each (the three skips are
the fixture cases, which run under the `w02-subscription-update` registration).
TIMEOUT 300 is enough - the arm takes about 3 s when the engine settles at once
and its own worst case is bounded by the module's published 10 s readiness
budget.
