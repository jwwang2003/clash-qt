# FORBIDDEN graph for R3. A backend module links Qt6::Widgets directly.
# The same project ALSO contains the two allowed near-misses -- Qt6::Gui in a
# platform adapter and Qt6::Qml in a core library -- so this case fails on
# exactly one module and proves the rule is not "anything GUI-ish".
cmake_minimum_required(VERSION 3.21)
project(arch_selftest_forbidden_qt LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml)

add_library(synth_types INTERFACE)
target_link_libraries(synth_types INTERFACE Qt6::Core)

add_library(synth_platform STATIC src/platform.cpp)
target_link_libraries(synth_platform PUBLIC synth_types Qt6::Gui)

add_library(synth_config STATIC src/config.cpp)
target_link_libraries(synth_config PUBLIC synth_types PRIVATE Qt6::Qml)

# THE VIOLATION.
add_library(synth_core STATIC src/core.cpp)
target_link_libraries(synth_core PUBLIC synth_types PRIVATE Qt6::Widgets)
