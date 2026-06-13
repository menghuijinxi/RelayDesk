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
option(RELAYDESK_ENABLE_BOOST_ASIO "Enable Boost.Asio network backend" ON)
option(RELAYDESK_USE_SKIA_EUI_BACKEND "Use the experimental Skia-backed EUI renderer" OFF)

if(EXISTS "${CMAKE_SOURCE_DIR}/external/nlohmann-json/CMakeLists.txt")
    add_subdirectory(external/nlohmann-json EXCLUDE_FROM_ALL)
else()
    message(FATAL_ERROR "external/nlohmann-json is missing.")
endif()

set(RELAYDESK_HAS_BOOST_ASIO OFF)
if(RELAYDESK_ENABLE_BOOST_ASIO)
    find_package(Boost CONFIG QUIET COMPONENTS asio)
    find_path(RELAYDESK_BOOST_ASIO_INCLUDE_DIR
        NAMES boost/asio.hpp
        HINTS ENV BOOST_INCLUDEDIR ENV BOOST_ROOT
        PATH_SUFFIXES include
    )

    if(Boost_FOUND OR RELAYDESK_BOOST_ASIO_INCLUDE_DIR)
        add_library(relaydesk_boost_asio INTERFACE)
        if(Boost_FOUND AND TARGET Boost::asio)
            target_link_libraries(relaydesk_boost_asio INTERFACE Boost::asio)
        elseif(Boost_FOUND AND TARGET Boost::headers)
            target_link_libraries(relaydesk_boost_asio INTERFACE Boost::headers)
        elseif(Boost_FOUND AND TARGET Boost::boost)
            target_link_libraries(relaydesk_boost_asio INTERFACE Boost::boost)
        else()
            target_include_directories(relaydesk_boost_asio
                INTERFACE ${RELAYDESK_BOOST_ASIO_INCLUDE_DIR})
        endif()

        target_compile_definitions(relaydesk_boost_asio INTERFACE
            BOOST_ALL_NO_LIB
            BOOST_ERROR_CODE_HEADER_ONLY
        )

        if(WIN32)
            target_compile_definitions(relaydesk_boost_asio INTERFACE
                NOMINMAX
                WIN32_LEAN_AND_MEAN
                _WIN32_WINNT=0x0A00
            )
            target_link_libraries(relaydesk_boost_asio
                INTERFACE ws2_32 mswsock iphlpapi)
        endif()

        set(RELAYDESK_HAS_BOOST_ASIO ON)
    else()
        message(WARNING "Boost was not found; Boost.Asio network backend is skipped.")
    endif()
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RELAYDESK_HAS_EUI_NEO OFF)
set(RELAYDESK_EUI_NEO_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/external/EUI-NEO")
set(RELAYDESK_EUI_NEO_SOURCE_DIR "${RELAYDESK_EUI_NEO_SOURCE_ROOT}")
if(EXISTS "${RELAYDESK_EUI_NEO_SOURCE_ROOT}/CMakeLists.txt")
    if(RELAYDESK_USE_SKIA_EUI_BACKEND)
        set(RELAYDESK_EUI_NEO_SOURCE_DIR
            "${CMAKE_BINARY_DIR}/_deps/eui-neo-skia-src")
        set(RELAYDESK_EUI_NEO_BINARY_DIR
            "${CMAKE_BINARY_DIR}/_deps/eui-neo-skia-build")
        file(REMOVE_RECURSE "${RELAYDESK_EUI_NEO_SOURCE_DIR}")
        file(COPY "${RELAYDESK_EUI_NEO_SOURCE_ROOT}/"
             DESTINATION "${RELAYDESK_EUI_NEO_SOURCE_DIR}"
             PATTERN ".git" EXCLUDE)

        find_package(unofficial-skia CONFIG REQUIRED)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchEuiNeoSkiaTextMetrics.cmake")
        relaydesk_patch_eui_neo_skia_text_metrics("${RELAYDESK_EUI_NEO_SOURCE_DIR}")
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchEuiNeoSkiaTextSource.cmake")
        relaydesk_patch_eui_neo_skia_text_source("${RELAYDESK_EUI_NEO_SOURCE_DIR}")
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchEuiNeoSkiaBackend.cmake")
        relaydesk_patch_eui_neo_skia_backend("${RELAYDESK_EUI_NEO_SOURCE_DIR}")

        add_subdirectory("${RELAYDESK_EUI_NEO_SOURCE_DIR}"
                         "${RELAYDESK_EUI_NEO_BINARY_DIR}")
    else()
        add_subdirectory("${RELAYDESK_EUI_NEO_SOURCE_DIR}")
    endif()

    set(RELAYDESK_HAS_EUI_NEO ON)
    if(RELAYDESK_USE_SKIA_EUI_BACKEND)
        target_sources(eui_neo PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/eui-skia/SkiaOpenGLRenderBackend.cpp")
        target_include_directories(eui_neo PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/eui-skia")
        target_link_libraries(eui_neo PUBLIC unofficial::skia::skia)
        target_compile_definitions(eui_neo PRIVATE RELAYDESK_EUI_SKIA_BACKEND=1)
    endif()
else()
    message(WARNING "external/EUI-NEO is missing; relaydesk UI target will be skipped.")
endif()

if(RELAYDESK_HAS_EUI_NEO)
    add_executable(relaydesk
        "${RELAYDESK_EUI_NEO_SOURCE_DIR}/core/app/glfw_app_main.cpp"
        src/main/app.cpp
        src/main/app_runtime.cpp
        src/core/diagnostic_log.cpp
        src/core/time.cpp
        src/core/uuid.cpp
        src/platform/computer_name_win.cpp
        src/platform/text_encoding_win.cpp
        src/platform/windows_install_id_win.cpp
        src/net/discovery_local_announcement.cpp
        src/net/discovery_message.cpp
        src/net/discovery_peer_profile.cpp
        src/net/discovery_processor.cpp
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
        target_sources(relaydesk PRIVATE
            ${CMAKE_SOURCE_DIR}/relaydesk.rc
        )
    else()
        message(FATAL_ERROR "RelayDesk currently targets Windows only.")
    endif()

    if(MSVC)
        target_compile_options(relaydesk PRIVATE /W4 /permissive- /utf-8)
    endif()

    if(RELAYDESK_HAS_BOOST_ASIO)
        target_sources(relaydesk PRIVATE
            src/net/boost_asio_udp_discovery_transport.cpp
            src/net/discovery_service.cpp
            src/net/discovery_worker.cpp
        )
        target_compile_definitions(relaydesk PRIVATE RELAYDESK_HAS_BOOST_ASIO)
        target_link_libraries(relaydesk PRIVATE relaydesk_boost_asio)
    endif()

    eui_neo_configure_app(relaydesk)

    add_custom_command(TARGET relaydesk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:relaydesk>/assets
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:relaydesk>/assets/Font Awesome 7 Free-Solid-900.otf"
            "$<TARGET_FILE_DIR:relaydesk>/assets/JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf"
            "$<TARGET_FILE_DIR:relaydesk>/assets/YouSheBiaoTiHei-2.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/assets/icon.ico
            ${CMAKE_SOURCE_DIR}/assets/icon.png
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

add_executable(relaydesk_discovery_message_tests
    tests/net/discovery_message_tests.cpp
    src/net/discovery_message.cpp
)

target_include_directories(relaydesk_discovery_message_tests PRIVATE src)
target_compile_features(relaydesk_discovery_message_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_discovery_message_tests PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(relaydesk_discovery_message_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_discovery_message_tests COMMAND relaydesk_discovery_message_tests)

add_executable(relaydesk_discovery_local_announcement_tests
    tests/net/discovery_local_announcement_tests.cpp
    src/core/time.cpp
    src/core/uuid.cpp
    src/net/discovery_local_announcement.cpp
    src/net/discovery_message.cpp
    src/storage/app_paths.cpp
    src/storage/local_identity.cpp
)

target_include_directories(relaydesk_discovery_local_announcement_tests PRIVATE src)
target_compile_features(relaydesk_discovery_local_announcement_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_discovery_local_announcement_tests
    PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(relaydesk_discovery_local_announcement_tests
        PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_discovery_local_announcement_tests
         COMMAND relaydesk_discovery_local_announcement_tests)

add_executable(relaydesk_discovery_peer_profile_tests
    tests/net/discovery_peer_profile_tests.cpp
    src/net/discovery_message.cpp
    src/net/discovery_peer_profile.cpp
    src/storage/app_paths.cpp
    src/storage/peer_profile.cpp
)

target_include_directories(relaydesk_discovery_peer_profile_tests PRIVATE src)
target_compile_features(relaydesk_discovery_peer_profile_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_discovery_peer_profile_tests
    PRIVATE nlohmann_json::nlohmann_json)

target_compile_definitions(relaydesk_discovery_peer_profile_tests PRIVATE
    RELAYDESK_DISCOVERY_TEST_WORK_DIR="${CMAKE_SOURCE_DIR}/tmp/tests/discovery"
)

if(MSVC)
    target_compile_options(relaydesk_discovery_peer_profile_tests
        PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_discovery_peer_profile_tests
         COMMAND relaydesk_discovery_peer_profile_tests)

add_executable(relaydesk_discovery_processor_tests
    tests/net/discovery_processor_tests.cpp
    src/net/discovery_message.cpp
    src/net/discovery_peer_profile.cpp
    src/net/discovery_processor.cpp
    src/storage/app_paths.cpp
    src/storage/peer_profile.cpp
)

target_include_directories(relaydesk_discovery_processor_tests PRIVATE src)
target_compile_features(relaydesk_discovery_processor_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_discovery_processor_tests
    PRIVATE nlohmann_json::nlohmann_json)

set(RELAYDESK_DISCOVERY_PROCESSOR_TEST_PATH
    "${CMAKE_SOURCE_DIR}/tmp/tests/discovery_processor")

target_compile_definitions(relaydesk_discovery_processor_tests PRIVATE
    "RELAYDESK_DISCOVERY_PROCESSOR_TEST_WORK_DIR=\"${RELAYDESK_DISCOVERY_PROCESSOR_TEST_PATH}\""
)

if(MSVC)
    target_compile_options(relaydesk_discovery_processor_tests
        PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_discovery_processor_tests
         COMMAND relaydesk_discovery_processor_tests)

if(RELAYDESK_HAS_BOOST_ASIO)
    add_executable(relaydesk_boost_asio_udp_discovery_transport_tests
        tests/net/boost_asio_udp_discovery_transport_tests.cpp
        src/net/boost_asio_udp_discovery_transport.cpp
    )

    target_include_directories(relaydesk_boost_asio_udp_discovery_transport_tests
        PRIVATE src)
    target_compile_features(relaydesk_boost_asio_udp_discovery_transport_tests
        PRIVATE cxx_std_23)
    target_link_libraries(relaydesk_boost_asio_udp_discovery_transport_tests
        PRIVATE relaydesk_boost_asio)

    if(MSVC)
        target_compile_options(relaydesk_boost_asio_udp_discovery_transport_tests
            PRIVATE /W4 /permissive- /utf-8)
    endif()

    add_test(NAME relaydesk_boost_asio_udp_discovery_transport_tests
             COMMAND relaydesk_boost_asio_udp_discovery_transport_tests)

    add_executable(relaydesk_discovery_service_tests
        tests/net/discovery_service_tests.cpp
        src/core/diagnostic_log.cpp
        src/core/time.cpp
        src/core/uuid.cpp
        src/net/boost_asio_udp_discovery_transport.cpp
        src/net/discovery_local_announcement.cpp
        src/net/discovery_message.cpp
        src/net/discovery_peer_profile.cpp
        src/net/discovery_processor.cpp
        src/net/discovery_service.cpp
        src/storage/app_paths.cpp
        src/storage/local_identity.cpp
        src/storage/peer_profile.cpp
    )

    target_include_directories(relaydesk_discovery_service_tests PRIVATE src)
    target_compile_features(relaydesk_discovery_service_tests PRIVATE cxx_std_23)
    target_link_libraries(relaydesk_discovery_service_tests
        PRIVATE relaydesk_boost_asio nlohmann_json::nlohmann_json)

    set(RELAYDESK_DISCOVERY_SERVICE_TEST_PATH
        "${CMAKE_SOURCE_DIR}/tmp/tests/discovery_service")

    target_compile_definitions(relaydesk_discovery_service_tests PRIVATE
        "RELAYDESK_DISCOVERY_SERVICE_TEST_WORK_DIR=\"${RELAYDESK_DISCOVERY_SERVICE_TEST_PATH}\""
    )

    if(MSVC)
        target_compile_options(relaydesk_discovery_service_tests
            PRIVATE /W4 /permissive- /utf-8)
    endif()

    add_test(NAME relaydesk_discovery_service_tests
             COMMAND relaydesk_discovery_service_tests)

    add_executable(relaydesk_discovery_worker_tests
        tests/net/discovery_worker_tests.cpp
        src/core/diagnostic_log.cpp
        src/core/time.cpp
        src/core/uuid.cpp
        src/net/boost_asio_udp_discovery_transport.cpp
        src/net/discovery_local_announcement.cpp
        src/net/discovery_message.cpp
        src/net/discovery_peer_profile.cpp
        src/net/discovery_processor.cpp
        src/net/discovery_service.cpp
        src/net/discovery_worker.cpp
        src/storage/app_paths.cpp
        src/storage/local_identity.cpp
        src/storage/peer_profile.cpp
    )

    target_include_directories(relaydesk_discovery_worker_tests PRIVATE src)
    target_compile_features(relaydesk_discovery_worker_tests PRIVATE cxx_std_23)
    target_link_libraries(relaydesk_discovery_worker_tests
        PRIVATE relaydesk_boost_asio nlohmann_json::nlohmann_json)

    set(RELAYDESK_DISCOVERY_WORKER_TEST_PATH
        "${CMAKE_SOURCE_DIR}/tmp/tests/discovery_worker")

    target_compile_definitions(relaydesk_discovery_worker_tests PRIVATE
        "RELAYDESK_DISCOVERY_WORKER_TEST_WORK_DIR=\"${RELAYDESK_DISCOVERY_WORKER_TEST_PATH}\""
    )

    if(MSVC)
        target_compile_options(relaydesk_discovery_worker_tests
            PRIVATE /W4 /permissive- /utf-8)
    endif()

    add_test(NAME relaydesk_discovery_worker_tests
             COMMAND relaydesk_discovery_worker_tests)

    add_executable(relaydesk_discovery_process_smoke_tests
        tests/net/discovery_process_smoke_tests.cpp
        src/core/diagnostic_log.cpp
        src/core/time.cpp
        src/core/uuid.cpp
        src/net/boost_asio_udp_discovery_transport.cpp
        src/net/discovery_local_announcement.cpp
        src/net/discovery_message.cpp
        src/net/discovery_peer_profile.cpp
        src/net/discovery_processor.cpp
        src/net/discovery_service.cpp
        src/net/discovery_worker.cpp
        src/platform/text_encoding_win.cpp
        src/storage/app_paths.cpp
        src/storage/local_identity.cpp
        src/storage/peer_profile.cpp
    )

    target_include_directories(relaydesk_discovery_process_smoke_tests PRIVATE src)
    target_compile_features(relaydesk_discovery_process_smoke_tests
        PRIVATE cxx_std_23)
    target_link_libraries(relaydesk_discovery_process_smoke_tests
        PRIVATE relaydesk_boost_asio nlohmann_json::nlohmann_json)

    set(RELAYDESK_DISCOVERY_PROCESS_TEST_PATH
        "${CMAKE_SOURCE_DIR}/tmp/tests/discovery_process")

    target_compile_definitions(relaydesk_discovery_process_smoke_tests PRIVATE
        "RELAYDESK_DISCOVERY_PROCESS_TEST_WORK_DIR=\"${RELAYDESK_DISCOVERY_PROCESS_TEST_PATH}\""
    )

    if(MSVC)
        target_compile_options(relaydesk_discovery_process_smoke_tests
            PRIVATE /W4 /permissive- /utf-8)
    endif()

    add_test(NAME relaydesk_discovery_process_smoke_tests
             COMMAND relaydesk_discovery_process_smoke_tests)
endif()

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
