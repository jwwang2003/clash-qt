#ifndef CLASHQT_APP_RUNTIME_PROFILE_CONFIG_SOURCE_H
#define CLASHQT_APP_RUNTIME_PROFILE_CONFIG_SOURCE_H

// The one production implementation of ConfigSource: core::ProfileStore.
//
// Three forwarding calls and nothing else. It is a separate translation unit so
// that the only edge from app/runtime to clash_profiles is this file, and so a
// test of the reload rules does not have to build a profile store.

#include <QString>

#include "app/runtime/config_source.h"

namespace core {
class ProfileStore;
}

namespace app::runtime {

class ProfileStoreConfigSource final : public ConfigSource {
  public:
    // `profiles` must outlive this adapter. Not owned.
    explicit ProfileStoreConfigSource(core::ProfileStore *profiles);

    QString currentSelection() const override;
    QString workDir() const override;
    void requestRuntimeConfig() override;

  private:
    core::ProfileStore *profiles_;
};

}  // namespace app::runtime

#endif  // CLASHQT_APP_RUNTIME_PROFILE_CONFIG_SOURCE_H
