# FORBIDDEN graph for the acyclicity rule. CMake happily configures a static-library cycle and
# simply repeats the archives on the link line, so nothing in the build tells
# you it is there. Both edges ARE declared in architecture.json, so the module-edge rule passes
# and only the acyclicity rule can fail this case.
cmake_minimum_required(VERSION 3.21)
project(arch_selftest_cycle LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core)

add_library(synth_types INTERFACE)
target_link_libraries(synth_types INTERFACE Qt6::Core)

add_library(synth_a STATIC src/a.cpp)
add_library(synth_b STATIC src/b.cpp)
target_link_libraries(synth_a PUBLIC synth_types synth_b)
target_link_libraries(synth_b PUBLIC synth_types synth_a)
