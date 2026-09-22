# Test-only module. The shipping entry installs no test command extension.
add_library(clash_qt_fake_backend_module SHARED
    "${PROJECT_SOURCE_DIR}/tests/support/component/fake_module_entry.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/mihomo/module/backend_session.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/mihomo/module/backend_module.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/mihomo/module/host_privileged_service.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/mihomo/module/module_export.cpp")
target_link_libraries(clash_qt_fake_backend_module PRIVATE
    clash_component_marshal clash_backend_fake ${CMAKE_DL_LIBS})
set_target_properties(clash_qt_fake_backend_module PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules")

qt_add_executable(abi-contract-tests abi_contract_test.cpp)
target_link_libraries(abi-contract-tests PRIVATE Qt6::Test clash_module_host
    clash_component_marshal clash_backend_fake)
add_dependencies(abi-contract-tests clash_qt_backend_module clash_qt_fake_backend_module)
add_test(NAME abi-contract COMMAND abi-contract-tests)
set_tests_properties(abi-contract PROPERTIES TIMEOUT 180
    ENVIRONMENT "CLASH_QT_BACKEND_MODULE=$<TARGET_FILE:clash_qt_backend_module>;CLASH_QT_FAKE_MODULE=$<TARGET_FILE:clash_qt_fake_backend_module>")
clash_qt_label(abi-contract contract component integration)
clash_qt_isolate_test(abi-contract)

qt_add_executable(connection-snapshot-marshal-tests
    "${PROJECT_SOURCE_DIR}/tests/benchmarks/connection_snapshot_marshal_test.cpp")
target_link_libraries(connection-snapshot-marshal-tests PRIVATE Qt6::Test clash_component_marshal)
add_test(NAME connection-snapshot-codec COMMAND connection-snapshot-marshal-tests
    thePackedSnapshotSurvivesARoundTripAtEverySize
    repeatedStringsAreStoredOnceSoTheBufferStaysSmall)
clash_qt_label(connection-snapshot-codec unit component)
clash_qt_isolate_test(connection-snapshot-codec)
add_test(NAME connection-snapshot-marshal COMMAND connection-snapshot-marshal-tests
    comparePackedAndNaiveAtSnapshotScale
    benchmarkPackingAPacked benchmarkUnpackingAPacked)
clash_qt_label(connection-snapshot-marshal benchmark component)
clash_qt_isolate_test(connection-snapshot-marshal)
