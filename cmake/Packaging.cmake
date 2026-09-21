# Installation, bundle assembly and Qt runtime deployment.
include(GNUInstallDirs)

install(TARGETS clash-qt BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:clash-qt> DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(APPLE)
    # The POST_BUILD copy is the only path the helper has into the shipped bundle;
    # there is no install(TARGETS) rule for it. privileged_service_installer.cpp
    # resolves it bundle-relatively as "../Helpers/clash-qt-service-helper".
    add_dependencies(clash-qt clash-qt-service-helper)
    add_custom_command(TARGET clash-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:clash-qt>/Contents/Helpers"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:clash-qt-service-helper>"
            "$<TARGET_BUNDLE_DIR:clash-qt>/Contents/Helpers/clash-qt-service-helper"
        VERBATIM)
    # Keep the original development command pointing at the current bundle.
    add_custom_command(TARGET clash-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_CURRENT_BINARY_DIR}/clash-qt"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "clash-qt.app/Contents/MacOS/clash-qt" "${CMAKE_CURRENT_BINARY_DIR}/clash-qt"
        VERBATIM)
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
        find_program(qtpaths_program NAMES qtpaths6 qtpaths
            HINTS "${qt_library_dir}/../bin" REQUIRED NO_DEFAULT_PATH)
        execute_process(COMMAND "${qtpaths_program}" --query QT_INSTALL_QML
            OUTPUT_VARIABLE qt_qml_dir OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        # The development bundle also receives QML imports at POST_BUILD.
        # Repair SDK-relative metadata links there, not just in installed apps.
        set(development_qml_script "${CMAKE_CURRENT_BINARY_DIR}/resolve_development_qml.cmake")
        file(GENERATE OUTPUT "${development_qml_script}" CONTENT "
include(\"${PROJECT_SOURCE_DIR}/cmake/ResolveQmlLinks.cmake\")
resolve_qml_links(\"$<TARGET_BUNDLE_DIR:clash-qt>\" \"${qt_qml_dir}\")
")
        add_custom_command(TARGET clash-qt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -P "${development_qml_script}" VERBATIM)
        qt6_generate_deploy_script(TARGET clash-qt OUTPUT_SCRIPT deploy_script CONTENT "
qt_deploy_qml_imports(TARGET clash-qt PLUGINS_FOUND qml_plugins)
include(\"${PROJECT_SOURCE_DIR}/cmake/ResolveQmlLinks.cmake\")
resolve_qml_links(\"\${QT_DEPLOY_PREFIX}/clash-qt.app\" \"${qt_qml_dir}\")
qt_deploy_runtime_dependencies(EXECUTABLE \"clash-qt.app\"
    ADDITIONAL_MODULES \${qml_plugins}
    DEPLOY_TOOL_OPTIONS \"-libpath=${qt_library_dir}\")
")
    endif()
    install(SCRIPT ${deploy_script})
endif()

if(UNIX AND NOT APPLE)
    install(FILES assets/clash-qt.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/clash-qt.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
