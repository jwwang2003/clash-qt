cmake_minimum_required(VERSION 3.16)

project(fx LANGUAGES CXX)

include(CTest)

add_library(fx STATIC
    src/core/object/Foundation.cpp
    src/core/object/Threading.cpp
)
add_library(fx::fx ALIAS fx)

target_compile_features(fx PUBLIC cxx_std_17)
target_include_directories(fx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(WIN32)
    target_compile_definitions(fx PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(fx PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(BUILD_TESTING)
    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        include(FetchContent)
        set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.15.2
            GIT_SHALLOW TRUE
        )
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(fx_object_tests
        tests/src/core/RefCntAutoPtrTest.cpp
    )
    target_link_libraries(fx_object_tests PRIVATE fx::fx GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(fx_object_tests)
endif()
