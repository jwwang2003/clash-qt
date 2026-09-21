cmake_minimum_required(VERSION 3.21)
project(clash-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTORCC ON)

option(CLASH_QT_BUILD_APP "Build the desktop application and its UI tests" ON)

# Backend libraries need no desktop Qt. Discovering Widgets/Quick/Graphs
# unconditionally would make a headless backend build impossible on a machine
# that has only the base modules, so the desktop components are requested
# separately and only when the application is being built.
find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Network WebSockets Qml Concurrent)
find_package(yaml-cpp REQUIRED)
if(CLASH_QT_BUILD_APP)
    find_package(Qt6 6.9 REQUIRED COMPONENTS Widgets Quick Graphs)
endif()

qt_standard_project_setup(REQUIRES 6.9)

include(cmake/TrafficGraph.cmake)
include(cmake/Mihomo.cmake)

if(APPLE)
    enable_language(OBJCXX)
    add_subdirectory(src/services/macos)
endif()

add_subdirectory(src/platform)
add_subdirectory(src/core)
add_subdirectory(src/app/lifecycle)
add_subdirectory(src/app/backup)
add_subdirectory(src/app/runtime)
add_subdirectory(src/app/composition)

if(CLASH_QT_BUILD_APP)
    qt_add_executable(clash-qt src/main.cpp)
    target_link_libraries(clash-qt PRIVATE
        clash_profiles
        clash_backups
        clash_telemetry
        clash_mihomo_impl
        clash_backend_bridge
        clash_platform
        clash_config
        clash_preferences
        clash_app_lifecycle
        clash_app_backup
        clash_app_runtime
        clash_app_composition
        Qt6::Widgets
        Qt6::Network
        Qt6::WebSockets
        Qt6::Concurrent
        Qt6::Qml          # QJSEngine, for the profile enhancement scripts
    )

    # The UI stays inside the executable for now. Extracting it into its own
    # target waits until its test substitution points are explicit, so that
    # native resource linking stays verifiable at each step.
    set(clash_ui_target clash-qt)
    include(src/ui/sources.cmake)
    add_traffic_graph_module(clash-qt)

    set_target_properties(clash-qt PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.clash-qt.desktop"
        MACOSX_BUNDLE_BUNDLE_NAME "clash-qt"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        INSTALL_RPATH_USE_LINK_PATH TRUE
    )
    if(WIN32)
        set_target_properties(clash-qt PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()

    include(cmake/Packaging.cmake)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
