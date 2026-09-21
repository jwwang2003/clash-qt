# Owned by the ui/pages module. One entry per page implementation.
target_sources(${clash_ui_target} PRIVATE
    src/ui/pages/overview/home_page.cpp
    src/ui/pages/overview/traffic_graph.cpp
    src/ui/pages/profiles/profiles_page.cpp
    src/ui/pages/proxies/proxies_page.cpp
    src/ui/pages/connections/connections_page.cpp
    src/ui/pages/rules/rules_page.cpp
    src/ui/pages/providers/providers_page.cpp
    src/ui/pages/logs/logs_page.cpp
    src/ui/pages/backups/backup_page.cpp
    src/ui/pages/settings/settings_page.cpp
    src/ui/pages/settings/service_settings.cpp
    src/ui/pages/settings/hotkey_settings.cpp
    src/ui/pages/settings/chain_editor.cpp
)
