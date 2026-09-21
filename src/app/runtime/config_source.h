#ifndef CLASHQT_APP_RUNTIME_CONFIG_SOURCE_H
#define CLASHQT_APP_RUNTIME_CONFIG_SOURCE_H

// The configuration side of a reload, as RuntimeCoordinator needs it.
//
// PRE-ARCH section 4b group B is a conversation between three parties: the
// backend (what the managed core is doing), the profile store (what the user
// selected and where its runtime configuration lands) and a timer. Only the
// first is published as a contract. This port is the second, reduced to the
// three questions main.cpp actually asks of core::ProfileStore:
//
//   main.cpp:144  profiles->currentUid().isEmpty()   -> currentSelection()
//   main.cpp:133  profiles->dataDir()                -> workDir()
//   main.cpp:146  profiles->requestRuntimeConfig()   -> requestRuntimeConfig()
//
// It is a port, not a second contract: ProfileStoreConfigSource is the only
// production implementation and it forwards verbatim. It exists so that the
// reload gate, the debounce and the snapshot retention rules can be tested
// without a real profile store generating real YAML on a real thread pool -
// CFG-CORE owns that object and is reshaping it in parallel.
//
// The runtime configuration itself is delivered back asynchronously, through
// RuntimeCoordinator::onRuntimeConfigReady(): a ProfileStore signal in
// production, a direct call in a test.
//
// WHAT THIS PORT DELIBERATELY DOES NOT CARRY: the geo-data seed directory.
// ProfileStore::prepareRuntime() needs one, and this port looks like the place
// to hand it over, since ProfileStoreConfigSource is the adapter that drives
// generation. It is not, for two reasons.
//
//   * It would not reach every generation. This port drives one of four
//     entry points into ProfileStore's generator; main_window.cpp and
//     settings_page.cpp call requestRuntimeConfig() on the store directly, and
//     generateRuntimeConfig() is a synchronous path with no coordinator at all.
//     A seed directory that rode in on this call would be absent from three of
//     the four, and the symptom -- geo data quietly not copied -- is invisible
//     until the engine is already running without it.
//   * It would relocate the dependency rather than remove it. Answering the
//     question here means clash_app_runtime asking core/mihomo where the
//     engine's configuration lives, which is the same edge one module further
//     up, and clash_app_runtime is linked by the application too.
//
// The seed directory is store configuration, not a property of a reload, so the
// composition root sets it once on the store (ProfileStore::setSeedDir).

#include <QString>

namespace app::runtime {

class ConfigSource {
  public:
    virtual ~ConfigSource() = default;

    // The selected profile's uid, empty when nothing is selected. An empty
    // selection means a reload STOPS the core rather than regenerating a
    // configuration (main.cpp:143-147), and it also gates autostart.
    virtual QString currentSelection() const = 0;

    // The working directory a managed core is launched in - ProfileStore's
    // data directory. Passed straight to BackendLifecycle::start().
    virtual QString workDir() const = 0;

    // Asks for a fresh runtime configuration. Answered, eventually, by a call
    // to RuntimeCoordinator::onRuntimeConfigReady(). May be answered never:
    // generation can fail or be cancelled, and the coordinator holds no state
    // that assumes an answer arrives.
    virtual void requestRuntimeConfig() = 0;
};

}  // namespace app::runtime

#endif  // CLASHQT_APP_RUNTIME_CONFIG_SOURCE_H
