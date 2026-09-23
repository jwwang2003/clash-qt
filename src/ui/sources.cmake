# Owned by the UI module. Each substantial group keeps its own source manifest.
target_sources(${clash_ui_target} PRIVATE
    src/ui/resources/resources.qrc
)

include(src/ui/shell/sources.cmake)
include(src/ui/pages/sources.cmake)
include(src/ui/widgets/sources.cmake)
include(src/ui/theme/sources.cmake)
