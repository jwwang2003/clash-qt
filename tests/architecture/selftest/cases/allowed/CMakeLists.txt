# ALLOWED graph: the real architecture in miniature. The checker must pass.
#
# It deliberately contains the two patterns that must NOT be flagged -- a
# platform adapter with Qt6::Gui in its public interface, and a core library
# using Qt6::Qml -- plus one baselined exception edge from a component-private
# implementation onto an OS adapter. If a careless rule banned "anything
# QML-ish" or all of Qt Gui, this case would fail, which is exactly what it is
# here to prevent.
cmake_minimum_required(VERSION 3.21)
project(arch_selftest_allowed LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml Network)

add_library(synth_types INTERFACE)
target_include_directories(synth_types INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(synth_types INTERFACE Qt6::Core)

add_library(synth_yaml STATIC src/yaml.cpp)
target_link_libraries(synth_yaml PUBLIC synth_types)

add_library(synth_config STATIC src/config.cpp)
target_link_libraries(synth_config PUBLIC synth_types synth_yaml PRIVATE Qt6::Qml)

add_library(synth_telemetry STATIC src/telemetry.cpp)
target_link_libraries(synth_telemetry PUBLIC synth_types)

# Qt6::Gui PUBLIC, as clash_platform does for QKeySequence in hotkeys.h.
add_library(synth_platform STATIC src/platform.cpp)
target_link_libraries(synth_platform PUBLIC synth_types Qt6::Gui)

# The baselined edge: component-private implementation onto the OS adapter.
add_library(synth_impl STATIC src/impl.cpp)
target_link_libraries(synth_impl PUBLIC synth_types synth_platform PRIVATE Qt6::Network)

add_executable(synth_app src/main.cpp)
target_link_libraries(synth_app PRIVATE synth_impl synth_config synth_telemetry Qt6::Widgets)
