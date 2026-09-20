cmake_minimum_required(VERSION 3.21)
project(clash-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network WebSockets Qml)
find_package(yaml-cpp REQUIRED)

qt_standard_project_setup()

qt_add_executable(clash-qt src/main.cpp)

target_include_directories(clash-qt PRIVATE src)
target_link_libraries(clash-qt PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    Qt6::Qml          # QJSEngine, for the profile enhancement scripts
    yaml-cpp::yaml-cpp
)

# Each module owns its own source list so modules can be worked on independently.
include(src/core/sources.cmake)
include(src/platform/sources.cmake)
include(src/ui/sources.cmake)

set_target_properties(clash-qt PROPERTIES MACOSX_BUNDLE OFF)
