Historical verdict; preference-causality finding 1 is withdrawn and superseded
by the independent attribution correction in this directory. Functional GO stands.

Audit complete. Repository untouched; all writes confined to my scratch.

## P4 independent audit — **GO**

**Proven by me, not accepted.** Mutations on an external copy (`mutate/src`, restored, `diff -rq` clean):

- **Integration + newly-discovered transport case:** reverting the same-address session boundary (`src/core/mihomo/mihomo_client.cpp:124`) failed 5 cases, including `sharedReplacementAtTheSameAddressOpensANewSession` on **both** `direct-real` and `module-real`, plus retired-stream and drop-disconnect cases.
- **Boundary:** deleting the runtime-tag clause (`src/core/component/abi/module_entry.h:157`) failed `theHandshakeRefusesAMismatchedRuntime`.
- **Config:** dropping profile-scoped presets (`src/core/config/config_composer.cpp:234`) failed the composer, `preset-store` and the UI preview's "same composition the runtime would get" — the preview is real, not synthetic.

**Artifacts:** module exports exactly `_clashqt_component_module_entry`; app symbol table has **0** `MihomoBackendImpl`/`CoreProcess`/`MihomoClient`; staged module deps all `@loader_path`; `codesign --verify --deep --strict` OK; engine sha matches its manifest. My own SDK consumer, built from the **installed** headers and linking zero project libraries, drove the pinned engine to ready → **confirmed stop** → **image unmapped**, exit 0. Launching the staged app with no overrides mapped `Contents/Frameworks/libclash_qt_backend_module.dylib`, no Homebrew/Anaconda images, no helper socket. 17 shared cases × exactly 4 subjects = 68 rows, 105 cases, **0 skips/XFAIL**. Benchmark reproduced (packed/naive 0.853, Debug) with no speed assertion. Both G2 migration exceptions deleted; permanent six-site allowance intact. macOS arm64 only; Windows/Linux **zero** evidence.

**Findings (disclose, none product-blocking):**
1. `tests/support/scoped_environment.cpp:187` — the harness opens the real `QSettings("clash-qt","clash-qt")` domain every scope. **I changed the user's plist**: `681784…` → `82a0d1e4…` (my first ctest, with the guard firing), then it drifted to `204612a8…` with nothing running. Key set unchanged (one `window.geometry`); no product write possible in my UI-less build. The before/after hash invariant is therefore unsound, and the record's "After: 681784…" is stale.
2. `src/core/component/abi/module_entry.h:161` — runtime tag matches the Qt *build*, not *image*; two `QtCore` copies handshake successfully and then misbehave.
3. `examples/component_consumer/CMakeLists.txt:9` — default include root is the whole `src/`; only the installed SDK is genuinely isolated.
4. `src/integrations/component/module_backend.cpp:432` — surviving mutant, honestly documented.

Notes/logs: `/tmp/clash-qt-p4.w0Y8Uo/final-audit/`.

STABLE
