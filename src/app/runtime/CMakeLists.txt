# Application runtime services (MOD-RUNTIME). Reload scheduling, snapshot
# retention and routing coordination lifted out of src/main.cpp - PRE-ARCH
# section 4b groups B, C and D.
#
# The composition root adds this with
#   add_subdirectory(src/app/runtime)
# and links clash_app_runtime into clash-qt. The dependency direction is
# composition root -> runtime -> backend contract, never the reverse.
#
# DEPENDENCIES:
#   clash_backend   the published MihomoBackend contract (header-only). Both
#                   services are CONSUMERS of it; neither touches the engine.
#   clash_platform  PUBLIC, because routing_controller.h names
#                   platform::ProxyConfig and platform::SystemProxyService.
#                   Deliberately the real service rather than a port: its
#                   Operation is already a constructor injection point, so a
#                   test drives the genuine isChanging()/isBusy()/state()
#                   semantics that "a pending change is not a success" depends
#                   on, with no OS call.
#   clash_profiles  PRIVATE, and reached by exactly one translation unit.
#                   runtime_coordinator.h depends on the ConfigSource port, not
#                   on core::ProfileStore; profile_config_source.cpp is the
#                   only adapter and forwards three calls verbatim.
#   Qt6::Core       QObject, QTimer, QThreadPool, QFile.
# It links no Qt6::Widgets: nothing here owns a widget or opens a dialog.

add_library(clash_app_runtime STATIC
    runtime_coordinator.cpp
    routing_controller.cpp
    profile_config_source.cpp)

target_sources(clash_app_runtime PUBLIC FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        config_source.h
        runtime_coordinator.h
        routing_controller.h
        profile_config_source.h)

target_include_directories(clash_app_runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clash_app_runtime
    PUBLIC  clash_backend clash_platform Qt6::Core
    PRIVATE clash_profiles)
