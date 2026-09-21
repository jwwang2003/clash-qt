# Backup ownership (MOD-LIFECYCLE). Constructs and owns the process's one
# core::BackupStore, so shutdown no longer discovers it by walking the widget
# tree (main.cpp:182).
#
# The composition root adds this with
#   add_subdirectory(src/app/backup)
# and links clash_app_backup into clash-qt.
#
# DEPENDENCIES:
#   clash_backups        the store itself - this target constructs it.
#   clash_app_lifecycle  the ShutdownState query that replaces the
#                        "shuttingDown" application property.
#   clash_backend        the published MihomoBackend contract (header-only).
# It does NOT link clash_profiles or clash_config: ProfileStore and
# ConfigEnhancer reach it through the StoreHooks the composition root binds.

add_library(clash_app_backup STATIC
    backup_store_session.cpp
    backup_coordinator.cpp)

target_sources(clash_app_backup PUBLIC FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        backup_session.h
        backup_store_session.h
        backup_coordinator.h)

target_include_directories(clash_app_backup PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clash_app_backup
    PUBLIC  clash_backend clash_app_lifecycle Qt6::Core
    PRIVATE clash_backups)
