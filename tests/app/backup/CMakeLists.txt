# Acceptance suite for app/backup (MOD-LIFECYCLE).
#
# Registration: tests/CMakeLists.txt adds
#   add_subdirectory(app/backup)
# after clash_backend_fake and clash_test_support are defined.
#
# clash_test_support is for ScopedEnvironment: one case builds the real
# core::BackupStore, and every suite that can reach core::preferences isolates
# it first. QTEST_GUILESS_MAIN, so no offscreen platform is needed.

qt_add_executable(backup-coordinator-tests backup_coordinator_test.cpp)
target_link_libraries(backup-coordinator-tests PRIVATE
    Qt6::Test clash_app_backup clash_app_lifecycle clash_backend_fake
    clash_backups clash_test_support)
add_test(NAME backup-coordinator COMMAND backup-coordinator-tests)
set_tests_properties(backup-coordinator PROPERTIES LABELS "unit;app;backup")

foreach(name backup-coordinator)
    clash_qt_isolate_test(${name})
endforeach()
clash_qt_label(backup-coordinator unit app backup)

