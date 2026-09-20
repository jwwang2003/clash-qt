# Owned by the core module. Add new core/*.cpp files here.
target_sources(clash-qt PRIVATE
    src/core/controller_discovery.cpp
    src/core/mihomo_client.cpp
    src/core/provider_client.cpp
    src/core/backup_store.cpp
    src/core/yaml_util.cpp
    src/core/traffic_history.cpp
)

include(src/core/process/sources.cmake)
include(src/core/profile/sources.cmake)
include(src/core/enhance/sources.cmake)
