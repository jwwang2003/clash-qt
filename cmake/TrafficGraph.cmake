# QML module carrying the Qt Graphs traffic view.
#
# The alias property is applied *inside* the function on purpose. QT_RESOURCE_ALIAS
# is directory-scoped, and this module is built for two targets that now live in
# different directory scopes (clash-qt at the root, data-pages-tests under tests/).
# Setting it once at the root would silently leave the test target's QML registered
# under its full source path, which fails at runtime rather than at build time.
function(add_traffic_graph_module target)
    set(qml_file "${PROJECT_SOURCE_DIR}/src/ui/pages/overview/qml/TrafficGraph.qml")
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS TrafficGraph.qml)
    target_link_libraries(${target} PRIVATE Qt6::Quick Qt6::Graphs)
    qt_add_qml_module(${target}
        URI ClashQt
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml/${target}/ClashQt"
        QML_FILES "${qml_file}"
    )
endfunction()
