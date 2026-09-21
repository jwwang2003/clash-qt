# Test registration. CTest names are preserved exactly from the pre-refactor
# build; renames are a separate, recorded change.
find_package(Qt6 6.9 REQUIRED COMPONENTS Test)

# Backend suites now link the production libraries instead of recompiling their
# sources, so a test exercises the same objects the application ships.
qt_add_executable(runtime-tests core/runtime_test.cpp)
target_link_libraries(runtime-tests PRIVATE Qt6::Test Qt6::Network
    clash_profiles clash_config clash_yaml clash_mihomo_impl clash_platform)
add_test(NAME runtime COMMAND runtime-tests)

qt_add_executable(controller-tests core/controller_test.cpp)
target_link_libraries(controller-tests PRIVATE Qt6::Test Qt6::Network clash_mihomo_impl)
add_test(NAME controller COMMAND controller-tests)

qt_add_executable(provider-tests core/provider_test.cpp)
target_link_libraries(provider-tests PRIVATE Qt6::Test Qt6::Network clash_mihomo_impl)
add_test(NAME provider COMMAND provider-tests)

qt_add_executable(backup-tests core/backup_test.cpp)
target_link_libraries(backup-tests PRIVATE Qt6::Test Qt6::Network clash_backups)
add_test(NAME backup COMMAND backup-tests)

qt_add_executable(traffic-history-tests core/traffic_history_test.cpp)
target_link_libraries(traffic-history-tests PRIVATE Qt6::Test clash_telemetry)
add_test(NAME traffic-history COMMAND traffic-history-tests)

qt_add_executable(privileged-service-tests platform/privileged_service_client_test.cpp)
target_link_libraries(privileged-service-tests PRIVATE Qt6::Test Qt6::Network clash_platform)
add_test(NAME privileged-service COMMAND privileged-service-tests)

qt_add_executable(system-proxy-async-tests platform/system_proxy_async_test.cpp)
target_link_libraries(system-proxy-async-tests PRIVATE Qt6::Test clash_platform)
add_test(NAME system-proxy-async COMMAND system-proxy-async-tests)

# Substitutes SystemProxyBackend's command runner through its constructor, so it
# links the production library like every other suite. It must not additionally
# compile system_proxy.cpp: that would define platform::SystemProxy twice.
qt_add_executable(proxy-tests platform/system_proxy_test.cpp)
target_link_libraries(proxy-tests PRIVATE Qt6::Test clash_platform)
add_test(NAME proxy COMMAND proxy-tests)

if(CLASH_QT_BUILD_APP)
    qt_add_executable(data-pages-tests ui/data_pages_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/proxies/proxies_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/connections/connections_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/rules/rules_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/logs/logs_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/home_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/traffic_graph.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/providers/providers_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/formatting.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/settings_section.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(data-pages-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_mihomo_impl clash_telemetry)
    add_traffic_graph_module(data-pages-tests)
    add_test(NAME data-pages COMMAND data-pages-tests)

    qt_add_executable(tray-tests ui/tray_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/shell/proxy_environment.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/shell/tray_proxy_menu.cpp")
    target_link_libraries(tray-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network clash_mihomo_impl)
    add_test(NAME tray COMMAND tray-tests)

    qt_add_executable(routing-controls-tests ui/routing_controls_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/shell/routing_controls.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/toggle_switch.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(routing-controls-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_mihomo_impl)
    add_test(NAME routing-controls COMMAND routing-controls-tests)

    # Injects BrowserOperations through DashboardButton's constructor, so it links
    # the real platform library. clash_platform is named explicitly rather than
    # relied on transitively: today it arrives via clash_mihomo_impl's PUBLIC D2
    # edge, and that edge is removed in P3.
    qt_add_executable(dashboard-async-tests ui/dashboard_async_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/shell/dashboard_button.cpp")
    target_link_libraries(dashboard-async-tests PRIVATE Qt6::Test Qt6::Widgets
        Qt6::Concurrent clash_mihomo_impl clash_platform)
    add_test(NAME dashboard-async COMMAND dashboard-async-tests)
endif()

if(APPLE)
    add_test(NAME privileged-helper COMMAND clash-qt-service-helper --self-test)
    add_executable(privileged-helper-ipc-tests
        "${PROJECT_SOURCE_DIR}/src/services/macos/macos_helper.mm")
    set_target_properties(privileged-helper-ipc-tests PROPERTIES AUTOMOC OFF AUTORCC OFF)
    target_compile_definitions(privileged-helper-ipc-tests PRIVATE CLASH_QT_HELPER_TESTING=1)
    target_compile_options(privileged-helper-ipc-tests PRIVATE -fobjc-arc)
    target_link_libraries(privileged-helper-ipc-tests PRIVATE
        "-framework Foundation" "-framework Security")
    add_test(NAME privileged-helper-ipc COMMAND privileged-helper-ipc-tests --ipc-self-test)
endif()

set(offscreen_tests runtime controller provider backup proxy tray routing-controls
    traffic-history dashboard-async system-proxy-async privileged-service)
if(CLASH_QT_BUILD_APP)
    list(APPEND offscreen_tests data-pages)
endif()
foreach(name IN LISTS offscreen_tests)
    if(TEST ${name})
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endforeach()
if(TEST data-pages)
    set_tests_properties(data-pages PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()

# Shared test infrastructure (BASE-FIXTURE). The fake core is a compiled helper,
# not a shell script or an interpreter invocation, so the process tests are not
# POSIX-only and cannot silently skip on Windows.
add_executable(clash-qt-fake-core fixtures/fake_core_main.cpp)
set_target_properties(clash-qt-fake-core PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

qt_add_library(clash_test_support STATIC
    support/scoped_environment.cpp support/loopback_server.cpp support/fake_core.cpp)
target_include_directories(clash_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(clash_test_support PUBLIC Qt6::Core Qt6::Network)
add_dependencies(clash_test_support clash-qt-fake-core)

qt_add_executable(scoped-environment-tests support/scoped_environment_test.cpp)
target_link_libraries(scoped-environment-tests PRIVATE clash_test_support Qt6::Test)
add_test(NAME scoped-environment COMMAND scoped-environment-tests)

qt_add_executable(loopback-server-tests support/loopback_server_test.cpp)
target_link_libraries(loopback-server-tests PRIVATE clash_test_support Qt6::Test Qt6::WebSockets)
add_test(NAME loopback-server COMMAND loopback-server-tests)

qt_add_executable(fake-core-tests support/fake_core_test.cpp)
target_link_libraries(fake-core-tests PRIVATE clash_test_support Qt6::Test)
add_test(NAME fake-core COMMAND fake-core-tests)
set_tests_properties(fake-core PROPERTIES
    ENVIRONMENT "CLASH_QT_FAKE_CORE=$<TARGET_FILE:clash-qt-fake-core>")

# Portable component object model contract (component-r1).
qt_add_executable(component-contract-tests contracts/component/component_contract_test.cpp)
target_link_libraries(component-contract-tests PRIVATE Qt6::Test clashqt_com)
add_test(NAME component-contract COMMAND component-contract-tests)

# Architecture checks. Reads evaluated CMake target dependencies through the File
# API, so transitive and generated edges are included rather than guessed at from
# the text of these files.
add_subdirectory(architecture)

# CTest labels select the lanes the Make facade exposes.
#   unit        deterministic, no real I/O beyond a temp directory
#   integration real Qt networking, filesystem or child processes against fixtures
#   ui          widget suites; run offscreen in the routine lane
# Reserved for packages that do not exist yet, so a lane cannot pass vacuously:
#   real-core   drives the locally built mihomo
#   native      needs a real display or real networking
#   privileged  needs an exclusive disposable host
#   benchmark   measured, machine-specific budgets
#
# Written out one call per suite rather than as a data list: CMake flattens
# semicolons inside a quoted string into separate list elements, which silently
# desynchronises a paired name/labels list.
function(clash_qt_label test_name)
    if(TEST ${test_name})
        set_tests_properties(${test_name} PROPERTIES LABELS "${ARGN}")
    endif()
endfunction()

clash_qt_label(runtime               integration core config profiles)
clash_qt_label(controller            integration core mihomo)
clash_qt_label(provider              integration core mihomo)
clash_qt_label(backup                integration core backups)
clash_qt_label(traffic-history       unit core telemetry)
clash_qt_label(proxy                 unit platform proxy)
clash_qt_label(system-proxy-async    unit platform proxy)
clash_qt_label(privileged-service    integration platform service)
clash_qt_label(privileged-helper     unit platform service)
clash_qt_label(privileged-helper-ipc unit platform service)
clash_qt_label(data-pages            ui pages)
clash_qt_label(tray                  ui shell)
clash_qt_label(routing-controls      ui shell)
clash_qt_label(dashboard-async       ui shell)
clash_qt_label(component-contract  unit contract component)
clash_qt_label(scoped-environment  unit support)
clash_qt_label(loopback-server     unit support)
clash_qt_label(fake-core           unit support)
