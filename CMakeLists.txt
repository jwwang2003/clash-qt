cmake_minimum_required(VERSION 3.21)
project(clash-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network WebSockets Qml Concurrent)
find_package(yaml-cpp REQUIRED)

qt_standard_project_setup()

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

set_target_properties(clash-qt PROPERTIES MACOSX_BUNDLE OFF)

if(APPLE)
    enable_language(OBJCXX)
    add_executable(clash-qt-service-helper src/service/macos_helper.mm)
    set_target_properties(clash-qt-service-helper PROPERTIES AUTOMOC OFF AUTORCC OFF)
    target_compile_options(clash-qt-service-helper PRIVATE -fobjc-arc)
    target_link_libraries(clash-qt-service-helper PRIVATE "-framework Foundation" "-framework Security")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(runtime-tests
        tests/runtime_test.cpp
        src/core/profile/profile_store.cpp
        src/core/enhance/config_enhancer.cpp
        src/core/yaml_util.cpp
        src/core/process/core_process.cpp
        src/platform/service/privileged_service_client.cpp
        src/core/controller_discovery.cpp
    )
    target_include_directories(runtime-tests PRIVATE src)
    target_link_libraries(runtime-tests PRIVATE Qt6::Test Qt6::Network Qt6::Qml Qt6::Concurrent yaml-cpp::yaml-cpp)
    add_test(NAME runtime COMMAND runtime-tests)
    qt_add_executable(controller-tests
        tests/controller_test.cpp
        src/core/mihomo_client.cpp
        src/core/controller_discovery.cpp
    )
    target_include_directories(controller-tests PRIVATE src)
    target_link_libraries(controller-tests PRIVATE Qt6::Test Qt6::Network Qt6::WebSockets yaml-cpp::yaml-cpp)
    add_test(NAME controller COMMAND controller-tests)
    qt_add_executable(provider-tests tests/provider_test.cpp src/core/provider_client.cpp src/core/mihomo_client.cpp)
    target_include_directories(provider-tests PRIVATE src)
    target_link_libraries(provider-tests PRIVATE Qt6::Test Qt6::Network Qt6::WebSockets)
    add_test(NAME provider COMMAND provider-tests)
    qt_add_executable(backup-tests tests/backup_test.cpp src/core/backup_store.cpp)
    target_include_directories(backup-tests PRIVATE src)
    target_link_libraries(backup-tests PRIVATE Qt6::Test Qt6::Network Qt6::Concurrent yaml-cpp::yaml-cpp)
    add_test(NAME backup COMMAND backup-tests)
    qt_add_executable(proxy-tests tests/system_proxy_test.cpp)
    target_include_directories(proxy-tests PRIVATE src)
    target_link_libraries(proxy-tests PRIVATE Qt6::Test)
    add_test(NAME proxy COMMAND proxy-tests)
    qt_add_executable(privileged-service-tests tests/privileged_service_client_test.cpp
        src/platform/service/privileged_service_client.cpp)
    target_include_directories(privileged-service-tests PRIVATE src)
    target_link_libraries(privileged-service-tests PRIVATE Qt6::Test Qt6::Network)
    add_test(NAME privileged-service COMMAND privileged-service-tests)
    if(APPLE)
        add_test(NAME privileged-helper COMMAND clash-qt-service-helper --self-test)
        add_executable(privileged-helper-ipc-tests src/service/macos_helper.mm)
        set_target_properties(privileged-helper-ipc-tests PROPERTIES AUTOMOC OFF AUTORCC OFF)
        target_compile_definitions(privileged-helper-ipc-tests PRIVATE CLASH_QT_HELPER_TESTING=1)
        target_compile_options(privileged-helper-ipc-tests PRIVATE -fobjc-arc)
        target_link_libraries(privileged-helper-ipc-tests PRIVATE "-framework Foundation" "-framework Security")
        add_test(NAME privileged-helper-ipc COMMAND privileged-helper-ipc-tests --ipc-self-test)
    endif()
    qt_add_executable(system-proxy-async-tests tests/system_proxy_async_test.cpp
        src/platform/proxy/system_proxy_service.cpp src/platform/proxy/system_proxy.cpp)
    target_include_directories(system-proxy-async-tests PRIVATE src)
    target_link_libraries(system-proxy-async-tests PRIVATE Qt6::Test Qt6::Concurrent)
    if(WIN32)
        target_link_libraries(system-proxy-async-tests PRIVATE wininet)
    endif()
    add_test(NAME system-proxy-async COMMAND system-proxy-async-tests)
    qt_add_executable(traffic-history-tests tests/traffic_history_test.cpp src/core/traffic_history.cpp)
    target_include_directories(traffic-history-tests PRIVATE src)
    target_link_libraries(traffic-history-tests PRIVATE Qt6::Test)
    add_test(NAME traffic-history COMMAND traffic-history-tests)
    set_tests_properties(runtime controller provider backup proxy privileged-service system-proxy-async traffic-history
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
