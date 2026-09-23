# Local-source mihomo engine. The managed engine is always built from
# 3rdparty/mihomo at the recorded submodule commit; there is deliberately no
# fallback to PATH, to another Clash installation, or to a binary download.
set(CLASH_QT_MIHOMO_SOURCE "${PROJECT_SOURCE_DIR}/3rdparty/mihomo"
    CACHE PATH "Local mihomo source checkout")
set(CLASH_QT_CORE_DIR "${CMAKE_BINARY_DIR}/core")
if(WIN32)
    set(CLASH_QT_CORE_BINARY "${CLASH_QT_CORE_DIR}/mihomo.exe")
else()
    set(CLASH_QT_CORE_BINARY "${CLASH_QT_CORE_DIR}/mihomo")
endif()

find_program(CLASH_QT_GO_EXECUTABLE go)

# Depend on the recorded submodule pointer, so a dependency change rebuilds the
# engine rather than silently reusing another revision's binary.
set(mihomo_stamp "${PROJECT_SOURCE_DIR}/.git/modules/3rdparty/mihomo/HEAD")
if(NOT EXISTS "${mihomo_stamp}")
    set(mihomo_stamp "${CLASH_QT_MIHOMO_SOURCE}/go.mod")
endif()

add_custom_command(
    OUTPUT "${CLASH_QT_CORE_BINARY}"
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CLASH_QT_MIHOMO_SOURCE}
        -DOUTPUT=${CLASH_QT_CORE_BINARY}
        -DGO=${CLASH_QT_GO_EXECUTABLE}
        -DRELEASE=$<IF:$<CONFIG:Release>,ON,OFF>
        -P "${PROJECT_SOURCE_DIR}/scripts/build/build_mihomo.cmake"
    DEPENDS "${mihomo_stamp}" "${PROJECT_SOURCE_DIR}/scripts/build/build_mihomo.cmake"
    COMMENT "Building mihomo from local source"
    VERBATIM)

add_custom_target(clash-qt-core DEPENDS "${CLASH_QT_CORE_BINARY}")

if(NOT CLASH_QT_GO_EXECUTABLE)
    add_custom_target(clash-qt-core-missing-go
        COMMAND ${CMAKE_COMMAND} -E echo
            "Go toolchain not found. The managed engine is built from source; install Go and re-run 'make doctor'."
        COMMAND ${CMAKE_COMMAND} -E false)
    add_dependencies(clash-qt-core clash-qt-core-missing-go)
endif()
