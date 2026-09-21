# Owned by the core module. Add new core/*.cpp files here.
target_sources(clash-qt PRIVATE
    src/core/mihomo/controller_discovery.cpp
    src/core/mihomo/mihomo_client.cpp
    src/core/mihomo/provider_client.cpp
    src/core/backups/backup_store.cpp
    src/core/config/yaml_util.cpp
    src/core/telemetry/traffic_history.cpp
)

include(src/core/mihomo/process/sources.cmake)
include(src/core/profiles/sources.cmake)
include(src/core/config/enhance/sources.cmake)
