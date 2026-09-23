# Composition-root adapters. This is the only layer allowed to see both sides of
# the privileged seam: IR-CORE-NOT-PLATFORM keeps platform headers out of
# src/core/**, and IR-PLATFORM-NOT-CORE keeps core headers out of
# src/platform/**. Neither side may name the other's types, so the adaptation
# between them has to happen somewhere that is allowed to see both: here. It is
# a library rather than main.cpp so it can be built and checked independently.
add_library(clash_app_composition INTERFACE)
target_sources(clash_app_composition INTERFACE FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES privileged_service_adapter.h)
target_include_directories(clash_app_composition INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clash_app_composition INTERFACE clash_backend clash_platform)
