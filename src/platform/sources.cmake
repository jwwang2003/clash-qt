# Owned by the platform module (browsers, system proxy, autostart, hotkeys).
target_sources(clash-qt PRIVATE
    src/platform/browser_launcher.cpp
)

if(APPLE)
    # Launch Services: the default handler for the http scheme.
    target_link_libraries(clash-qt PRIVATE "-framework CoreServices")
endif()

include(src/platform/proxy/sources.cmake)
include(src/platform/system/sources.cmake)
