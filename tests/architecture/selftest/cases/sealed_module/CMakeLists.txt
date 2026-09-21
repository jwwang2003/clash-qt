# SEALED-MODULE graph: the shape of the G2 link ledger in miniature.
#
# synth_impl stands in for clash_mihomo_impl, the component-private module that
# consumers are not supposed to link. Two executables link it directly, and a
# third does not. The point of the case is not the graph -- check_targets()
# would be happy with several different declarations of it -- but whether the
# LEDGER is forced to describe the graph:
#
#   synth_app    a declared consumer that links the sealed module
#   synth_tool   an UNdeclared executable that links it too. Undeclared
#                executables are invisible to the consumer allowlist, which is
#                how three P3-created targets linked clash_mihomo_impl without
#                appearing anywhere in the exception ledger.
#   synth_probe  links only the OS adapter. It exists so that a rule which
#                flagged everything would fail this case.
cmake_minimum_required(VERSION 3.21)
project(arch_selftest_sealed LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Gui)

add_library(synth_types INTERFACE)
target_include_directories(synth_types INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(synth_types INTERFACE Qt6::Core)

add_library(synth_platform STATIC src/types.cpp)
target_link_libraries(synth_platform PUBLIC synth_types Qt6::Gui)

# The sealed module.
add_library(synth_impl STATIC src/impl.cpp)
target_link_libraries(synth_impl PUBLIC synth_types)

add_executable(synth_app src/app.cpp)
target_link_libraries(synth_app PRIVATE synth_impl)

add_executable(synth_tool src/tool.cpp)
target_link_libraries(synth_tool PRIVATE synth_impl)

add_executable(synth_probe src/probe.cpp)
target_link_libraries(synth_probe PRIVATE synth_platform)
