# FORBIDDEN graph for the external-dependency rule, transitive arm. Nothing here links a forbidden module
# directly: the narrow telemetry leaf links one broad "umbrella" library, and
# the umbrella re-exports Qt6::Widgets and Qt6::Network PUBLIC. A checker that
# only read each target's own target_link_libraries would call this clean.
cmake_minimum_required(VERSION 3.21)
project(arch_selftest_umbrella LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

add_library(synth_types INTERFACE)
target_link_libraries(synth_types INTERFACE Qt6::Core)

add_library(synth_umbrella STATIC src/umbrella.cpp)
target_link_libraries(synth_umbrella PUBLIC synth_types Qt6::Widgets Qt6::Network)

add_library(synth_telemetry STATIC src/telemetry.cpp)
target_link_libraries(synth_telemetry PUBLIC synth_types synth_umbrella)
