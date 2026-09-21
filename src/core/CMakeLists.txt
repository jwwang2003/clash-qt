# Reusable backend libraries. Each owns one responsibility and declares only the
# dependencies its code actually has; see .refactor/DECISIONS.md D1.

# Shared value types. Header-only, so everything can depend on it without any
# target depending on another implementation library. This is what keeps the
# component-private mihomo library and the UI from having to share a home.
add_library(clash_types INTERFACE)
target_sources(clash_types INTERFACE FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src" FILES types.h)
target_include_directories(clash_types INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clash_types INTERFACE Qt6::Core)

# YAML helpers. A leaf of its own rather than part of clash_config, so the
# separately packaged component never has to drag the configuration library in.
add_library(clash_yaml STATIC config/yaml_util.cpp)
target_link_libraries(clash_yaml PUBLIC clash_types yaml-cpp::yaml-cpp)

add_library(clash_config STATIC config/enhance/config_enhancer.cpp)
target_link_libraries(clash_config
    PUBLIC  clash_types clash_yaml
    PRIVATE Qt6::Qml          # QJSEngine runs the profile enhancement scripts
            Qt6::Concurrent)

add_library(clash_profiles STATIC profiles/profile_store.cpp)
target_link_libraries(clash_profiles
    PUBLIC  clash_types clash_config
    PRIVATE Qt6::Network Qt6::Concurrent yaml-cpp::yaml-cpp)

add_library(clash_backups STATIC backups/backup_store.cpp)
# Uses QThread + QSemaphore directly, not QtConcurrent: the old backup-tests
# target linked Qt6::Concurrent unnecessarily.
target_link_libraries(clash_backups
    PUBLIC  clash_types
    PRIVATE Qt6::Network yaml-cpp::yaml-cpp)

add_library(clash_telemetry STATIC telemetry/traffic_history.cpp)
target_link_libraries(clash_telemetry PUBLIC clash_types)

# Component-private per G2: the UI must reach the engine through the published
# component interface, never by linking this directly. The architecture checker
# enforces that once the component boundary exists.
add_library(clash_mihomo_impl STATIC
    mihomo/controller_discovery.cpp
    mihomo/mihomo_client.cpp
    mihomo/provider_client.cpp
    mihomo/process/core_process.cpp)
target_link_libraries(clash_mihomo_impl
    PUBLIC  clash_types
            # BASELINED EXCEPTION (decision D2, owner P3/MOD-CORE):
            # platform::PrivilegedServiceClient appears in CoreProcess's
            # constructor signature, so this edge is PUBLIC today. It is removed
            # by injecting an abstract interface owned by this library.
            clash_platform
    PRIVATE Qt6::Network Qt6::WebSockets Qt6::Concurrent clash_yaml)
