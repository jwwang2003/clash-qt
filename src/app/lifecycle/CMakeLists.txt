# Application lifecycle services (MOD-LIFECYCLE). Startup and shutdown
# coordination lifted out of src/main.cpp.
#
# The composition root adds this with
#   add_subdirectory(src/app/lifecycle)
# and links clash_app_lifecycle into clash-qt. Nothing else in the tree needs
# it; the dependency direction is composition root -> lifecycle -> backend
# contract, never the reverse.
#
# DEPENDENCIES, deliberately minimal:
#   clash_backend  the published MihomoBackend contract (header-only).
#   Qt6::Core      QObject, QTimer, QThreadPool, QEvent.
# It links neither clash_platform nor clash_profiles nor Qt6::Widgets: the
# system proxy, the profile stores and every dialog reach it through the ports
# in shutdown_ports.h, which is what lets the quit gate be tested by holding and
# releasing each condition rather than by waiting for one.

add_library(clash_app_lifecycle STATIC
    quit_guard.cpp
    shutdown_ports.cpp
    shutdown_coordinator.cpp)

target_sources(clash_app_lifecycle PUBLIC FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        quit_guard.h
        shutdown_ports.h
        shutdown_coordinator.h)

target_include_directories(clash_app_lifecycle PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clash_app_lifecycle PUBLIC clash_backend Qt6::Core)
