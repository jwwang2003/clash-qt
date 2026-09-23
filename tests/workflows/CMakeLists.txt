# Complete journeys. These drive the assembled
# coordinators over the real ProfileStore, the real filesystem, real loopback
# networking and the compiled fake core -- they are journeys, not unit tests.
#
# RUN_SERIAL on every suite. The journeys no longer share one controller port --
# each claims a free one and hands it to the profile store through
# CLASH_QT_CONTROLLER_PORT -- but they still start real child processes, bind
# real loopback sockets and count the engines running out of their own
# directories, so two of them at once would read each other's machine.
set(clash_qt_workflow_libs
    clash_test_support clash_backend_fake clash_app_runtime clash_app_lifecycle
    clash_app_backup clash_app_composition clash_module_host clash_profiles
    clash_config clash_backups clash_platform clash_preferences clash_telemetry)

# Target, source stem and registered name all agree. They briefly did not: the
# registered names dropped their ordinal prefix while the targets kept theirs,
# because the file declaring targets was held by another package at the time. A
# journey's subject names it better than its position in a list, and one name is
# easier to follow than two.
foreach(wf first_launch subscription_update restore recovery)
    string(REPLACE "_" "-" wf_name "${wf}")
    set(wf_test "${wf_name}")
    qt_add_executable(${wf_name}-tests ${wf}_test.cpp)
    target_link_libraries(${wf_name}-tests PRIVATE
        Qt6::Test Qt6::Network Qt6::Concurrent ${clash_qt_workflow_libs})
    add_dependencies(${wf_name}-tests clash-qt-fake-core clash_qt_backend_module)
    if(wf STREQUAL "subscription_update")
        add_test(NAME ${wf_test} COMMAND ${wf_name}-tests
            theHarnessStillMirrorsTheCompositionRoot
            aSubscriptionIsComposedThroughPresetsStartedRefreshedAndReopened
            aRejectedRefreshPreservesTheCachedSubscriptionAndTheLiveCore
            aHeldRefreshLosesToAUrlEditAndTheLatestUrlWins)
    else()
        add_test(NAME ${wf_test} COMMAND ${wf_name}-tests)
    endif()
    set_tests_properties(${wf_test} PROPERTIES
        RUN_SERIAL TRUE TIMEOUT 300
        ENVIRONMENT "CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>;CLASH_QT_MODULE_PATH=$<TARGET_FILE:clash_qt_backend_module>")
endforeach()

# Routing controls is the only journey that builds the real shell, so it needs
# the UI sources and the QML module. src/ui/sources.cmake cannot be include()d
# from here: every path in it is relative to its own directory.
if(CLASH_QT_BUILD_APP)
qt_add_executable(routing-controls-journey-tests routing_controls_journey_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/backups/backup_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/connections/connections_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/logs/logs_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/home_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/traffic_graph.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/profiles/profiles_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/profiles/preset_editor.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/profiles/effective_config_view.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/providers/providers_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/proxies/proxies_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/rules/rules_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/settings/chain_editor.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/settings/hotkey_settings.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/settings/service_settings.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/settings/settings_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc"
        "${PROJECT_SOURCE_DIR}/src/ui/shell/dashboard_button.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/shell/main_window.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/shell/proxy_environment.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/shell/routing_controls.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/shell/tray_icon.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/shell/tray_proxy_menu.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/formatting.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/settings_section.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/toggle_switch.cpp"
)
target_link_libraries(routing-controls-journey-tests PRIVATE
    Qt6::Test Qt6::Network Qt6::Concurrent Qt6::Widgets Qt6::Qml
    clash_backend_bridge ${clash_qt_workflow_libs})
add_traffic_graph_module(routing-controls-journey-tests)
add_dependencies(routing-controls-journey-tests clash-qt-fake-core clash_qt_backend_module)
add_test(NAME routing-controls-journey COMMAND routing-controls-journey-tests)
# The offscreen platform and the software Quick backend are set HERE, not in the
# parent. This journey builds the real shell, and the parent's env loops use
# if(TEST ...), which is false for a subdirectory's test -- so naming it there
# set nothing at all, silently. The same block once sat ABOVE the add_test()
# calls it named: the tests did not exist yet, if(TEST ...) was false for every
# one of them, and it set nothing at all just as quietly. Scope instead of
# order, same silence.
set_tests_properties(routing-controls-journey PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 300
    ENVIRONMENT "CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>;CLASH_QT_MODULE_PATH=$<TARGET_FILE:clash_qt_backend_module>;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

# Launches the real shipping binary. This is the lane that can see a component
# that builds and links but that nothing wires up -- the shape of the
# privileged-service regression this project has already shipped once.
qt_add_executable(app-smoke-tests app_smoke_test.cpp)
target_link_libraries(app-smoke-tests PRIVATE
    Qt6::Test Qt6::Network Qt6::Concurrent ${clash_qt_workflow_libs})
add_dependencies(app-smoke-tests clash-qt-fake-core clash-qt clash_qt_backend_module)
add_test(NAME app-smoke COMMAND app-smoke-tests)
set_tests_properties(app-smoke PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 600
    ENVIRONMENT "CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>;CLASH_QT_APP_BINARY=$<TARGET_FILE:clash-qt>;CLASH_QT_MODULE_PATH=$<TARGET_FILE:clash_qt_backend_module>;CLASH_QT_SERVICE_SOCKET=${CMAKE_BINARY_DIR}/test-data/app-smoke/helper.socket")
clash_qt_label(routing-controls-journey workflow integration app ui)
clash_qt_label(app-smoke             workflow integration app packaging)
clash_qt_isolate_test(routing-controls-journey)
clash_qt_isolate_test(app-smoke)
endif()

clash_qt_label(first-launch          workflow integration app)
clash_qt_label(subscription-update   workflow integration app config)
clash_qt_label(restore               workflow integration app)
clash_qt_label(recovery              workflow integration app)
foreach(name first-launch subscription-update restore recovery)
    clash_qt_isolate_test(${name})
endforeach()

add_test(NAME subscription-update-real-core COMMAND subscription-update-tests
    theRealSourceBuiltCoreRefreshesAndRejectsAnInvalidUpdate)
set_tests_properties(subscription-update-real-core PROPERTIES RUN_SERIAL TRUE TIMEOUT 300
    ENVIRONMENT "CLASH_QT_SUBSCRIPTION_REAL_CORE=1;CLASH_QT_CORE_BINARY=${CLASH_QT_CORE_BINARY};CLASH_QT_CORE_PROVENANCE=${CLASH_QT_CORE_DIR}/mihomo-provenance.json;CLASH_QT_MODULE_PATH=$<TARGET_FILE:clash_qt_backend_module>")
clash_qt_label(subscription-update-real-core workflow real-core integration app config)
clash_qt_isolate_test(subscription-update-real-core)
