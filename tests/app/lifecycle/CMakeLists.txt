# Acceptance suite for app/lifecycle.
#
# Registration: tests/CMakeLists.txt adds
#   add_subdirectory(app/lifecycle)
# after clash_backend_fake is defined. Each package under tests/app registers
# itself; there is deliberately no shared parent for them, because the runtime
# package belongs to another owner and one parent file would be a standing
# collision between the two.
#
# QTEST_GUILESS_MAIN, so it needs neither QT_QPA_PLATFORM=offscreen nor a place
# in the `offscreen_tests` list. The label is set here because clash_qt_label()
# is defined further down tests/CMakeLists.txt than this can be added.

qt_add_executable(shutdown-coordinator-tests shutdown_coordinator_test.cpp)
target_link_libraries(shutdown-coordinator-tests PRIVATE
    Qt6::Test clash_app_lifecycle clash_backend_fake)
add_test(NAME shutdown-coordinator COMMAND shutdown-coordinator-tests)
set_tests_properties(shutdown-coordinator PROPERTIES LABELS "unit;app;lifecycle")

foreach(name shutdown-coordinator)
    clash_qt_isolate_test(${name})
endforeach()
clash_qt_label(shutdown-coordinator unit app lifecycle)

