cmake_minimum_required(VERSION 3.21)
project(clash-qt-component-consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.9 REQUIRED COMPONENTS Core)

# This is a standalone project: no add_subdirectory of the desktop, no private
# engine headers, and no project implementation library on the link line.
set(CLASH_QT_COMPONENT_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src"
    CACHE PATH "Public component SDK include root (contains core/component)")
add_executable(component_consumer main.cpp)
target_include_directories(component_consumer PRIVATE "${CLASH_QT_COMPONENT_INCLUDE_DIR}")
target_link_libraries(component_consumer PRIVATE Qt6::Core ${CMAKE_DL_LIBS})
