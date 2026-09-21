#pragma once

namespace core {
class ConfigEnhancer;
class ProfileStore;
}

namespace platform {
class Hotkeys;
}

namespace app::backup {
class BackupCoordinator;
}

namespace app {

/// The long-lived objects the UI is built against. Passed by reference so
/// adding a service does not reshape every widget constructor.
///
/// `client` and `coreProcess` are GONE. core::MihomoBackendImpl owns its client
/// and its process privately and publishes neither, so there is no pointer to
/// put here; the UI reaches the engine through core::backend::BackendBridge,
/// which the composition root injects into the window and the pages directly
/// (G2). Nothing in src/ui/** read either field by the time they were removed.
struct Context {
    core::ProfileStore *profiles = nullptr;
    core::ConfigEnhancer *enhancer = nullptr;
    platform::Hotkeys *hotkeys = nullptr;
    /// The one backup coordinator in the process. BackupPage also takes it as a
    /// constructor parameter - the field is what lets a page built somewhere
    /// else find it without the widget-tree search main.cpp:182 used to do.
    app::backup::BackupCoordinator *backups = nullptr;
};

}  // namespace app
