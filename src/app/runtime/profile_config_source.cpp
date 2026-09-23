#include "app/runtime/profile_config_source.h"

#include "core/profiles/profile_store.h"

namespace app::runtime {

ProfileStoreConfigSource::ProfileStoreConfigSource(core::ProfileStore *profiles)
    : profiles_(profiles) {}

QString ProfileStoreConfigSource::currentSelection() const {
    return profiles_ ? profiles_->currentUid() : QString();
}

QString ProfileStoreConfigSource::workDir() const {
    return profiles_ ? profiles_->dataDir() : QString();
}

void ProfileStoreConfigSource::requestRuntimeConfig() {
    if (profiles_) profiles_->requestRuntimeConfig();
}

}  // namespace app::runtime
