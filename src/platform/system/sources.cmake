# Owned by the platform/system module.
target_sources(clash-qt PRIVATE
    src/platform/system/autostart.cpp
    src/platform/system/hotkeys.cpp
)

if(APPLE)
    # Carbon: RegisterEventHotKey is still the only system-wide hotkey API.
    target_link_libraries(clash-qt PRIVATE "-framework Carbon")
elseif(UNIX)
    # Xlib: XGrabKey and the keysym lookup behind it.
    find_package(X11 REQUIRED)
    target_link_libraries(clash-qt PRIVATE X11::X11)
endif()
