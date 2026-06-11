cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(RelayDesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(EUI_BUILD_APPS OFF CACHE BOOL "Build EUI-NEO examples" FORCE)
set(EUI_BUILD_TEST_FIXTURES OFF CACHE BOOL "Build EUI-NEO test fixtures" FORCE)
set(EUI_DEPS_MODE bundled CACHE STRING "Use vendored EUI-NEO dependencies" FORCE)
set(EUI_ENABLE_MARKDOWN OFF CACHE BOOL "RelayDesk does not need markdown yet" FORCE)

add_subdirectory(external/EUI-NEO)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(relaydesk
    external/EUI-NEO/core/app/glfw_app_main.cpp
    src/main/app.cpp
    src/platform/computer_name_win.cpp
    src/storage/app_paths.cpp
    src/storage/app_paths_win.cpp
)

target_include_directories(relaydesk PRIVATE src)
target_compile_features(relaydesk PRIVATE cxx_std_23)

if(WIN32)
    target_compile_definitions(relaydesk PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
    )
else()
    message(FATAL_ERROR "RelayDesk currently targets Windows only.")
endif()

if(MSVC)
    target_compile_options(relaydesk PRIVATE /W4 /permissive-)
endif()

eui_neo_configure_app(relaydesk)

enable_testing()

add_executable(relaydesk_storage_tests
    tests/storage/app_paths_tests.cpp
    src/storage/app_paths.cpp
)

target_include_directories(relaydesk_storage_tests PRIVATE src)
target_compile_features(relaydesk_storage_tests PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(relaydesk_storage_tests PRIVATE /W4 /permissive-)
endif()

add_test(NAME relaydesk_storage_tests COMMAND relaydesk_storage_tests)
