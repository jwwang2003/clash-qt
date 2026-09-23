cmake_minimum_required(VERSION 3.21)
# The version is declared once, here. Everything else derives from it: the bundle
# strings below, and CLASH_QT_VERSION for code that has to state which build it
# is. A second literal elsewhere is a version that will disagree with this one
# the first time only one of them is bumped.
# Pinned before project(), which is where the compiler is probed and the value
# has to already be set. Without it the floor is whatever SDK the build machine
# happens to have, so the app silently refuses to launch on anything older than
# the machine that built it, and two builds of the same commit can disagree
# about who can run them.
#
# 14.0 is not a preference, it is the floor Qt imposes: the Qt frameworks this
# links against are themselves built for 14.0, so a lower number here compiles
# and then fails to launch on the machines it claims to support. Raise it only
# when the Qt being linked requires more.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()

project(clash-qt VERSION 0.1.0 LANGUAGES CXX)

# Keep the build tree out of Spotlight and Launchpad.
#
# A built .app and a staged .app are complete applications as far as macOS is
# concerned, so it indexes them and registers them with LaunchServices. They
# then appear in Spotlight, in Launchpad and in "Open With" alongside installed
# software, several at a time, and they outlive the directories they came from:
# LaunchServices kept registrations here for four build directories that had
# been deleted, because unregistering is not something deleting a folder does.
#
# This marker is Apple's documented request not to index a directory, written at
# the top of the build tree so every preset underneath is covered, and before any
# target is declared -- configuring alone is enough to produce the problem, since
# CMake writes the bundle's Info.plist then, and a directory with an Info.plist
# and an empty MacOS/ is already an application to Launchpad.
#
# It is NOT sufficient on its own. Measured on macOS 26: with the marker in place
# and older than the bundle, a freshly created clash-qt.app was indexed and
# registered within seconds anyway. It is kept because it costs nothing and is
# the documented mechanism, not because it solves this.
#
# What actually keeps the build tree out of Spotlight is adding it to the
# Privacy list in System Settings, which has no scriptable equivalent --
# mdutil works per volume, not per directory. Failing that, `make clean` removes
# the bundles and `scripts/uninstall.sh --app` clears registrations left behind
# by build directories that no longer exist. See docs/installing.md.
if(APPLE)
    get_filename_component(clash_qt_build_root "${CMAKE_BINARY_DIR}/.." ABSOLUTE)
    foreach(dir "${CMAKE_BINARY_DIR}" "${clash_qt_build_root}")
        if(NOT EXISTS "${dir}/.metadata_never_index")
            file(TOUCH "${dir}/.metadata_never_index")
        endif()
    endforeach()
endif()


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
include(cmake/Component.cmake)
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
        clash_module_host
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
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        # The build number. Finder shows the short string; an updater compares
        # this one, and it must not go backwards between releases.
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        INSTALL_RPATH_USE_LINK_PATH TRUE
    )
    if(WIN32)
        set_target_properties(clash-qt PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()

    # So the running app can state its own version. An update check cannot ask
    # "is there something newer than me?" without it, and a bug report that
    # names a version is worth several that do not.
    target_compile_definitions(clash-qt PRIVATE
        CLASH_QT_VERSION="${PROJECT_VERSION}")

    include(cmake/Packaging.cmake)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
