# Reusable backend libraries. Each owns one responsibility and declares only the
# dependencies its code actually has; see .refactor/DECISIONS.md D1.

# Published MihomoBackend contract, revision backend-r3. Header-only: it is the
# boundary the application codes against, and both the real backend and the fake
# implement it. Depends on clash_types alone.
add_library(clash_backend INTERFACE)
target_sources(clash_backend INTERFACE FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        backend/attachment.h backend/backend.h backend/capabilities.h
        backend/control.h backend/lifecycle.h backend/observer.h
        backend/privileged_core_service.h
        backend/telemetry.h backend/types.h)
target_include_directories(clash_backend INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clash_backend INTERFACE clash_types)

# Qt-native view of the backend contract. BackendObserver is a plain non-QObject,
# but ~66 UI call sites connect to signals; without this bridge the G2 migration
# cannot start. Deliberately NOT itself a BackendObserver: 23 callbacks share a
# name with their signal, which would make &BackendBridge::proxiesUpdated
# ambiguous at every migrated call site. The sink is a nested member.
qt_add_library(clash_backend_bridge STATIC backend/backend_bridge.cpp)
target_sources(clash_backend_bridge PUBLIC FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src" FILES backend/backend_bridge.h)
set_target_properties(clash_backend_bridge PROPERTIES AUTOMOC ON AUTOUIC OFF AUTORCC OFF)
target_include_directories(clash_backend_bridge PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clash_backend_bridge PUBLIC clash_backend Qt6::Core)

# Portable component object model, contract revision component-r1. Deliberately
# links nothing -- not even Qt6::Core -- because it is the foundation every
# component boundary is expressed in; a Qt type in a published signature would
# defeat the point. Verified: the archive has zero undefined symbols.
add_library(clashqt_com STATIC component/component.cpp)
target_sources(clashqt_com PUBLIC FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        component/buffer.h component/com_ptr.h component/component.h
        component/component_module.h component/error_info.h
        component/interface_id.h component/object.h
        component/object_support.h component/result.h)
target_include_directories(clashqt_com PUBLIC "${PROJECT_SOURCE_DIR}/src")
# qt_standard_project_setup turns AUTOMOC on for every target. This one has no
# Q_OBJECT and must not acquire a Qt code path at all, so opt it back out.
set_target_properties(clashqt_com PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

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
    # PRE-ARCH edge 1 is GONE: the composition root supplies seedDir, so this
    # library no longer reaches into the component-private implementation for it.
    # That also removed the Qt Gui and WebSockets creep it was dragging in.
    PRIVATE Qt6::Network Qt6::Concurrent yaml-cpp::yaml-cpp)

# The one accessor for the application's preference store. A leaf beside
# clash_yaml: Qt6::Core and nothing else in the project, so clash_backups, the UI
# and the composition root can each read a setting without depending on one
# another. Without CLASH_QT_DATA_DIR it is byte-for-byte the previous native
# store, so existing users keep their settings.
add_library(clash_preferences STATIC preferences/preferences.cpp)
target_include_directories(clash_preferences PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clash_preferences PUBLIC Qt6::Core)

add_library(clash_backups STATIC backups/backup_store.cpp)
# Uses QThread + QSemaphore directly, not QtConcurrent: the old backup-tests
# target linked Qt6::Concurrent unnecessarily.
target_link_libraries(clash_backups
    PUBLIC  clash_types
    PRIVATE Qt6::Network yaml-cpp::yaml-cpp clash_preferences)

add_library(clash_telemetry STATIC telemetry/traffic_history.cpp)
target_link_libraries(clash_telemetry PUBLIC clash_types)

# Component-private per G2: the UI must reach the engine through the published
# component interface, never by linking this directly. The architecture checker
# enforces that once the component boundary exists.
add_library(clash_mihomo_impl STATIC
    mihomo/mihomo_backend.cpp
    mihomo/controller_discovery.cpp
    mihomo/mihomo_client.cpp
    mihomo/provider_client.cpp
    mihomo/process/core_process.cpp)
# D2 is DISCHARGED: CoreProcess now takes core::PrivilegedCoreService, an
# interface this library owns and publishes through clash_backend. The concrete
# platform client is adapted in the composition root, so no platform type
# appears in this component's surface and the edge is gone, not baselined.
target_link_libraries(clash_mihomo_impl
    PUBLIC  clash_types clash_backend
    PRIVATE Qt6::Network Qt6::WebSockets Qt6::Concurrent clash_yaml)
