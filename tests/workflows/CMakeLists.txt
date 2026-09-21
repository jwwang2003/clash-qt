# Complete workflows: the stage 3 acceptance clause. These drive the assembled
# coordinators over the real ProfileStore, the real filesystem, real loopback
# networking and the compiled fake core -- they are journeys, not unit tests.
#
# RUN_SERIAL on every suite: they share the fixed controller port 29097, which
# ProfileStore forces because it rewrites external-controller in every generated
# config, so a journey cannot choose its own port.
set(clash_qt_workflow_libs
    clash_test_support clash_backend_fake clash_app_runtime clash_app_lifecycle
    clash_app_backup clash_app_composition clash_mihomo_impl clash_profiles
    clash_config clash_backups clash_platform clash_preferences clash_telemetry)

foreach(wf w01_first_launch w04_restore w05_recovery)
    string(REPLACE "_" "-" wf_name "${wf}")
    string(REGEX REPLACE "^w0([0-9])-" "w0\\1-" wf_test "${wf_name}")
    qt_add_executable(${wf_name}-tests ${wf}_test.cpp)
    target_link_libraries(${wf_name}-tests PRIVATE
        Qt6::Test Qt6::Network Qt6::Concurrent ${clash_qt_workflow_libs})
    add_dependencies(${wf_name}-tests clash-qt-fake-core)
    add_test(NAME ${wf_name} COMMAND ${wf_name}-tests)
    set_tests_properties(${wf_name} PROPERTIES
        RUN_SERIAL TRUE TIMEOUT 300
        ENVIRONMENT "CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>")
endforeach()

# W03 is the only journey that builds the real shell, so it needs the UI sources
# and the QML module. src/ui/sources.cmake cannot be include()d from here: every
# path in it is relative to its own directory.
qt_add_executable(w03-routing-controls-tests w03_routing_controls_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/backups/backup_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/connections/connections_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/logs/logs_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/home_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/traffic_graph.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/profiles/profiles_page.cpp"
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
target_link_libraries(w03-routing-controls-tests PRIVATE
    Qt6::Test Qt6::Network Qt6::Concurrent Qt6::Widgets Qt6::Qml
    clash_backend_bridge ${clash_qt_workflow_libs})
add_traffic_graph_module(w03-routing-controls-tests)
add_dependencies(w03-routing-controls-tests clash-qt-fake-core)
add_test(NAME w03-routing-controls COMMAND w03-routing-controls-tests)
# The offscreen platform and the software Quick backend are set HERE, not in the
# parent. W03 builds the real shell, and the parent's env loops use if(TEST ...),
# which is false for a subdirectory's test -- so naming it there set nothing at
# all, silently. Same failure as the ordering bug fixed in 447c381, recurring by
# scope instead of order.
set_tests_properties(w03-routing-controls PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 300
    ENVIRONMENT "CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

# Launches the real shipping binary. This is the lane that can see a component
# that builds and links but that nothing wires up -- the shape of the
# privileged-service regression P3 shipped.
qt_add_executable(app-smoke-tests app_smoke_test.cpp)
target_link_libraries(app-smoke-tests PRIVATE
    Qt6::Test Qt6::Network Qt6::Concurrent ${clash_qt_workflow_libs})
add_dependencies(app-smoke-tests clash-qt-fake-core clash-qt)
add_test(NAME app-smoke COMMAND app-smoke-tests)
set_tests_properties(app-smoke PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 600
    ENVIRONMENT "CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>;CLASH_QT_APP_BINARY=$<TARGET_FILE:clash-qt>")

clash_qt_label(w01-first-launch      workflow integration app)
clash_qt_label(w03-routing-controls  workflow integration app ui)
clash_qt_label(w04-restore           workflow integration app)
clash_qt_label(w05-recovery          workflow integration app)
clash_qt_label(app-smoke             workflow integration app packaging)
foreach(name w01-first-launch w03-routing-controls w04-restore w05-recovery app-smoke)
    clash_qt_isolate_test(${name})
endforeach()
