# Inversion sweep -- P4 CFG-CORE

| inversion | verdict | observed |
|---|---|---|
| `I1-controller-reassertion` | **DETECTED** | 1/1 Test #4: config-composer ..................Subprocess aborted***Exception:   0.20 sec; FAIL!  : ConfigComposerTest::noPresetCanSelectItsOwnControllerSecretOrUiDirectory() Caught unhandled exception; 4 - config-composer (Subprocess aborted) |
| `I2-precedence-order` | **DETECTED** | FAIL!  : ConfigComposerTest::everyLayerOverridesTheOneBelowIt() Compared values are not the same |
| `I3-merge-is-deep` | **DETECTED** | 1/1 Test #4: config-composer ..................Subprocess aborted***Exception:   0.33 sec; FAIL!  : ConfigComposerTest::mergeIsDeepForMapsAndReplacingForSequences() Caught unhandled exception; 4 - config-composer (Subprocess aborted) |
| `I4-defaults-respect-choices` | **DETECTED** | FAIL!  : ConfigComposerTest::tunDefaultsAreFilledInButNeverOverrideAChoice() '!chosen["tun"]["auto-route"].as<bool>()' returned FALSE. () |
| `I5-prepend-append-ends` | **DETECTED** | FAIL!  : ConfigComposerTest::prependAndAppendPutRulesAtTheRightEnd() Compared values are not the same |
| `I6-false-is-a-value` | **DETECTED** | FAIL!  : ConfigComposerTest::explicitFalseZeroAndEmptyAreValuesNotAbsences() Compared values are not the same |
| `I7-composition-is-pure` | **DETECTED** (second attempt) | First attempt BUILD-FAILED: `compose()` could not be made to write a file without also adding `#include <QFile>`, because the TU has no file-I/O header. With the include added as part of the mutation: `FAIL!  : ConfigComposerTest::composingTouchesNoFile() 'entries.isEmpty()' returned FALSE. (composer-leak.tmp)` |
| `I8-lastgood-recovery` | **DETECTED** | FAIL!  : PresetStoreTest::amalformedFileIsRecoveredFromTheLastGoodCopyWithADiagnostic() Compared values are not the same; FAIL!  : PresetStoreTest::astructurallyInvalidFileIsRecoveredToo() Compared values are not the same |
| `I9-rejection-is-total` | **DETECTED** | FAIL!  : PresetStoreTest::arejectedDocumentChangesNothingAtAll() Compared values are not the same |
| `I10-preview-writes-nothing` | **DETECTED** | FAIL!  : EffectiveConfigPreviewTest::apreviewWritesNothingAnywhereInTheDataDirectory() Compared lists have different sizes. |
| `I11-stale-suppression` | **DETECTED** | FAIL!  : EffectiveConfigPreviewTest::onlyTheNewestRequestIsAnswered() Compared values are not the same; FAIL!  : EffectiveConfigPreviewTest::aneditDuringAPreviewSupersedesItRatherThanLeakingIntoIt() Compared values are not the same |
| `I12-chain-is-snapshotted` | **DETECTED** | FAIL!  : ChainSnapshotTest::asnapshotFreezesContentsAndTheLiveChainDoesNot() 'YAML::Load(snapshotted.yaml.toStdString())["ipv6"].as<bool>()' returned FALSE. () |

Method: each inversion patches ONE line in the external snapshot (`/tmp/clash-qt-p4.w0Y8Uo/cfg/snapshot`), rebuilds, runs only the suite that should notice, then reverts from the repository copy. Driver: `invert.py`. The repository tree was never built.

I1 and I3 are recorded as detected via an unhandled `YAML::InvalidNode`: with the defect present the expected key is absent, so `.as<T>()` throws and the suite aborts. The failure is unambiguous and attributed to the named test function, but it is a crash rather than a clean comparison failure -- noted rather than hidden.

Plist `com.clash-qt.clash-qt.plist` hashed before and after every single run in `plist-hashes.log`: one distinct value throughout, `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`.
