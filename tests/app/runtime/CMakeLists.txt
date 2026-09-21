# Runtime and routing coordinators (MOD-RUNTIME). Both are QTEST_GUILESS_MAIN,
# so neither needs an offscreen platform.
qt_add_executable(runtime-coordinator-tests runtime_coordinator_test.cpp)
target_link_libraries(runtime-coordinator-tests PRIVATE
    Qt6::Test clash_app_runtime clash_backend_fake clash_test_support)
add_test(NAME runtime-coordinator COMMAND runtime-coordinator-tests)

qt_add_executable(routing-controller-tests routing_controller_test.cpp)
target_link_libraries(routing-controller-tests PRIVATE
    Qt6::Test clash_app_runtime clash_backend_fake clash_test_support)
add_test(NAME routing-controller COMMAND routing-controller-tests)

foreach(name runtime-coordinator routing-controller)
    clash_qt_isolate_test(${name})
endforeach()
clash_qt_label(runtime-coordinator unit app runtime)
clash_qt_label(routing-controller unit app runtime)

