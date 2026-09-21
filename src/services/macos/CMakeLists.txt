# Standalone privileged helper. Apple-only; keeps its own native framework links
# and stays out of non-Apple builds entirely.
add_executable(clash-qt-service-helper macos_helper.mm)
set_target_properties(clash-qt-service-helper PROPERTIES AUTOMOC OFF AUTORCC OFF)
target_compile_options(clash-qt-service-helper PRIVATE -fobjc-arc)
target_link_libraries(clash-qt-service-helper PRIVATE
    "-framework Foundation" "-framework Security")
