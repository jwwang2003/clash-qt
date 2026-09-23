# FORBIDDEN graph for the module-edge rule: a low-level module links a higher-level one.
# Identical to the allowed case except for the one extra edge below.
cmake_minimum_required(VERSION 3.21)
project(arch_selftest_forbidden_edge LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)

add_library(synth_types INTERFACE)
target_include_directories(synth_types INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(synth_types INTERFACE Qt6::Core)

add_library(synth_platform STATIC src/platform.cpp)
target_link_libraries(synth_platform PUBLIC synth_types Qt6::Gui)

add_library(synth_impl STATIC src/impl.cpp)
target_link_libraries(synth_impl PUBLIC synth_types PRIVATE Qt6::Network)

# THE VIOLATION: the telemetry leaf reaches up into the implementation library.
add_library(synth_telemetry STATIC src/telemetry.cpp)
target_link_libraries(synth_telemetry PUBLIC synth_types PRIVATE synth_impl)
