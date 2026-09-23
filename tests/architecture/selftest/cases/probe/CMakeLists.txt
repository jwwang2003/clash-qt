# Public-header probe self-test. It include()s the real
# tests/architecture/CMakeLists.txt, so what runs here is the shipped probe
# implementation, not a copy of it.
#
#   synth_good  -- a published header that stands on its own          -> probe builds
#   synth_bad   -- a published header that needs a private header and
#                  a particular include order first                   -> probe MUST fail
#
# Both libraries themselves compile cleanly: their own .cpp includes the
# private header first, which is exactly how this class of defect hides.
cmake_minimum_required(VERSION 3.21)
project(arch_selftest_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_library(synth_good STATIC src/good/good.cpp)
target_include_directories(synth_good PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(synth_bad STATIC src/bad/bad.cpp)
# The published header is reachable, but the directory holding the private
# header it silently depends on is PRIVATE -- so a consumer gets the header and
# cannot compile it.
target_include_directories(synth_bad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(synth_bad PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/bad/detail")

set(CLASH_ARCH_DEFINE_ONLY TRUE)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../CMakeLists.txt")

clash_arch_add_public_header_probes(
    GRAPH "${CMAKE_CURRENT_SOURCE_DIR}/architecture.json"
    PREFIX arch_probe_
    AGGREGATE arch_public_header_probes)
