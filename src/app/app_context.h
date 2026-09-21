#pragma once

namespace core {
class ConfigEnhancer;
class CoreProcess;
class MihomoClient;
class ProfileStore;
}

namespace platform {
class Hotkeys;
}

namespace app {

/// The long-lived objects the UI is built against. Passed by reference so
/// adding a service does not reshape every widget constructor.
struct Context {
    core::MihomoClient *client = nullptr;
    core::ProfileStore *profiles = nullptr;
    core::CoreProcess *coreProcess = nullptr;
    core::ConfigEnhancer *enhancer = nullptr;
    platform::Hotkeys *hotkeys = nullptr;
};

}  // namespace app
