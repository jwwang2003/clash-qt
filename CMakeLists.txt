cmake_minimum_required(VERSION 3.21)
project(clash-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.9 REQUIRED COMPONENTS Widgets Network WebSockets Qml Quick Graphs Concurrent)
find_package(yaml-cpp REQUIRED)


qt_standard_project_setup(REQUIRES 6.9)
set(CMAKE_AUTORCC ON)

set_source_files_properties(src/ui/pages/overview/qml/TrafficGraph.qml PROPERTIES QT_RESOURCE_ALIAS TrafficGraph.qml)
function(add_traffic_graph_module target)
    target_link_libraries(${target} PRIVATE Qt6::Quick Qt6::Graphs)
    qt_add_qml_module(${target}
        URI ClashQt
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml/${target}/ClashQt"
        QML_FILES src/ui/pages/overview/qml/TrafficGraph.qml
    )
endfunction()

qt_add_executable(clash-qt src/main.cpp)

target_include_directories(clash-qt PRIVATE src)
target_link_libraries(clash-qt PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    Qt6::Concurrent
    Qt6::Qml          # QJSEngine, for the profile enhancement scripts
    yaml-cpp::yaml-cpp
)

# Each module owns its own source list so modules can be worked on independently.
include(src/core/sources.cmake)
include(src/platform/sources.cmake)
include(src/ui/sources.cmake)
add_traffic_graph_module(clash-qt)

set_target_properties(clash-qt PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.clash-qt.desktop"
    MACOSX_BUNDLE_BUNDLE_NAME "clash-qt"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)
if(APPLE)
    # Keep the original development command pointing at the current bundle.
    add_custom_command(TARGET clash-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_CURRENT_BINARY_DIR}/clash-qt"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "clash-qt.app/Contents/MacOS/clash-qt" "${CMAKE_CURRENT_BINARY_DIR}/clash-qt"
        VERBATIM
    )
endif()
if(WIN32)
    set_target_properties(clash-qt PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(APPLE)
    enable_language(OBJCXX)
    add_executable(clash-qt-service-helper src/services/macos/macos_helper.mm)
    set_target_properties(clash-qt-service-helper PROPERTIES AUTOMOC OFF AUTORCC OFF)
    target_compile_options(clash-qt-service-helper PRIVATE -fobjc-arc)
    target_link_libraries(clash-qt-service-helper PRIVATE "-framework Foundation" "-framework Security")
    add_dependencies(clash-qt clash-qt-service-helper)
    add_custom_command(TARGET clash-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:clash-qt>/Contents/Helpers"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:clash-qt-service-helper>"
            "$<TARGET_BUNDLE_DIR:clash-qt>/Contents/Helpers/clash-qt-service-helper"
        VERBATIM)
endif()


include(GNUInstallDirs)
install(TARGETS clash-qt BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:clash-qt> DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(APPLE OR WIN32)
    set(deploy_options)
    if(APPLE AND Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
        # Modular Qt installations keep plugin frameworks outside QtCore's prefix.
        get_filename_component(qt_library_dir "${Qt6_DIR}/../.." ABSOLUTE)
        list(APPEND deploy_options DEPLOY_TOOL_OPTIONS "-libpath=${qt_library_dir}")
    endif()
    qt_generate_deploy_qml_app_script(TARGET clash-qt OUTPUT_SCRIPT deploy_script
        MACOS_BUNDLE_POST_BUILD ${deploy_options})
    if(APPLE)
        find_program(qtpaths_program NAMES qtpaths6 qtpaths HINTS "${qt_library_dir}/../bin" REQUIRED NO_DEFAULT_PATH)
        execute_process(COMMAND "${qtpaths_program}" --query QT_INSTALL_QML
            OUTPUT_VARIABLE qt_qml_dir OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        # The development bundle also receives QML imports at POST_BUILD.
        # Repair SDK-relative metadata links there, not just in installed apps.
        set(development_qml_script "${CMAKE_CURRENT_BINARY_DIR}/resolve_development_qml.cmake")
        file(GENERATE OUTPUT "${development_qml_script}" CONTENT "
include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/ResolveQmlLinks.cmake\")
resolve_qml_links(\"$<TARGET_BUNDLE_DIR:clash-qt>\" \"${qt_qml_dir}\")
")
        add_custom_command(TARGET clash-qt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -P "${development_qml_script}" VERBATIM)
        qt6_generate_deploy_script(TARGET clash-qt OUTPUT_SCRIPT deploy_script CONTENT "
qt_deploy_qml_imports(TARGET clash-qt PLUGINS_FOUND qml_plugins)
include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/ResolveQmlLinks.cmake\")
resolve_qml_links(\"\${QT_DEPLOY_PREFIX}/clash-qt.app\" \"${qt_qml_dir}\")
qt_deploy_runtime_dependencies(EXECUTABLE \"clash-qt.app\"
    ADDITIONAL_MODULES \${qml_plugins}
    DEPLOY_TOOL_OPTIONS \"-libpath=${qt_library_dir}\")
")
    endif()
    install(SCRIPT ${deploy_script})
endif()
if(UNIX AND NOT APPLE)
    install(FILES assets/clash-qt.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/clash-qt.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(runtime-tests
        tests/core/runtime_test.cpp
        src/core/profiles/profile_store.cpp
        src/core/config/enhance/config_enhancer.cpp
        src/core/config/yaml_util.cpp
        src/core/mihomo/process/core_process.cpp
        src/platform/service/privileged_service_client.cpp
        src/core/mihomo/controller_discovery.cpp
    )
    target_include_directories(runtime-tests PRIVATE src)
    target_link_libraries(runtime-tests PRIVATE Qt6::Test Qt6::Network Qt6::Qml Qt6::Concurrent yaml-cpp::yaml-cpp)
    add_test(NAME runtime COMMAND runtime-tests)
    qt_add_executable(controller-tests
        tests/core/controller_test.cpp
        src/core/mihomo/mihomo_client.cpp
        src/core/mihomo/controller_discovery.cpp
    )
    target_include_directories(controller-tests PRIVATE src)
    target_link_libraries(controller-tests PRIVATE Qt6::Test Qt6::Network Qt6::WebSockets yaml-cpp::yaml-cpp)
    add_test(NAME controller COMMAND controller-tests)
    qt_add_executable(provider-tests tests/core/provider_test.cpp src/core/mihomo/provider_client.cpp src/core/mihomo/mihomo_client.cpp)
    target_include_directories(provider-tests PRIVATE src)
    target_link_libraries(provider-tests PRIVATE Qt6::Test Qt6::Network Qt6::WebSockets)
    add_test(NAME provider COMMAND provider-tests)
    qt_add_executable(backup-tests tests/core/backup_test.cpp src/core/backups/backup_store.cpp)
    target_include_directories(backup-tests PRIVATE src)
    target_link_libraries(backup-tests PRIVATE Qt6::Test Qt6::Network Qt6::Concurrent yaml-cpp::yaml-cpp)
    add_test(NAME backup COMMAND backup-tests)
    qt_add_executable(data-pages-tests tests/ui/data_pages_test.cpp
        src/core/mihomo/mihomo_client.cpp src/ui/pages/proxies/proxies_page.cpp src/ui/pages/connections/connections_page.cpp
        src/ui/pages/rules/rules_page.cpp src/ui/pages/logs/logs_page.cpp src/ui/theme/formatting.cpp src/ui/theme/theme.cpp
        src/ui/pages/overview/home_page.cpp src/ui/widgets/settings_section.cpp src/ui/resources/resources.qrc src/ui/widgets/combo_box.cpp
        src/ui/pages/overview/traffic_graph.cpp src/core/telemetry/traffic_history.cpp
        src/ui/pages/providers/providers_page.cpp src/core/mihomo/provider_client.cpp)
    target_include_directories(data-pages-tests PRIVATE src)
    target_link_libraries(data-pages-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Concurrent)
    add_traffic_graph_module(data-pages-tests)
    add_test(NAME data-pages COMMAND data-pages-tests)
    qt_add_executable(proxy-tests tests/platform/system_proxy_test.cpp)
    target_include_directories(proxy-tests PRIVATE src)
    target_link_libraries(proxy-tests PRIVATE Qt6::Test)
    add_test(NAME proxy COMMAND proxy-tests)
    qt_add_executable(privileged-service-tests tests/platform/privileged_service_client_test.cpp
        src/platform/service/privileged_service_client.cpp)
    target_include_directories(privileged-service-tests PRIVATE src)
    target_link_libraries(privileged-service-tests PRIVATE Qt6::Test Qt6::Network)
    add_test(NAME privileged-service COMMAND privileged-service-tests)
    if(APPLE)
        add_test(NAME privileged-helper COMMAND clash-qt-service-helper --self-test)
        add_executable(privileged-helper-ipc-tests src/services/macos/macos_helper.mm)
        set_target_properties(privileged-helper-ipc-tests PROPERTIES AUTOMOC OFF AUTORCC OFF)
        target_compile_definitions(privileged-helper-ipc-tests PRIVATE CLASH_QT_HELPER_TESTING=1)
        target_compile_options(privileged-helper-ipc-tests PRIVATE -fobjc-arc)
        target_link_libraries(privileged-helper-ipc-tests PRIVATE "-framework Foundation" "-framework Security")
        add_test(NAME privileged-helper-ipc COMMAND privileged-helper-ipc-tests --ipc-self-test)
    endif()
    qt_add_executable(system-proxy-async-tests tests/platform/system_proxy_async_test.cpp
        src/platform/proxy/system_proxy_service.cpp src/platform/proxy/system_proxy.cpp)
    target_include_directories(system-proxy-async-tests PRIVATE src)
    target_link_libraries(system-proxy-async-tests PRIVATE Qt6::Test Qt6::Concurrent)
    if(WIN32)
        target_link_libraries(system-proxy-async-tests PRIVATE wininet)
    endif()
    add_test(NAME system-proxy-async COMMAND system-proxy-async-tests)
    qt_add_executable(tray-tests tests/ui/tray_test.cpp src/ui/shell/proxy_environment.cpp
        src/ui/shell/tray_proxy_menu.cpp src/core/mihomo/mihomo_client.cpp)
    target_include_directories(tray-tests PRIVATE src)
    target_link_libraries(tray-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network Qt6::WebSockets)
    add_test(NAME tray COMMAND tray-tests)
    qt_add_executable(routing-controls-tests tests/ui/routing_controls_test.cpp
        src/ui/shell/routing_controls.cpp src/ui/widgets/toggle_switch.cpp src/ui/theme/theme.cpp src/core/mihomo/mihomo_client.cpp
        src/ui/resources/resources.qrc)
    target_include_directories(routing-controls-tests PRIVATE src)
    target_link_libraries(routing-controls-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network Qt6::WebSockets)
    add_test(NAME routing-controls COMMAND routing-controls-tests)
    qt_add_executable(dashboard-async-tests tests/ui/dashboard_async_test.cpp
        src/ui/shell/dashboard_button.cpp src/core/mihomo/mihomo_client.cpp)
    target_include_directories(dashboard-async-tests PRIVATE src)
    target_link_libraries(dashboard-async-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Concurrent)
    add_test(NAME dashboard-async COMMAND dashboard-async-tests)
    qt_add_executable(traffic-history-tests tests/core/traffic_history_test.cpp src/core/telemetry/traffic_history.cpp)
    target_include_directories(traffic-history-tests PRIVATE src)
    target_link_libraries(traffic-history-tests PRIVATE Qt6::Test)
    add_test(NAME traffic-history COMMAND traffic-history-tests)
    set_tests_properties(runtime controller provider backup data-pages proxy tray routing-controls traffic-history dashboard-async system-proxy-async privileged-service
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(data-pages PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
