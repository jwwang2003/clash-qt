# P4 binary boundary. Included after src/core defines the host-side contracts.
add_library(clash_component_abi INTERFACE)
target_link_libraries(clash_component_abi INTERFACE clashqt_com)
target_sources(clash_component_abi INTERFACE FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        "${PROJECT_SOURCE_DIR}/src/core/component/abi/abi.h"
        "${PROJECT_SOURCE_DIR}/src/core/component/abi/backend_abi.h"
        "${PROJECT_SOURCE_DIR}/src/core/component/abi/module_entry.h"
        "${PROJECT_SOURCE_DIR}/src/core/component/abi/target_abi.h"
        "${PROJECT_SOURCE_DIR}/src/core/component/abi/wire.h")

# Private codecs are compiled into both sides. Symbols stay local, and buffers
# are destroyed through their producing side's COM vtable.
add_library(clash_component_marshal STATIC
    src/integrations/component/marshal/backend_marshal.cpp
    src/integrations/component/marshal/codec.cpp
    src/integrations/component/marshal/com_objects.cpp
    src/integrations/component/marshal/connection_snapshot.cpp
    src/integrations/component/marshal/runtime_tag.cpp)
target_link_libraries(clash_component_marshal PUBLIC clash_component_abi clash_backend Qt6::Core)
set_target_properties(clash_component_marshal PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

set(clash_component_module_sources
    src/core/mihomo/module/backend_session.cpp
    src/core/mihomo/module/backend_module.cpp
    src/core/mihomo/module/host_privileged_service.cpp
    src/core/mihomo/module/module_export.cpp)

# Compile the supervisor implementation into the binary module itself. The
# private static target remains available only to its implementation suites.
add_library(clash_qt_backend_module SHARED
    ${clash_component_module_sources}
    src/core/mihomo/module/mihomo_module_entry.cpp
    src/core/mihomo/mihomo_backend.cpp
    src/core/mihomo/controller_discovery.cpp
    src/core/mihomo/mihomo_client.cpp
    src/core/mihomo/provider_client.cpp
    src/core/mihomo/process/core_process.cpp)
target_link_libraries(clash_qt_backend_module PRIVATE
    clash_component_marshal clash_backend clash_yaml
    Qt6::Network Qt6::WebSockets Qt6::Concurrent ${CMAKE_DL_LIBS})
set_target_properties(clash_qt_backend_module PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules")

add_library(clash_module_host STATIC
    src/integrations/component/module_loader.cpp
    src/integrations/component/module_backend.cpp)
target_link_libraries(clash_module_host PUBLIC clash_backend clash_component_abi
    PRIVATE clash_component_marshal ${CMAKE_DL_LIBS})
target_sources(clash_module_host PUBLIC FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES src/integrations/component/module_loader.h src/integrations/component/module_backend.h)

# Public headers can be copied to an independent consumer without exposing the
# Qt-facing backend or the private engine implementation.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/core/component/"
    DESTINATION "include/clash-qt/core/component"
    COMPONENT component-sdk FILES_MATCHING PATTERN "*.h")
