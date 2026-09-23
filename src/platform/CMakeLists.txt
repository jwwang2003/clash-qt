# Operating-system adapters. No dependency on core, app or UI code.
add_library(clash_platform STATIC
    browser/browser_launcher.cpp
    proxy/system_proxy.cpp
    proxy/system_proxy_service.cpp
    service/privileged_service_client.cpp
    service/privileged_service_installer.cpp
    system/autostart.cpp
    system/hotkeys.cpp)

# Qt6::Gui is PUBLIC because system/hotkeys.h exposes QKeySequence. This is an
# allowed dependency for a platform adapter; Widgets, Quick and Graphs are not.
target_link_libraries(clash_platform
    PUBLIC  clash_types Qt6::Gui
    PRIVATE Qt6::Network Qt6::Concurrent)

if(APPLE)
    # CoreServices: default handler for the http scheme. Carbon: RegisterEventHotKey
    # is still the only system-wide hotkey API.
    target_link_libraries(clash_platform PRIVATE
        "-framework CoreServices" "-framework Security" "-framework Carbon")
elseif(WIN32)
    target_link_libraries(clash_platform PRIVATE ole32 shell32 wininet)
elseif(UNIX)
    find_package(X11 REQUIRED)   # XGrabKey and the keysym lookup behind it
    target_link_libraries(clash_platform PRIVATE X11::X11)
endif()
