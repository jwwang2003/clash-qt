foreach(suite preset_store effective_config_preview)
    string(REPLACE "_" "-" test_name "${suite}")
    qt_add_executable(${test_name}-tests ${suite}_test.cpp)
    target_link_libraries(${test_name}-tests PRIVATE
        Qt6::Test Qt6::Network clash_profiles clash_test_support)
    add_test(NAME ${test_name} COMMAND ${test_name}-tests)
    clash_qt_label(${test_name} core config integration)
    clash_qt_isolate_test(${test_name})
endforeach()
foreach(name preset-store effective-config-preview)
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen" "QT_QUICK_BACKEND=software")
endforeach()
