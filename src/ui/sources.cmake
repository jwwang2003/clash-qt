# Owned by the UI module. Add new ui/*.cpp files here.
target_sources(clash-qt PRIVATE
    src/ui/resources.qrc
    src/ui/combo_box.cpp
    src/ui/chain_editor.cpp
    src/ui/connections_page.cpp
    src/ui/dashboard_button.cpp
    src/ui/formatting.cpp
    src/ui/hotkey_settings.cpp
    src/ui/home_page.cpp
    src/ui/backup_page.cpp
    src/ui/logs_page.cpp
    src/ui/main_window.cpp
    src/ui/profiles_page.cpp
    src/ui/providers_page.cpp
    src/ui/proxies_page.cpp
    src/ui/rules_page.cpp
    src/ui/settings_page.cpp
    src/ui/service_settings.cpp
    src/ui/settings_section.cpp
    src/ui/theme.cpp
    src/ui/tray_icon.cpp
    src/ui/tray_proxy_menu.cpp
    src/ui/proxy_environment.cpp
    src/ui/routing_controls.cpp
    src/ui/toggle_switch.cpp
    src/ui/traffic_graph.cpp
)
