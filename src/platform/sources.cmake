# Owned by the platform module (browsers, system proxy, autostart, hotkeys).
target_sources(clash-qt PRIVATE
    src/platform/browser/browser_launcher.cpp
    src/platform/service/privileged_service_client.cpp
    src/platform/service/privileged_service_installer.cpp
)

if(APPLE)
    # Launch Services: the default handler for the http scheme.
    target_link_libraries(clash-qt PRIVATE "-framework CoreServices" "-framework Security")
endif()

if(WIN32)
    target_link_libraries(clash-qt PRIVATE ole32 shell32)
endif()

include(src/platform/proxy/sources.cmake)
include(src/platform/system/sources.cmake)
