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
set(JSON_BuildTests OFF CACHE INTERNAL "RelayDesk does not build nlohmann/json tests")

if(EXISTS "${CMAKE_SOURCE_DIR}/external/nlohmann-json/CMakeLists.txt")
    add_subdirectory(external/nlohmann-json EXCLUDE_FROM_ALL)
else()
    message(FATAL_ERROR "external/nlohmann-json is missing.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RELAYDESK_HAS_EUI_NEO OFF)
if(EXISTS "${CMAKE_SOURCE_DIR}/external/EUI-NEO/CMakeLists.txt")
    add_subdirectory(external/EUI-NEO)
    set(RELAYDESK_HAS_EUI_NEO ON)
else()
    message(WARNING "external/EUI-NEO is missing; relaydesk UI target will be skipped.")
endif()

if(RELAYDESK_HAS_EUI_NEO)
    add_executable(relaydesk
        external/EUI-NEO/core/app/glfw_app_main.cpp
        src/main/app.cpp
        src/core/time.cpp
        src/core/uuid.cpp
        src/platform/computer_name_win.cpp
        src/platform/text_encoding_win.cpp
        src/storage/app_paths.cpp
        src/storage/history_store.cpp
        src/storage/local_identity.cpp
        src/storage/peer_profile.cpp
        src/storage/app_paths_win.cpp
    )

    target_include_directories(relaydesk PRIVATE src)
    target_compile_features(relaydesk PRIVATE cxx_std_23)
    target_link_libraries(relaydesk PRIVATE nlohmann_json::nlohmann_json)

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
        target_compile_options(relaydesk PRIVATE /W4 /permissive- /utf-8)
    endif()

    eui_neo_configure_app(relaydesk)

    add_custom_command(TARGET relaydesk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:relaydesk>/assets
    )
endif()

enable_testing()

add_executable(relaydesk_storage_tests
    tests/storage/app_paths_tests.cpp
    src/storage/app_paths.cpp
)

target_include_directories(relaydesk_storage_tests PRIVATE src)
target_compile_features(relaydesk_storage_tests PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(relaydesk_storage_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_storage_tests COMMAND relaydesk_storage_tests)

add_executable(relaydesk_identity_tests
    tests/storage/local_identity_tests.cpp
    src/core/time.cpp
    src/core/uuid.cpp
    src/storage/app_paths.cpp
    src/storage/local_identity.cpp
)

target_include_directories(relaydesk_identity_tests PRIVATE src)
target_compile_features(relaydesk_identity_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_identity_tests PRIVATE nlohmann_json::nlohmann_json)

target_compile_definitions(relaydesk_identity_tests PRIVATE
    RELAYDESK_TEST_WORK_DIR="${CMAKE_SOURCE_DIR}/tmp/tests/local_identity"
)

if(MSVC)
    target_compile_options(relaydesk_identity_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_identity_tests COMMAND relaydesk_identity_tests)

add_executable(relaydesk_history_tests
    tests/storage/history_store_tests.cpp
    src/storage/app_paths.cpp
    src/storage/history_store.cpp
)

target_include_directories(relaydesk_history_tests PRIVATE src)
target_compile_features(relaydesk_history_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_history_tests PRIVATE nlohmann_json::nlohmann_json)

target_compile_definitions(relaydesk_history_tests PRIVATE
    RELAYDESK_HISTORY_TEST_WORK_DIR="${CMAKE_SOURCE_DIR}/tmp/tests/history_store"
)

if(MSVC)
    target_compile_options(relaydesk_history_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_history_tests COMMAND relaydesk_history_tests)

add_executable(relaydesk_core_tests
    tests/core/time_uuid_tests.cpp
    src/core/time.cpp
    src/core/uuid.cpp
)

target_include_directories(relaydesk_core_tests PRIVATE src)
target_compile_features(relaydesk_core_tests PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(relaydesk_core_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_core_tests COMMAND relaydesk_core_tests)

add_executable(relaydesk_text_encoding_tests
    tests/platform/text_encoding_tests.cpp
    src/platform/text_encoding_win.cpp
)

target_include_directories(relaydesk_text_encoding_tests PRIVATE src)
target_compile_features(relaydesk_text_encoding_tests PRIVATE cxx_std_23)

if(WIN32)
    target_compile_definitions(relaydesk_text_encoding_tests PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
    )
endif()

if(MSVC)
    target_compile_options(relaydesk_text_encoding_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_text_encoding_tests COMMAND relaydesk_text_encoding_tests)

add_executable(relaydesk_peer_profile_tests
    tests/storage/peer_profile_tests.cpp
    src/storage/app_paths.cpp
    src/storage/peer_profile.cpp
)

target_include_directories(relaydesk_peer_profile_tests PRIVATE src)
target_compile_features(relaydesk_peer_profile_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_peer_profile_tests PRIVATE nlohmann_json::nlohmann_json)

target_compile_definitions(relaydesk_peer_profile_tests PRIVATE
    RELAYDESK_PEER_PROFILE_TEST_WORK_DIR="${CMAKE_SOURCE_DIR}/tmp/tests/peer_profile"
)

if(MSVC)
    target_compile_options(relaydesk_peer_profile_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_peer_profile_tests COMMAND relaydesk_peer_profile_tests)
