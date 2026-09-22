# Installation, bundle assembly and Qt runtime deployment.
include(GNUInstallDirs)

install(TARGETS clash-qt BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
add_dependencies(clash-qt clash_qt_backend_module)
# Re-run bundle staging when only the module changes. A build-order dependency
# alone would leave an older copied module beside an unchanged executable.
set_property(TARGET clash-qt APPEND PROPERTY LINK_DEPENDS
    "$<TARGET_FILE:clash_qt_backend_module>")
if(APPLE)
    # Copy before Qt deployment so the module's runtime dependencies are also
    # resolved inside the bundle. The loader uses this installed location.
    add_custom_command(TARGET clash-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:clash-qt>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:clash_qt_backend_module>"
            "$<TARGET_BUNDLE_DIR:clash-qt>/Contents/Frameworks/$<TARGET_FILE_NAME:clash_qt_backend_module>"
        VERBATIM)
else()
    add_custom_command(TARGET clash-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:clash_qt_backend_module>" "$<TARGET_FILE_DIR:clash-qt>"
        VERBATIM)
    install(TARGETS clash_qt_backend_module
        LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:clash-qt> DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES $<TARGET_RUNTIME_DLLS:clash_qt_backend_module> DESTINATION ${CMAKE_INSTALL_BINDIR})
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
        list(APPEND deploy_options DEPLOY_TOOL_OPTIONS
            "-libpath=${qt_library_dir}"
            "-executable=$<TARGET_BUNDLE_DIR:clash-qt>/Contents/Frameworks/$<TARGET_FILE_NAME:clash_qt_backend_module>")
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
        \"clash-qt.app/Contents/Frameworks/$<TARGET_FILE_NAME:clash_qt_backend_module>\"
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

# Stage the locally built engine beside the application executable. G1 requires
# the package to consume this exact build output. discoverBinary() already looks
# next to the executable first, so no runtime change is needed to find it.
#
# Installed AFTER the Qt deployment script on purpose: macdeployqt rewrites and
# signs what it finds in the bundle, and the engine is a self-contained Go binary
# that must not be processed as a Qt executable.
if(APPLE)
    install(PROGRAMS "${CLASH_QT_CORE_BINARY}"
        DESTINATION "clash-qt.app/Contents/MacOS")
    install(FILES "${CLASH_QT_CORE_DIR}/mihomo-provenance.json"
        DESTINATION "clash-qt.app/Contents/Resources")
    # Qt deployment runs before the Go engine is copied. Seal the completed
    # developer bundle afterwards without re-signing the pinned engine itself.
    # Ad-hoc signing needs no certificate or trust-store change. Unlike a tool
    # that only logs an error, verification failure must fail `make package`.
    find_program(clash_qt_codesign NAMES codesign REQUIRED)
    install(CODE "
set(_bundle \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/clash-qt.app\")
execute_process(COMMAND \"${clash_qt_codesign}\" --force --sign -
    \"\${_bundle}/Contents/Frameworks/$<TARGET_FILE_NAME:clash_qt_backend_module>\"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND \"${clash_qt_codesign}\" --force --sign -
    \"\${_bundle}/Contents/Helpers/clash-qt-service-helper\"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND \"${clash_qt_codesign}\" --force --sign -
    \"\${_bundle}\"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND \"${clash_qt_codesign}\" --verify --deep --strict
    \"\${_bundle}\"
    COMMAND_ERROR_IS_FATAL ANY)
")
else()
    install(PROGRAMS "${CLASH_QT_CORE_BINARY}" DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES "${CLASH_QT_CORE_DIR}/mihomo-provenance.json"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/clash-qt)
endif()
