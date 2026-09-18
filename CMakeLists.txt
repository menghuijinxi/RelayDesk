cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(RelayDesk LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF)

if(MSVC AND NOT CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(EUI_BUILD_APPS OFF CACHE BOOL "Build EUI-NEO examples" FORCE)
set(EUI_BUILD_TEST_FIXTURES OFF CACHE BOOL "Build EUI-NEO test fixtures" FORCE)
set(EUI_DEPS_MODE bundled CACHE STRING "Use vendored EUI-NEO dependencies" FORCE)
set(EUI_ENABLE_MARKDOWN OFF CACHE BOOL "RelayDesk does not need markdown yet" FORCE)
option(RELAYDESK_ENABLE_BOOST_ASIO "Enable Boost.Asio network backend" ON)
option(RELAYDESK_USE_SKIA_EUI_BACKEND "Use the experimental Skia-backed EUI renderer" OFF)
option(RELAYDESK_BUILD_SKIAUI "Build the standalone SkiaUI RelayDesk executable" OFF)
set(RELAYDESK_TEST_SANDBOX_PARENT "${CMAKE_SOURCE_DIR}/tmp/tests")

set(RELAYDESK_EUI_NEO_GIT_REPOSITORY
    "https://github.com/sudoevolve/EUI-NEO.git"
    CACHE STRING "EUI-NEO Git repository used by FetchContent")
set(RELAYDESK_EUI_NEO_GIT_TAG
    "f2dfa6eae5b7c344ad68a3eb152eabe78330a02c"
    CACHE STRING "EUI-NEO Git commit used by FetchContent")

find_package(nlohmann_json CONFIG REQUIRED)

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
if(RELAYDESK_USE_SKIA_EUI_BACKEND)
    find_package(unofficial-skia CONFIG REQUIRED)
    FetchContent_Declare(eui_neo_skia
        GIT_REPOSITORY "${RELAYDESK_EUI_NEO_GIT_REPOSITORY}"
        GIT_TAG "${RELAYDESK_EUI_NEO_GIT_TAG}"
        PATCH_COMMAND
            ${CMAKE_COMMAND}
             "-DRELAYDESK_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
             "-DRELAYDESK_BUILD_DIR=${CMAKE_BINARY_DIR}"
             "-DEUI_NEO_SOURCE_DIR=<SOURCE_DIR>"
             -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchEuiNeoSkia.cmake"
    )
    FetchContent_MakeAvailable(eui_neo_skia)
    FetchContent_GetProperties(eui_neo_skia)
    set(RELAYDESK_EUI_NEO_SOURCE_DIR "${eui_neo_skia_SOURCE_DIR}")
else()
    FetchContent_Declare(eui_neo
        GIT_REPOSITORY "${RELAYDESK_EUI_NEO_GIT_REPOSITORY}"
        GIT_TAG "${RELAYDESK_EUI_NEO_GIT_TAG}"
        PATCH_COMMAND
            ${CMAKE_COMMAND}
             "-DRELAYDESK_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
             "-DRELAYDESK_BUILD_DIR=${CMAKE_BINARY_DIR}"
             "-DEUI_NEO_SOURCE_DIR=<SOURCE_DIR>"
             -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchEuiNeoAppShortcuts.cmake"
    )
    FetchContent_MakeAvailable(eui_neo)
    FetchContent_GetProperties(eui_neo)
    set(RELAYDESK_EUI_NEO_SOURCE_DIR "${eui_neo_SOURCE_DIR}")
endif()

set(RELAYDESK_BUILD_DIR "${CMAKE_BINARY_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchEuiNeoAppShortcuts.cmake")
relaydesk_patch_eui_neo_app_shortcuts("${RELAYDESK_EUI_NEO_SOURCE_DIR}")

if(TARGET eui_neo)
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
    message(WARNING "EUI-NEO target was not created; relaydesk UI target will be skipped.")
endif()

if(RELAYDESK_HAS_EUI_NEO)
    file(GLOB RELAYDESK_VSCODE_ICON_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/third_party/vscode-icons/icons/*.svg")
    list(SORT RELAYDESK_VSCODE_ICON_FILES)
    set(RELAYDESK_VSCODE_ICON_RESOURCE_SCRIPT
        "${CMAKE_CURRENT_BINARY_DIR}/relaydesk_vscode_icon_resources.rc")
    file(WRITE "${RELAYDESK_VSCODE_ICON_RESOURCE_SCRIPT}"
        "// Generated by CMake. Do not edit.\n")
    foreach(RELAYDESK_VSCODE_ICON_FILE IN LISTS RELAYDESK_VSCODE_ICON_FILES)
        get_filename_component(RELAYDESK_VSCODE_ICON_NAME
            "${RELAYDESK_VSCODE_ICON_FILE}" NAME)
        string(MAKE_C_IDENTIFIER "${RELAYDESK_VSCODE_ICON_NAME}"
            RELAYDESK_VSCODE_ICON_RESOURCE_NAME)
        string(TOUPPER "${RELAYDESK_VSCODE_ICON_RESOURCE_NAME}"
            RELAYDESK_VSCODE_ICON_RESOURCE_NAME)
        file(APPEND "${RELAYDESK_VSCODE_ICON_RESOURCE_SCRIPT}"
            "VSICON_${RELAYDESK_VSCODE_ICON_RESOURCE_NAME} RCDATA "
            "\"${RELAYDESK_VSCODE_ICON_FILE}\"\n")
    endforeach()
    foreach(RELAYDESK_VSCODE_NOTICE_FILE IN ITEMS LICENSE NOTICE.md)
        set(RELAYDESK_VSCODE_NOTICE_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/third_party/vscode-icons/${RELAYDESK_VSCODE_NOTICE_FILE}")
        if(EXISTS "${RELAYDESK_VSCODE_NOTICE_PATH}")
            string(MAKE_C_IDENTIFIER "vscode_icons_${RELAYDESK_VSCODE_NOTICE_FILE}"
                RELAYDESK_VSCODE_NOTICE_RESOURCE_NAME)
            string(TOUPPER "${RELAYDESK_VSCODE_NOTICE_RESOURCE_NAME}"
                RELAYDESK_VSCODE_NOTICE_RESOURCE_NAME)
            file(APPEND "${RELAYDESK_VSCODE_ICON_RESOURCE_SCRIPT}"
                "VSICON_${RELAYDESK_VSCODE_NOTICE_RESOURCE_NAME} RCDATA "
                "\"${RELAYDESK_VSCODE_NOTICE_PATH}\"\n")
        endif()
    endforeach()

    set(RELAYDESK_EUI_MAIN_WRAPPER
        "${CMAKE_CURRENT_BINARY_DIR}/relaydesk_eui_glfw_app_main.cpp")
    file(READ "${RELAYDESK_EUI_NEO_SOURCE_DIR}/core/app/glfw_app_main.cpp"
        RELAYDESK_EUI_MAIN_SOURCE)
    string(REPLACE "int main()" "int relaydesk_eui_main()"
        RELAYDESK_EUI_MAIN_SOURCE "${RELAYDESK_EUI_MAIN_SOURCE}")
    file(WRITE "${RELAYDESK_EUI_MAIN_WRAPPER}"
        "${RELAYDESK_EUI_MAIN_SOURCE}")

    add_executable(relaydesk
        "${RELAYDESK_EUI_MAIN_WRAPPER}"
        src/main/relaydesk_entry.cpp
        src/main/app.cpp
        src/main/image_attachment_store.cpp
        src/main/app_runtime.cpp
        src/core/app_version.cpp
        src/core/diagnostic_log.cpp
        src/core/time.cpp
        src/core/uuid.cpp
        src/platform/attachment_input_win.cpp
        src/platform/computer_name_win.cpp
        src/platform/startup_launch_state.cpp
        src/platform/startup_launch_win.cpp
        src/platform/text_encoding_win.cpp
        src/platform/windows_install_id_win.cpp
        src/net/discovery_local_announcement.cpp
        src/net/discovery_message.cpp
        src/net/discovery_peer_profile.cpp
        src/net/discovery_processor.cpp
        src/net/peer_frame.cpp
        src/net/peer_message.cpp
        src/storage/app_paths.cpp
        src/storage/history_store.cpp
        src/storage/local_identity.cpp
        src/storage/peer_profile.cpp
        src/storage/sticker_store.cpp
        src/storage/ui_preferences.cpp
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
            ${RELAYDESK_VSCODE_ICON_RESOURCE_SCRIPT}
        )
        target_link_libraries(relaydesk PRIVATE
            shell32
            windowscodecs
            ole32
            advapi32
            bcrypt
            dwmapi
            winmm
        )
    else()
        message(FATAL_ERROR "RelayDesk currently targets Windows only.")
    endif()

    if(MSVC)
        target_compile_options(relaydesk PRIVATE /W4 /permissive- /utf-8)
        # MSVC 14.51 can hit internal compiler errors while optimizing these
        # large UI/runtime translation units in Release builds.
        set_source_files_properties(src/main/app.cpp src/main/app_runtime.cpp PROPERTIES
            COMPILE_OPTIONS "$<$<CONFIG:Release>:/Od>;$<$<CONFIG:Release>:/Ob0>")
    endif()

    if(RELAYDESK_HAS_BOOST_ASIO)
        target_sources(relaydesk PRIVATE
            src/net/boost_asio_tcp_peer_transport.cpp
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
            "$<TARGET_FILE_DIR:relaydesk>/assets/icon.png"
            "$<TARGET_FILE_DIR:relaydesk>/assets/icon.ico"
            "$<TARGET_FILE_DIR:relaydesk>/assets/Font Awesome 7 Free-Solid-900.otf"
            "$<TARGET_FILE_DIR:relaydesk>/assets/JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf"
            "$<TARGET_FILE_DIR:relaydesk>/assets/YouSheBiaoTiHei-2.ttf"
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:relaydesk>/assets/third_party"
    )
endif()

if(RELAYDESK_BUILD_SKIAUI)
    find_package(SkiaUI CONFIG REQUIRED)

    set(RELAYDESK_COMMON_RUNTIME_SOURCES
        src/main/app_runtime.cpp
        src/main/image_attachment_store.cpp
        src/main/skiaui_background.cpp
        src/core/app_version.cpp
        src/core/diagnostic_log.cpp
        src/core/platform/async.cpp
        src/core/time.cpp
        src/core/uuid.cpp
        src/platform/attachment_input_win.cpp
        src/platform/computer_name_win.cpp
        src/platform/crash_report_win.cpp
        src/platform/crash_reporter_win.cpp
        src/platform/crash_uploader_win.cpp
        src/platform/startup_launch_state.cpp
        src/platform/startup_launch_win.cpp
        src/platform/text_encoding_win.cpp
        src/platform/windows_install_id_win.cpp
        src/net/discovery_local_announcement.cpp
        src/net/discovery_message.cpp
        src/net/discovery_peer_profile.cpp
        src/net/discovery_processor.cpp
        src/net/peer_frame.cpp
        src/net/peer_message.cpp
        src/storage/app_paths.cpp
        src/storage/history_store.cpp
        src/storage/local_identity.cpp
        src/storage/peer_profile.cpp
        src/storage/ui_preferences.cpp
        src/storage/app_paths_win.cpp
    )

    add_executable(relaydesk_skiaui WIN32
        src/main/skiaui_app.cpp
        ${RELAYDESK_COMMON_RUNTIME_SOURCES}
    )

    target_include_directories(relaydesk_skiaui PRIVATE src)
    target_compile_features(relaydesk_skiaui PRIVATE cxx_std_23)
    target_link_libraries(relaydesk_skiaui PRIVATE
        nlohmann_json::nlohmann_json
        SkiaUI::SkuiWin32Dx12
    )

    # 分发版本必须保留调试信息：崩溃报告里的偏移量只有在能映射回函数名和行号时
    # 才有价值。这里同时生成 PDB 并记录程序库信息，供符号归档使用。
    if(MSVC)
        target_compile_options(relaydesk_skiaui PRIVATE
            "$<$<CONFIG:Release>:/Zi>"
            "$<$<CONFIG:RelWithDebInfo>:/Zi>"
        )
        target_link_options(relaydesk_skiaui PRIVATE
            "$<$<CONFIG:Release>:/DEBUG:FULL>"
            "$<$<CONFIG:Release>:/OPT:REF>"
            "$<$<CONFIG:Release>:/OPT:ICF>"
            "$<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>"
            "$<$<CONFIG:RelWithDebInfo>:/OPT:REF>"
            "$<$<CONFIG:RelWithDebInfo>:/OPT:ICF>"
        )
    endif()

    if(WIN32)
        set(RELAYDESK_SKIAUI_ASSETS_RC
            ${CMAKE_SOURCE_DIR}/relaydesk_skiaui_assets.rc)
        set(RELAYDESK_SKIAUI_EMBEDDED_ASSETS
            ${CMAKE_SOURCE_DIR}/assets/skui_relay_demo/relaydesk.html
            ${CMAKE_SOURCE_DIR}/assets/skui_relay_demo/icons/file_type_pdf.svg
            ${CMAKE_SOURCE_DIR}/assets/skui_relay_demo/icons/file_type_zip.svg
        )
        set_source_files_properties(${RELAYDESK_SKIAUI_ASSETS_RC}
            PROPERTIES OBJECT_DEPENDS "${RELAYDESK_SKIAUI_EMBEDDED_ASSETS}")

        target_compile_definitions(relaydesk_skiaui PRIVATE
            "RELAYDESK_TEST_SANDBOX_PARENT=\"${CMAKE_SOURCE_DIR}/tmp/tests\""
            NOMINMAX
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
        )
        target_sources(relaydesk_skiaui PRIVATE
            ${CMAKE_SOURCE_DIR}/relaydesk.rc
            ${RELAYDESK_SKIAUI_ASSETS_RC}
        )
        target_link_libraries(relaydesk_skiaui PRIVATE
            advapi32
            bcrypt
            comdlg32
            dbghelp
            dwmapi
            psapi
            winhttp
            shell32
            winmm
        )
    else()
        message(FATAL_ERROR "RelayDesk SkiaUI currently targets Windows only.")
    endif()

    if(MSVC)
        target_compile_options(relaydesk_skiaui PRIVATE
            /W4
            /permissive-
            /utf-8
            "$<$<CONFIG:Release>:/O1>"
        )
        # VS 2026 MSVC 14.51 在大型运行时翻译单元上可能因激进内联触发 C1001。
        # 保留 /O1 优化，仅关闭该文件级内联，避免牺牲整体 Release 优化。
        set_source_files_properties(
            src/main/app_runtime.cpp
            src/platform/startup_launch_state.cpp
            src/platform/startup_launch_win.cpp
            PROPERTIES
            COMPILE_OPTIONS "$<$<CONFIG:Release>:/Ob0>")
    endif()

    if(RELAYDESK_HAS_BOOST_ASIO)
        target_sources(relaydesk_skiaui PRIVATE
            src/net/boost_asio_tcp_peer_transport.cpp
            src/net/boost_asio_udp_discovery_transport.cpp
            src/net/discovery_service.cpp
            src/net/discovery_worker.cpp
        )
        target_compile_definitions(relaydesk_skiaui PRIVATE RELAYDESK_HAS_BOOST_ASIO)
        target_link_libraries(relaydesk_skiaui PRIVATE relaydesk_boost_asio)
    endif()

    # 崩溃报告要说明现场来自哪一次构建。配置名和构建时间由 CMake 注入，
    # 版本号由 src/core/app_version.cpp 维护。
    string(TIMESTAMP RELAYDESK_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)
    target_compile_definitions(relaydesk_skiaui PRIVATE
        "RELAYDESK_BUILD_CONFIGURATION=\"$<CONFIG>\""
        "RELAYDESK_BUILD_TIMESTAMP=\"${RELAYDESK_BUILD_TIMESTAMP}\""
    )
endif()

# 符号归档：把每次发布产出的 exe 与 pdb 按版本集中存放。崩溃报告不能自带符号
# （pdb 有几十 MB），所以排查时依赖这份归档把偏移量还原成函数名与行号。
function(relaydesk_archive_symbols target)
    if(NOT MSVC)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DSYMBOL_TARGET_FILE=$<TARGET_FILE:${target}>"
            "-DSYMBOL_TARGET_PDB=$<TARGET_PDB_FILE:${target}>"
            "-DSYMBOL_CONFIG=$<CONFIG>"
            "-DSYMBOL_ARCHIVE_DIR=${CMAKE_SOURCE_DIR}/out/symbols"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArchiveSymbols.cmake"
        COMMENT "Archiving symbols for ${target}"
        VERBATIM
    )
endfunction()

if(RELAYDESK_BUILD_SKIAUI AND TARGET relaydesk_skiaui)
    relaydesk_archive_symbols(relaydesk_skiaui)
endif()

enable_testing()

if(RELAYDESK_BUILD_SKIAUI)
    add_test(NAME relaydesk_skiaui_render_to_bgra_pixels_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_capture.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-peer-switch
    )
    add_test(NAME relaydesk_skiaui_image_message_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_image_message.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-image-message
    )
    add_test(NAME relaydesk_skiaui_file_card_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_file_card.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-file-card
    )
    add_test(NAME relaydesk_skiaui_multi_file_message_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_multi_file_message.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-multi-file-message
    )
    add_test(NAME relaydesk_skiaui_composer_attachments_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_composer_attachments.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-composer-attachments
    )
    add_test(NAME relaydesk_skiaui_composer_keyboard_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_composer_keyboard.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-composer-keyboard
    )
    add_test(NAME relaydesk_skiaui_history_pagination_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_history_pagination.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-history-pagination
    )
    add_test(NAME relaydesk_skiaui_unread_badge_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_unread_badge.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-unread-badge
    )
    add_test(NAME relaydesk_skiaui_app_update_available_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_app_update_available.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-app-update-available
    )
    add_test(NAME relaydesk_skiaui_app_update_downloading_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_app_update_downloading.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-app-update-downloading
    )
    add_test(NAME relaydesk_skiaui_app_update_failed_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_app_update_failed.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-app-update-failed
    )
    add_test(NAME relaydesk_skiaui_settings_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_settings.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-settings
    )
    add_test(NAME relaydesk_skiaui_settings_dark_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_settings_dark.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-settings-dark
    )
    add_test(NAME relaydesk_skiaui_dark_chat_tabs_capture
        COMMAND relaydesk_skiaui
            --capture-skiaui
            "${CMAKE_BINARY_DIR}/tmp/relaydesk_skiaui_dark_chat_tabs.png"
            --capture-width 1600
            --capture-height 900
            --capture-dpi-scale 1.0
            --capture-test-dark-chat-tabs
    )

    set(RELAYDESK_SKIAUI_CAPTURE_TESTS
        relaydesk_skiaui_render_to_bgra_pixels_capture
        relaydesk_skiaui_image_message_capture
        relaydesk_skiaui_file_card_capture
        relaydesk_skiaui_multi_file_message_capture
        relaydesk_skiaui_composer_attachments_capture
        relaydesk_skiaui_composer_keyboard_capture
        relaydesk_skiaui_history_pagination_capture
        relaydesk_skiaui_unread_badge_capture
        relaydesk_skiaui_app_update_available_capture
        relaydesk_skiaui_app_update_downloading_capture
        relaydesk_skiaui_app_update_failed_capture
        relaydesk_skiaui_settings_capture
        relaydesk_skiaui_settings_dark_capture
        relaydesk_skiaui_dark_chat_tabs_capture
    )
    foreach(capture_test IN LISTS RELAYDESK_SKIAUI_CAPTURE_TESTS)
        set(capture_test_root
            "${RELAYDESK_TEST_SANDBOX_PARENT}/skiaui_capture/${capture_test}")
        set(capture_test_environment
            "RELAYDESK_TEST_MODE=1"
            "RELAYDESK_TEST_ROOT=${capture_test_root}"
            "RELAYDESK_TEST_DATA_DIRECTORY=${capture_test_root}/data")
        set_tests_properties(${capture_test} PROPERTIES
            ENVIRONMENT "${capture_test_environment}"
        )
    endforeach()
endif()

add_test(NAME relaydesk_patch_eui_neo_app_shortcuts_tests
    COMMAND ${CMAKE_COMMAND}
        "-DRELAYDESK_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DRELAYDESK_PATCH_TEST_WORK_DIR=${CMAKE_SOURCE_DIR}/tmp/tests/patch_eui_neo_app_shortcuts"
        -P "${CMAKE_SOURCE_DIR}/tests/cmake/patch_eui_neo_app_shortcuts_tests.cmake"
)

add_executable(relaydesk_storage_tests
    tests/storage/app_paths_tests.cpp
    src/storage/app_paths.cpp
)

if(WIN32)
    target_sources(relaydesk_storage_tests PRIVATE
        src/storage/app_paths_win.cpp)
    target_compile_definitions(relaydesk_storage_tests PRIVATE
        "RELAYDESK_TEST_SANDBOX_PARENT=\"${RELAYDESK_TEST_SANDBOX_PARENT}\"")
endif()

target_include_directories(relaydesk_storage_tests PRIVATE src)
target_compile_features(relaydesk_storage_tests PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(relaydesk_storage_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_storage_tests COMMAND relaydesk_storage_tests)

if(WIN32)
    add_executable(relaydesk_image_attachment_store_tests
        tests/main/image_attachment_store_tests.cpp
        src/main/image_attachment_store.cpp
        src/core/uuid.cpp
        src/platform/attachment_input_win.cpp
        src/platform/text_encoding_win.cpp
        src/storage/app_paths.cpp
        src/storage/app_paths_win.cpp
    )

    target_include_directories(relaydesk_image_attachment_store_tests PRIVATE src)
    target_compile_features(relaydesk_image_attachment_store_tests PRIVATE cxx_std_23)

    set(RELAYDESK_IMAGE_TEST_WORK_DIR
        "${CMAKE_SOURCE_DIR}/tmp/tests/image_attachment_store")
    set(RELAYDESK_IMAGE_TEST_WORK_DIR_DEFINE
        "RELAYDESK_IMAGE_ATTACHMENT_STORE_TEST_WORK_DIR=\"${RELAYDESK_IMAGE_TEST_WORK_DIR}\"")

    target_compile_definitions(relaydesk_image_attachment_store_tests PRIVATE
        ${RELAYDESK_IMAGE_TEST_WORK_DIR_DEFINE}
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
    )

    target_link_libraries(relaydesk_image_attachment_store_tests PRIVATE
        shell32
        windowscodecs
        ole32
        bcrypt
    )

    if(MSVC)
        target_compile_options(relaydesk_image_attachment_store_tests
            PRIVATE /W4 /permissive- /utf-8)
    endif()

    add_test(NAME relaydesk_image_attachment_store_tests
        COMMAND relaydesk_image_attachment_store_tests)
endif()

if(WIN32 AND RELAYDESK_HAS_EUI_NEO AND RELAYDESK_HAS_BOOST_ASIO)
    set(RELAYDESK_APP_RUNTIME_PROCESS_TEST_WORK_DIR
        "${CMAKE_SOURCE_DIR}/tmp/tests/app_runtime_process")

    add_executable(relaydesk_app_runtime_update_prompt_tests
        tests/main/app_runtime_update_prompt_tests.cpp
        src/main/app_runtime.cpp
        src/main/image_attachment_store.cpp
        src/core/app_version.cpp
        src/core/diagnostic_log.cpp
        src/core/time.cpp
        src/core/uuid.cpp
        src/platform/attachment_input_win.cpp
        src/platform/computer_name_win.cpp
        src/platform/text_encoding_win.cpp
        src/platform/windows_install_id_win.cpp
        src/net/boost_asio_tcp_peer_transport.cpp
        src/net/boost_asio_udp_discovery_transport.cpp
        src/net/discovery_local_announcement.cpp
        src/net/discovery_message.cpp
        src/net/discovery_peer_profile.cpp
        src/net/discovery_processor.cpp
        src/net/discovery_service.cpp
        src/net/discovery_worker.cpp
        src/net/peer_frame.cpp
        src/net/peer_message.cpp
        src/storage/app_paths.cpp
        src/storage/app_paths_win.cpp
        src/storage/history_store.cpp
        src/storage/local_identity.cpp
        src/storage/peer_profile.cpp
        src/storage/sticker_store.cpp
        src/storage/ui_preferences.cpp
    )

    set_target_properties(relaydesk_app_runtime_update_prompt_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY
            "${RELAYDESK_APP_RUNTIME_PROCESS_TEST_WORK_DIR}/runner")

    target_include_directories(relaydesk_app_runtime_update_prompt_tests
        PRIVATE src)
    target_compile_features(relaydesk_app_runtime_update_prompt_tests
        PRIVATE cxx_std_23)
    target_compile_definitions(relaydesk_app_runtime_update_prompt_tests PRIVATE
        "RELAYDESK_APP_RUNTIME_PROCESS_TEST_WORK_DIR=\"${RELAYDESK_APP_RUNTIME_PROCESS_TEST_WORK_DIR}\""
        "RELAYDESK_TEST_SANDBOX_PARENT=\"${RELAYDESK_TEST_SANDBOX_PARENT}\""
        RELAYDESK_HAS_BOOST_ASIO
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
    )

    if(TARGET relaydesk_skiaui)
        add_dependencies(relaydesk_app_runtime_update_prompt_tests
            relaydesk_skiaui)
        target_compile_definitions(
            relaydesk_app_runtime_update_prompt_tests PRIVATE
            "RELAYDESK_SKIAUI_EXECUTABLE_PATH=\"$<TARGET_FILE:relaydesk_skiaui>\"")
    endif()

    target_link_libraries(relaydesk_app_runtime_update_prompt_tests PRIVATE
        eui::neo
        relaydesk_boost_asio
        nlohmann_json::nlohmann_json
        shell32
        windowscodecs
        ole32
        bcrypt
    )

    if(MSVC)
        target_compile_options(relaydesk_app_runtime_update_prompt_tests
            PRIVATE /W4 /permissive- /utf-8)
    endif()

    add_test(NAME relaydesk_app_runtime_update_prompt_tests
        COMMAND relaydesk_app_runtime_update_prompt_tests)
    set(app_runtime_test_environment
        "RELAYDESK_TEST_MODE=1"
        "RELAYDESK_TEST_ROOT=${RELAYDESK_APP_RUNTIME_PROCESS_TEST_WORK_DIR}")
    set_tests_properties(relaydesk_app_runtime_update_prompt_tests PROPERTIES
        ENVIRONMENT "${app_runtime_test_environment}"
    )
endif()

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

add_executable(relaydesk_ui_preferences_tests
    tests/storage/ui_preferences_tests.cpp
    src/storage/app_paths.cpp
    src/storage/ui_preferences.cpp
)

target_include_directories(relaydesk_ui_preferences_tests PRIVATE src)
target_compile_features(relaydesk_ui_preferences_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_ui_preferences_tests PRIVATE nlohmann_json::nlohmann_json)

target_compile_definitions(relaydesk_ui_preferences_tests PRIVATE
    RELAYDESK_TEST_WORK_DIR="${CMAKE_SOURCE_DIR}/tmp/tests/ui_preferences"
)

if(MSVC)
    target_compile_options(relaydesk_ui_preferences_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_ui_preferences_tests COMMAND relaydesk_ui_preferences_tests)

add_executable(relaydesk_sticker_store_tests
    tests/storage/sticker_store_tests.cpp
    src/storage/app_paths.cpp
    src/storage/sticker_store.cpp
)

target_include_directories(relaydesk_sticker_store_tests PRIVATE src)
target_compile_features(relaydesk_sticker_store_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_sticker_store_tests PRIVATE nlohmann_json::nlohmann_json)

target_compile_definitions(relaydesk_sticker_store_tests PRIVATE
    RELAYDESK_STICKER_TEST_WORK_DIR="${CMAKE_SOURCE_DIR}/tmp/tests/sticker_store"
)

if(MSVC)
    target_compile_options(relaydesk_sticker_store_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_sticker_store_tests COMMAND relaydesk_sticker_store_tests)

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

add_executable(relaydesk_startup_launch_state_tests
    tests/platform/startup_launch_state_tests.cpp
    src/platform/startup_launch_state.cpp
)

target_include_directories(relaydesk_startup_launch_state_tests PRIVATE src)
target_compile_features(relaydesk_startup_launch_state_tests PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(relaydesk_startup_launch_state_tests
        PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_startup_launch_state_tests
    COMMAND relaydesk_startup_launch_state_tests)

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

add_executable(relaydesk_peer_frame_tests
    tests/net/peer_frame_tests.cpp
    src/net/peer_frame.cpp
)

target_include_directories(relaydesk_peer_frame_tests PRIVATE src)
target_compile_features(relaydesk_peer_frame_tests PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(relaydesk_peer_frame_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_peer_frame_tests COMMAND relaydesk_peer_frame_tests)

add_executable(relaydesk_peer_message_tests
    tests/net/peer_message_tests.cpp
    src/net/peer_frame.cpp
    src/net/peer_message.cpp
    src/storage/history_store.cpp
)

target_include_directories(relaydesk_peer_message_tests PRIVATE src)
target_compile_features(relaydesk_peer_message_tests PRIVATE cxx_std_23)
target_link_libraries(relaydesk_peer_message_tests PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(relaydesk_peer_message_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME relaydesk_peer_message_tests COMMAND relaydesk_peer_message_tests)

add_executable(relaydesk_discovery_local_announcement_tests
    tests/net/discovery_local_announcement_tests.cpp
    src/core/app_version.cpp
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

    add_executable(relaydesk_boost_asio_tcp_peer_transport_tests
        tests/net/boost_asio_tcp_peer_transport_tests.cpp
        src/net/boost_asio_tcp_peer_transport.cpp
        src/net/peer_frame.cpp
        src/net/peer_message.cpp
        src/storage/history_store.cpp
    )

    target_include_directories(relaydesk_boost_asio_tcp_peer_transport_tests
        PRIVATE src)
    target_compile_features(relaydesk_boost_asio_tcp_peer_transport_tests
        PRIVATE cxx_std_23)
    target_link_libraries(relaydesk_boost_asio_tcp_peer_transport_tests
        PRIVATE relaydesk_boost_asio nlohmann_json::nlohmann_json)

    if(MSVC)
        target_compile_options(relaydesk_boost_asio_tcp_peer_transport_tests
            PRIVATE /W4 /permissive- /utf-8)
    endif()

    add_test(NAME relaydesk_boost_asio_tcp_peer_transport_tests
             COMMAND relaydesk_boost_asio_tcp_peer_transport_tests)

    add_executable(relaydesk_discovery_service_tests
        tests/net/discovery_service_tests.cpp
        src/core/app_version.cpp
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

    add_executable(relaydesk_discovery_app_version_transmission_tests
        tests/net/discovery_app_version_transmission_tests.cpp
        src/core/app_version.cpp
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

    target_include_directories(relaydesk_discovery_app_version_transmission_tests
        PRIVATE src)
    target_compile_features(relaydesk_discovery_app_version_transmission_tests
        PRIVATE cxx_std_23)
    target_link_libraries(relaydesk_discovery_app_version_transmission_tests
        PRIVATE relaydesk_boost_asio nlohmann_json::nlohmann_json)

    set(RELAYDESK_DISCOVERY_APP_VERSION_TEST_PATH
        "${CMAKE_SOURCE_DIR}/tmp/tests/discovery_app_version")

    target_compile_definitions(
        relaydesk_discovery_app_version_transmission_tests PRIVATE
        "RELAYDESK_DISCOVERY_APP_VERSION_TEST_WORK_DIR=\"${RELAYDESK_DISCOVERY_APP_VERSION_TEST_PATH}\""
    )

    if(MSVC)
        target_compile_options(
            relaydesk_discovery_app_version_transmission_tests
            PRIVATE /W4 /permissive- /utf-8)
    endif()

    add_test(NAME relaydesk_discovery_app_version_transmission_tests
             COMMAND relaydesk_discovery_app_version_transmission_tests)

    add_executable(relaydesk_discovery_worker_tests
        tests/net/discovery_worker_tests.cpp
        src/core/app_version.cpp
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
        src/core/app_version.cpp
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
