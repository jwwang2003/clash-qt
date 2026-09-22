# FORBIDDEN graph for the ratchet. The baselined implementation-to-adapter edge
# (synth_impl -> synth_platform) is still here and still named in
# architecture.json. A SECOND, undeclared edge of the same shape has appeared:
# synth_yaml -> synth_platform. Baselining one edge must not make the next one
# free.
cmake_minimum_required(VERSION 3.21)
project(arch_selftest_extra_exception LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)

add_library(synth_types INTERFACE)
target_link_libraries(synth_types INTERFACE Qt6::Core)

add_library(synth_platform STATIC src/platform.cpp)
target_link_libraries(synth_platform PUBLIC synth_types Qt6::Gui)

add_library(synth_impl STATIC src/impl.cpp)
target_link_libraries(synth_impl PUBLIC synth_types synth_platform PRIVATE Qt6::Network)

# THE NEW, UNBASELINED EDGE.
add_library(synth_yaml STATIC src/yaml.cpp)
target_link_libraries(synth_yaml PUBLIC synth_types PRIVATE synth_platform)
