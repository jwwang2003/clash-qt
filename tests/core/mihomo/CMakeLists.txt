# The real MihomoBackend (MOD-CORE).
#
# backend-real-contract runs the backend-r2 acceptance set against the real
# engine adapter with real child processes and a real loopback controller. It is
# the counterpart to backend-contract, which runs the same semantics against the
# fake: a passing fake proves the contract is implementable, this proves the
# engine honours it.
qt_add_executable(backend-real-contract-tests backend_real_contract_test.cpp)
target_link_libraries(backend-real-contract-tests PRIVATE
    Qt6::Test Qt6::Network clash_mihomo_impl clash_app_composition clash_test_support)
add_test(NAME backend-real-contract COMMAND backend-real-contract-tests)
# Every FakeCore consumer needs the fixture path, not just the fixture's own suite.
set_tests_properties(backend-real-contract PROPERTIES
    ENVIRONMENT "CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>")

qt_add_executable(engine-discovery-tests engine_discovery_test.cpp)
target_link_libraries(engine-discovery-tests PRIVATE
    Qt6::Test clash_mihomo_impl clash_test_support)
add_test(NAME engine-discovery COMMAND engine-discovery-tests)

# Drives the locally built engine. Labelled real-core, which `make test`
# excludes and `make test-integration` selects, so an absent engine cannot make
# the routine lane fail and cannot be mistaken for a pass either.
qt_add_executable(backend-real-core-tests backend_real_core_test.cpp)
target_link_libraries(backend-real-core-tests PRIVATE
    Qt6::Test Qt6::Network clash_mihomo_impl clash_app_composition clash_test_support)
add_test(NAME backend-real-core COMMAND backend-real-core-tests)
set_tests_properties(backend-real-core PROPERTIES
    ENVIRONMENT "CLASH_QT_CORE_BINARY=${CMAKE_BINARY_DIR}/core/mihomo;CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>")

foreach(name backend-real-contract engine-discovery backend-real-core)
    clash_qt_isolate_test(${name})
endforeach()
clash_qt_label(backend-real-contract integration core mihomo contract)
clash_qt_label(engine-discovery unit core mihomo)
clash_qt_label(backend-real-core real-core integration core mihomo)

