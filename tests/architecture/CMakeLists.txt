# Architecture checks.
#
# Registered from tests/CMakeLists.txt with a single line:
#
#     add_subdirectory(architecture)
#
# This file is also include()d directly by the public-header probe self-test in
# selftest/cases/probe, with CLASH_ARCH_DEFINE_ONLY set, so the self-test
# exercises the probe implementation itself rather than a copy of it.

set(CLASH_ARCH_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ---------------------------------------------------------------------------
# Public-header compile probe (TEST_STRATEGY 'Architecture checks' item 3).
#
# For every published header of a module, a one-line translation unit that
# includes it and nothing else is compiled into a consumer that links ONLY that
# module. The probe deliberately does not add the project's src/ directory to
# its include path: the header must be reachable through the module's own
# INTERFACE include directories, or a real client could not use it either.
# One header per translation unit, included first, so no header can rely on
# another having been included before it.
# ---------------------------------------------------------------------------
function(clash_arch_add_public_header_probes)
    cmake_parse_arguments(ARCH "" "GRAPH;PREFIX;AGGREGATE" "" ${ARGN})
    if(NOT ARCH_GRAPH)
        message(FATAL_ERROR "clash_arch_add_public_header_probes: GRAPH is required")
    endif()
    if(NOT ARCH_PREFIX)
        set(ARCH_PREFIX arch_probe_)
    endif()
    if(NOT ARCH_AGGREGATE)
        set(ARCH_AGGREGATE arch_public_header_probes)
    endif()

    file(READ "${ARCH_GRAPH}" arch_graph_json)
    string(JSON arch_module_count LENGTH "${arch_graph_json}" modules)
    if(arch_module_count EQUAL 0)
        add_custom_target(${ARCH_AGGREGATE})
        return()
    endif()

    set(arch_probe_targets "")
    set(arch_skipped "")
    math(EXPR arch_last "${arch_module_count} - 1")
    foreach(i RANGE 0 ${arch_last})
        string(JSON arch_module MEMBER "${arch_graph_json}" modules ${i})
        if(arch_module MATCHES "^_")
            continue()
        endif()
        string(JSON arch_headers ERROR_VARIABLE arch_err
               GET "${arch_graph_json}" modules "${arch_module}" public_headers)
        if(arch_err OR NOT arch_headers)
            continue()
        endif()
        if(NOT TARGET ${arch_module})
            list(APPEND arch_skipped ${arch_module})
            continue()
        endif()

        string(JSON arch_header_count LENGTH "${arch_headers}")
        if(arch_header_count EQUAL 0)
            continue()
        endif()
        math(EXPR arch_header_last "${arch_header_count} - 1")

        set(arch_dir "${CMAKE_CURRENT_BINARY_DIR}/public_header_probe/${arch_module}")
        set(arch_sources "")
        foreach(h RANGE 0 ${arch_header_last})
            string(JSON arch_header GET "${arch_headers}" ${h})
            string(REGEX REPLACE "[^A-Za-z0-9]" "_" arch_stem "${arch_header}")
            set(arch_tu "${arch_dir}/${arch_stem}.cpp")
            # The include is the very first thing in the translation unit.
            file(WRITE "${arch_tu}"
                 "#include \"${arch_header}\"\n"
                 "// Included twice: a published header must be self-contained\n"
                 "// and idempotent, not merely survive one inclusion.\n"
                 "#include \"${arch_header}\"\n")
            list(APPEND arch_sources "${arch_tu}")
        endforeach()
        file(WRITE "${arch_dir}/probe_main.cpp" "int main() { return 0; }\n")
        list(APPEND arch_sources "${arch_dir}/probe_main.cpp")

        set(arch_target "${ARCH_PREFIX}${arch_module}")
        add_executable(${arch_target} EXCLUDE_FROM_ALL ${arch_sources})
        # Only the module's own declared dependencies. No project-wide include
        # directories, no Qt modules added by hand, no AUTOMOC to paper over a
        # header that does not stand on its own.
        target_link_libraries(${arch_target} PRIVATE ${arch_module})
        set_target_properties(${arch_target} PROPERTIES
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
            FOLDER "architecture")
        list(APPEND arch_probe_targets ${arch_target})
    endforeach()

    add_custom_target(${ARCH_AGGREGATE} DEPENDS ${arch_probe_targets})
    if(arch_skipped)
        message(STATUS "architecture: public-header probe skipped (target absent): ${arch_skipped}")
    endif()
    set(${ARCH_AGGREGATE}_TARGETS "${arch_probe_targets}" PARENT_SCOPE)
endfunction()

if(CLASH_ARCH_DEFINE_ONLY)
    return()
endif()

# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)

get_filename_component(CLASH_ARCH_REPO_ROOT "${CLASH_ARCH_DIR}/../.." ABSOLUTE)
set(CLASH_ARCH_GRAPH "${CLASH_ARCH_DIR}/architecture.json")

clash_arch_add_public_header_probes(
    GRAPH "${CLASH_ARCH_GRAPH}"
    PREFIX arch_probe_
    AGGREGATE arch_public_header_probes)

# The File API query has to exist before the configure that produces the reply
# the checker reads, so it is written here and arch_check.py re-runs CMake once
# if the reply is not there yet.
file(WRITE "${CMAKE_BINARY_DIR}/.cmake/api/v1/query/client-clash-arch/query.json"
     "{\"requests\":[{\"kind\":\"codemodel\",\"version\":2}]}")

add_test(NAME arch-graph COMMAND "${Python3_EXECUTABLE}"
    "${CLASH_ARCH_DIR}/arch_check.py"
    --graph "${CLASH_ARCH_GRAPH}"
    --build-dir "${CMAKE_BINARY_DIR}"
    --repo-root "${CLASH_ARCH_REPO_ROOT}"
    --ensure-reply)

add_test(NAME arch-public-headers COMMAND "${CMAKE_COMMAND}"
    --build "${CMAKE_BINARY_DIR}" --target arch_public_header_probes)

add_test(NAME arch-selftest COMMAND "${Python3_EXECUTABLE}"
    "${CLASH_ARCH_DIR}/selftest/run_selftests.py"
    --work-dir "${CMAKE_BINARY_DIR}/arch-selftest")

set_tests_properties(arch-graph arch-public-headers arch-selftest
    PROPERTIES LABELS "architecture")
# The probe test shells out to the build system; it must not run concurrently
# with another job that is writing the same build tree.
set_tests_properties(arch-public-headers PROPERTIES RUN_SERIAL TRUE)
