# Test registration. CTest names are preserved exactly from the pre-refactor
# build; renames are a separate, recorded change.
find_package(Qt6 6.9 REQUIRED COMPONENTS Test)

# Helpers shared with the test subdirectories. Defined before any
# add_subdirectory(), because a child scope only sees functions already defined.
function(clash_qt_label test_name)
    if(TEST ${test_name})
        set_tests_properties(${test_name} PROPERTIES LABELS "${ARGN}")
    endif()
endfunction()

function(clash_qt_isolate_test test_name)
    get_test_property(${test_name} ENVIRONMENT existing_env)
    if(existing_env STREQUAL "NOTFOUND")
        set(existing_env "")
    endif()
    list(APPEND existing_env
        "CLASH_QT_DATA_DIR=${CMAKE_BINARY_DIR}/test-data/${test_name}")
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${existing_env}")
endfunction()


# Backend suites now link the production libraries instead of recompiling their
# sources, so a test exercises the same objects the application ships.

qt_add_executable(controller-tests core/controller_test.cpp)
target_link_libraries(controller-tests PRIVATE Qt6::Test Qt6::Network clash_mihomo_impl)
add_test(NAME controller COMMAND controller-tests)

qt_add_executable(provider-tests core/provider_test.cpp)
target_link_libraries(provider-tests PRIVATE Qt6::Test Qt6::Network clash_mihomo_impl)
add_test(NAME provider COMMAND provider-tests)

qt_add_executable(backup-tests core/backup_test.cpp)
target_link_libraries(backup-tests PRIVATE Qt6::Test Qt6::Network clash_backups
    clash_preferences clash_test_support)
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

    qt_add_executable(tray-tests ui/tray_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/shell/proxy_environment.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/shell/tray_proxy_menu.cpp")
    target_link_libraries(tray-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
    clash_backend_bridge clash_backend_fake)
    add_test(NAME tray COMMAND tray-tests)

    qt_add_executable(routing-controls-tests ui/routing_controls_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/shell/routing_controls.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/toggle_switch.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(routing-controls-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_app_runtime clash_backend_bridge clash_backend_fake)
    add_test(NAME routing-controls COMMAND routing-controls-tests)

    # The privileged uninstall guard. It was inert for an entire wave -- read in
    # three places, written nowhere -- after D3 removed its only producer, which
    # meant the helper could be uninstalled while another session's core ran
    # under it. These cases fail if it goes inert again.
    qt_add_executable(service-settings-tests ui/service_settings_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/settings/service_settings.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/settings_section.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(service-settings-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_backend_bridge clash_backend_fake clash_test_support
        clash_profiles clash_platform clash_preferences)
    add_test(NAME service-settings COMMAND service-settings-tests)

    # Injects BrowserOperations through DashboardButton's constructor, so it links
    # the real platform library. clash_platform is named explicitly rather than
    # relied on transitively. It used to arrive through clash_mihomo_impl's
    # PUBLIC D2 edge, which P3 discharged.
    qt_add_executable(dashboard-async-tests ui/dashboard_async_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/shell/dashboard_button.cpp")
    target_link_libraries(dashboard-async-tests PRIVATE Qt6::Test Qt6::Widgets
        Qt6::Concurrent clash_backend_bridge clash_backend_fake clash_platform clash_preferences)
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


# The settings-isolation regression guard: fails if production preference access
# escapes the isolated location. Without it the defect returns silently.
qt_add_executable(preferences-tests core/preferences_test.cpp)
target_link_libraries(preferences-tests PRIVATE Qt6::Test clash_preferences clash_test_support)
target_compile_definitions(preferences-tests PRIVATE CLASH_QT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME preferences COMMAND preferences-tests)

# Shared test infrastructure (BASE-FIXTURE). The fake core is a compiled helper,
# not a shell script or an interpreter invocation, so the process tests are not
# POSIX-only and cannot silently skip on Windows.
add_executable(clash-qt-fake-core fixtures/fake_core_main.cpp)
set_target_properties(clash-qt-fake-core PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

qt_add_library(clash_test_support STATIC
    support/scoped_environment.cpp support/loopback_server.cpp support/fake_core.cpp)
target_include_directories(clash_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# PUBLIC clash_preferences: support/preference_isolation.h is header-only and
# calls core::preferences, so every consumer of the support headers needs it.
target_link_libraries(clash_test_support PUBLIC Qt6::Core Qt6::Network clash_preferences)
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

# Deterministic in-process backend plus its contract suite. The same contract
# tests will run against the real backend; a passing fake proves the contract is
# implementable and the consumers correct, not that the engine works.
qt_add_library(clash_backend_fake STATIC
    support/backend/fake_backend.cpp support/backend/recording_observer.cpp)
set_target_properties(clash_backend_fake PROPERTIES AUTOMOC OFF)
target_include_directories(clash_backend_fake PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(clash_backend_fake PUBLIC clash_backend Qt6::Core)

qt_add_executable(backend-contract-tests contracts/backend/backend_contract_test.cpp)
target_link_libraries(backend-contract-tests PRIVATE Qt6::Test clash_backend_fake)
add_test(NAME backend-contract COMMAND backend-contract-tests)

qt_add_executable(backend-bridge-tests contracts/backend/bridge/backend_bridge_test.cpp)
target_link_libraries(backend-bridge-tests PRIVATE Qt6::Test clash_backend_bridge clash_backend_fake)
add_test(NAME backend-bridge COMMAND backend-bridge-tests)

# Portable component object model contract (component-r1).
qt_add_executable(component-contract-tests contracts/component/component_contract_test.cpp)
target_link_libraries(component-contract-tests PRIVATE Qt6::Test clashqt_com)
add_test(NAME component-contract COMMAND component-contract-tests)


# ---------------------------------------------------------------------------
# Suites partitioned from the two former monoliths. The per-case preservation
# map is tests/README.md; every original assertion has a recorded home there.
# ---------------------------------------------------------------------------

qt_add_executable(config-generation-tests core/config_generation_test.cpp)
target_link_libraries(config-generation-tests PRIVATE Qt6::Test
    clash_profiles clash_config clash_yaml clash_test_support)
add_test(NAME config-generation COMMAND config-generation-tests)

qt_add_executable(profile-store-tests core/profile_store_test.cpp)
target_link_libraries(profile-store-tests PRIVATE Qt6::Test Qt6::Network
    clash_profiles clash_config clash_test_support)
add_test(NAME profile-store COMMAND profile-store-tests)

qt_add_executable(core-process-tests core/core_process_test.cpp)
target_link_libraries(core-process-tests PRIVATE Qt6::Test Qt6::Network
    clash_mihomo_impl clash_platform clash_test_support)
add_test(NAME core-process COMMAND core-process-tests)

qt_add_executable(runtime-maintenance-tests core/maintenance_test.cpp)
target_link_libraries(runtime-maintenance-tests PRIVATE Qt6::Test
    clash_profiles clash_config clash_test_support)
add_test(NAME runtime-maintenance COMMAND runtime-maintenance-tests)

if(CLASH_QT_BUILD_APP)

    qt_add_executable(home-page-tests ui/home_page_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/home_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/traffic_graph.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/formatting.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/settings_section.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(home-page-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_backend_bridge clash_backend_fake clash_test_support clash_telemetry)
    add_traffic_graph_module(home-page-tests)
    add_test(NAME home-page COMMAND home-page-tests)

    qt_add_executable(traffic-graph-tests ui/traffic_graph_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/traffic_graph.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(traffic-graph-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_test_support clash_telemetry)
    add_traffic_graph_module(traffic-graph-tests)
    add_test(NAME traffic-graph COMMAND traffic-graph-tests)

    qt_add_executable(proxies-page-tests ui/proxies_page_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/proxies/proxies_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/formatting.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(proxies-page-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_backend_bridge clash_backend_fake clash_test_support)
    add_test(NAME proxies-page COMMAND proxies-page-tests)

    qt_add_executable(connections-page-tests ui/connections_page_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/connections/connections_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/formatting.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(connections-page-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_backend_bridge clash_backend_fake clash_test_support)
    add_test(NAME connections-page COMMAND connections-page-tests)

    qt_add_executable(providers-page-tests ui/providers_page_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/providers/providers_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/formatting.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(providers-page-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_backend_bridge clash_backend_fake clash_test_support)
    add_test(NAME providers-page COMMAND providers-page-tests)

    qt_add_executable(rules-page-tests ui/rules_page_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/rules/rules_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(rules-page-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
        clash_backend_bridge clash_backend_fake clash_test_support)
    add_test(NAME rules-page COMMAND rules-page-tests)

    qt_add_executable(logs-page-tests ui/logs_page_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/logs/logs_page.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(logs-page-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network Qt6::Concurrent
        clash_backend_bridge clash_backend_fake clash_test_support)
    add_test(NAME logs-page COMMAND logs-page-tests)

    # Benchmark lane. These two measure native frame pacing and are gated on
    # CLASH_QT_VERIFY_GRAPH_FRAMES / CLASH_QT_MEASURE_FRAMES. They reported as
    # "Passed" inside data-pages while silently skipping; here they carry the
    # benchmark label that `make test` excludes, so they are honestly not run.
    # They must NOT get QT_QPA_PLATFORM=offscreen: with their flag set, offscreen
    # turns a skip into a failure.
    qt_add_executable(traffic-graph-frames-tests benchmarks/traffic_graph_frames_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/traffic_graph.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(traffic-graph-frames-tests PRIVATE Qt6::Test Qt6::Widgets
        clash_telemetry clash_test_support)
    add_traffic_graph_module(traffic-graph-frames-tests)
    add_test(NAME traffic-graph-frames COMMAND traffic-graph-frames-tests)

    qt_add_executable(traffic-frame-pacing-tests benchmarks/traffic_frame_pacing_test.cpp
        "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/traffic_graph.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/theme/theme.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/widgets/combo_box.cpp"
        "${PROJECT_SOURCE_DIR}/src/ui/resources/resources.qrc")
    target_link_libraries(traffic-frame-pacing-tests PRIVATE Qt6::Test Qt6::Widgets
        clash_telemetry clash_test_support)
    add_traffic_graph_module(traffic-frame-pacing-tests)
    add_test(NAME traffic-frame-pacing COMMAND traffic-frame-pacing-tests)
endif()

add_subdirectory(app/lifecycle)
add_subdirectory(app/backup)
add_subdirectory(app/runtime)
add_subdirectory(core/mihomo)
add_subdirectory(workflows)

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

clash_qt_label(controller            integration core mihomo)
clash_qt_label(provider              integration core mihomo)
clash_qt_label(backup                integration core backups)
clash_qt_label(traffic-history       unit core telemetry)
clash_qt_label(proxy                 unit platform proxy)
clash_qt_label(system-proxy-async    unit platform proxy)
clash_qt_label(privileged-service    integration platform service)
clash_qt_label(privileged-helper     unit platform service)
clash_qt_label(privileged-helper-ipc unit platform service)
clash_qt_label(tray                  ui shell)
clash_qt_label(routing-controls      ui shell)
clash_qt_label(dashboard-async       ui shell)
clash_qt_label(component-contract  unit contract component)
clash_qt_label(scoped-environment  unit support)
clash_qt_label(loopback-server     unit support)
clash_qt_label(fake-core           unit support)
clash_qt_label(preferences         unit core preferences)
clash_qt_label(config-generation   integration core config)
clash_qt_label(profile-store       integration core profiles)
clash_qt_label(core-process        integration core mihomo)
clash_qt_label(runtime-maintenance integration core profiles)
clash_qt_label(home-page           ui pages)
clash_qt_label(traffic-graph       ui pages)
clash_qt_label(proxies-page        ui pages)
clash_qt_label(connections-page    ui pages)
clash_qt_label(providers-page      ui pages)
clash_qt_label(rules-page          ui pages)
clash_qt_label(logs-page           ui pages)
clash_qt_label(traffic-graph-frames  benchmark ui)
clash_qt_label(traffic-frame-pacing  benchmark ui)
clash_qt_label(backend-contract    unit contract backend)

# ---------------------------------------------------------------------------
# Harness-level preference isolation.
#
# core::preferences falls back to the developer's real native store when
# CLASH_QT_DATA_DIR is unset -- correct for a real user, dangerous for a test.
# Isolation was previously opt-in per test via ScopedEnvironment, so any new
# suite that forgot it wrote into ~/Library/Preferences. That happened: a new
# app-layer suite modified the real store during P3.
#
# Every registered test now gets its own data directory by default. A suite that
# also uses ScopedEnvironment simply overrides this with something narrower. The
# point is that forgetting is no longer sufficient to reach the real store.
# ---------------------------------------------------------------------------
# Subdirectory tests apply this themselves via clash_qt_isolate_test(); CMake
# does not allow a parent scope to set properties on a child's tests.
get_property(clash_qt_all_tests DIRECTORY PROPERTY TESTS)
foreach(test_name IN LISTS clash_qt_all_tests)
    get_test_property(${test_name} ENVIRONMENT existing_env)
    if(existing_env STREQUAL "NOTFOUND")
        set(existing_env "")
    endif()
    list(APPEND existing_env
        "CLASH_QT_DATA_DIR=${CMAKE_BINARY_DIR}/test-data/${test_name}")
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${existing_env}")
endforeach()
clash_qt_label(backend-bridge     unit contract backend)

# ---------------------------------------------------------------------------
# Offscreen platform for every widget-constructing suite.
#
# Applied HERE, at the end, on purpose. This block used to sit above the suite
# registrations, where `if(TEST ...)` was simply false for anything not yet
# defined -- so seven widget suites silently ran against the real display and
# would have failed on a headless runner. A guard that quietly does nothing is
# worse than no guard.
#
# The benchmark entries are deliberately excluded: with their measurement flag
# set, offscreen turns their skip into a failure.
#
# Only tests defined in THIS directory can be listed: if(TEST ...) is false here
# for a test registered in a subdirectory, so naming one would set nothing and
# say nothing. Subdirectories set their own (see tests/workflows/).
# ---------------------------------------------------------------------------
set(clash_qt_widget_tests
    controller provider backup proxy tray routing-controls traffic-history
    dashboard-async system-proxy-async privileged-service preferences
    config-generation profile-store core-process runtime-maintenance
    home-page traffic-graph proxies-page connections-page providers-page
    rules-page logs-page service-settings)
foreach(name IN LISTS clash_qt_widget_tests)
    if(TEST ${name})
        get_test_property(${name} ENVIRONMENT existing_env)
        if(existing_env STREQUAL "NOTFOUND")
            set(existing_env "")
        endif()
        list(APPEND existing_env "QT_QPA_PLATFORM=offscreen")
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${existing_env}")
    endif()
endforeach()

# The QQuickView-embedding suites additionally need the software backend.
foreach(name home-page traffic-graph)
    if(TEST ${name})
        get_test_property(${name} ENVIRONMENT existing_env)
        list(APPEND existing_env "QT_QUICK_BACKEND=software")
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${existing_env}")
    endif()
endforeach()
clash_qt_label(service-settings    ui pages service)
