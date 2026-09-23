# Read-only prerequisite diagnostics.
#
# This is a real (tiny) CMake project rather than a `cmake -P` script, because
# find_package(Qt6) is not scriptable: Qt's config files call add_library, which
# script mode rejects. Probing the way the real build probes is the only way for
# this check to mean anything. It configures nothing else, builds nothing and
# installs nothing.
cmake_minimum_required(VERSION 3.21)
# CXX is enabled, not NONE: Qt6Config pulls in FindThreads, which refuses to run
# without a language enabled. Requiring a compiler here is correct anyway.
project(clash-qt-doctor LANGUAGES CXX)

set(problems 0)
macro(missing what detail)
    message(STATUS "  MISSING ${what}: ${detail}")
    math(EXPR problems "${problems}+1")
endmacro()

cmake_host_system_information(RESULT host_os QUERY OS_NAME)
cmake_host_system_information(RESULT host_release QUERY OS_RELEASE)
cmake_host_system_information(RESULT host_arch QUERY OS_PLATFORM)
message(STATUS "Host")
message(STATUS "  system   ${host_os} ${host_release} ${host_arch}")
message(STATUS "  cmake    ${CMAKE_VERSION}")
message(STATUS "  compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(DEFINED MAKE_VERSION_REPORT AND NOT MAKE_VERSION_REPORT STREQUAL "")
    message(STATUS "  make     ${MAKE_VERSION_REPORT} (supported floor: GNU Make 3.81)")
endif()

message(STATUS "Toolchain")
# Each tool gets the version flag it actually accepts; `ninja version` is not a
# command and would otherwise be reported as a successful probe.
foreach(spec "go;version" "git;--version" "ninja;--version")
    list(GET spec 0 tool)
    list(GET spec 1 flag)
    find_program(prog_${tool} ${tool})
    if(prog_${tool})
        execute_process(COMMAND "${prog_${tool}}" ${flag}
            OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE rc)
        string(REGEX REPLACE "\n.*" "" out "${out}")
        if(rc EQUAL 0)
            message(STATUS "  ok       ${tool}: ${out}")
        else()
            missing("${tool}" "found at ${prog_${tool}} but '${tool} ${flag}' failed")
        endif()
    else()
        missing("${tool}" "not on PATH")
    endif()
endforeach()

# Python is a declared TEST-ONLY dependency: the architecture checker uses it.
# Building and running the application needs no Python, so a missing interpreter
# is a note, not a failure -- but it must be visible, because a clean Windows
# machine does not have one.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    message(STATUS "  ok       python3: ${Python3_VERSION} (architecture tests)")
else()
    message(STATUS "  note     python3 absent -- architecture tests cannot run; app builds fine")
endif()

message(STATUS "Qt and libraries")
find_package(Qt6 6.9 QUIET COMPONENTS Core Gui Network WebSockets Qml Concurrent)
if(Qt6_FOUND)
    message(STATUS "  ok       Qt6 base: ${Qt6_VERSION}  [${Qt6_DIR}]")
    find_package(Qt6 6.9 QUIET COMPONENTS Widgets Quick Graphs)
    if(Qt6Widgets_FOUND AND Qt6Quick_FOUND AND Qt6Graphs_FOUND)
        message(STATUS "  ok       Qt6 desktop: Widgets, Quick, Graphs")
    else()
        message(STATUS "  note     Qt6 desktop modules absent -- backend-only builds still work")
        message(STATUS "           (configure with PRESET=headless)")
    endif()
else()
    missing("Qt6 6.9+ base modules" "set CMAKE_PREFIX_PATH to your Qt installation")
endif()
find_package(yaml-cpp QUIET)
if(yaml-cpp_FOUND)
    message(STATUS "  ok       yaml-cpp [${yaml-cpp_DIR}]")
    # Reporting "present" without saying which copy is how a broken build passes
    # doctor: two prefixes can each provide a dependency and fail only at link.
    if(Qt6_FOUND)
        get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
        get_filename_component(_yaml_prefix "${yaml-cpp_DIR}/../../.." ABSOLUTE)
        if(NOT _qt_prefix STREQUAL _yaml_prefix)
            message(STATUS "  WARNING  Qt and yaml-cpp come from different prefixes:")
            message(STATUS "           Qt       ${_qt_prefix}")
            message(STATUS "           yaml-cpp ${_yaml_prefix}")
            message(STATUS "           Mixing them can fail at link time. Set CMAKE_PREFIX_PATH.")
        endif()
    endif()
else()
    missing("yaml-cpp" "install it or add it to CMAKE_PREFIX_PATH")
endif()

message(STATUS "Recorded sources")
if(EXISTS "${SOURCE_ROOT}/3rdparty/mihomo/go.mod")
    find_program(git_program git)
    execute_process(COMMAND "${git_program}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_ROOT}/3rdparty/mihomo"
        OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${git_program}" status --porcelain
        WORKING_DIRECTORY "${SOURCE_ROOT}/3rdparty/mihomo"
        OUTPUT_VARIABLE dirt OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(dirt STREQUAL "")
        message(STATUS "  ok       3rdparty/mihomo at ${rev} (clean)")
    else()
        message(STATUS "  note     3rdparty/mihomo at ${rev} (dirty -- release builds will refuse this)")
    endif()
else()
    missing("3rdparty/mihomo" "submodule not initialised; run 'make setup'")
endif()

message(STATUS "")
if(problems GREATER 0)
    message(FATAL_ERROR "doctor: ${problems} prerequisite(s) missing.")
endif()
message(STATUS "doctor: all prerequisites present.")
