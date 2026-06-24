function(relaydesk_patch_eui_neo_app_shortcuts eui_source_dir)
    cmake_path(ABSOLUTE_PATH eui_source_dir
               NORMALIZE
               OUTPUT_VARIABLE eui_patch_source_dir)
    if(DEFINED RELAYDESK_BUILD_DIR)
        set(eui_allowed_fetchcontent_dir "${RELAYDESK_BUILD_DIR}/_deps")
        cmake_path(ABSOLUTE_PATH eui_allowed_fetchcontent_dir
                   NORMALIZE
                   OUTPUT_VARIABLE eui_allowed_fetchcontent_dir)
        cmake_path(IS_PREFIX eui_allowed_fetchcontent_dir
                   "${eui_patch_source_dir}"
                   NORMALIZE
                   eui_source_is_fetchcontent)
        if(NOT eui_source_is_fetchcontent)
            message(FATAL_ERROR "Refusing to patch EUI-NEO outside the FetchContent build tree")
        endif()
    endif()

    set(eui_patched_app_shortcuts OFF)
    set(eui_glfw_app_source "${eui_source_dir}/core/app/glfw_app_main.cpp")
    if(EXISTS "${eui_glfw_app_source}")
        file(READ "${eui_glfw_app_source}" eui_glfw_app_content)
        string(CONCAT eui_glfw_include_block
            "#include <chrono>\n"
            "#include <cstdio>\n"
            "#include <memory>\n"
            "#include <thread>\n"
            "#include <vector>\n"
        )
        string(CONCAT eui_glfw_single_instance_include_block
            "#include <chrono>\n"
            "#include <cstdio>\n"
            "#include <iterator>\n"
            "#include <memory>\n"
            "#include <string>\n"
            "#include <thread>\n"
            "#include <vector>\n"
        )
        string(FIND "${eui_glfw_app_content}"
                    "${eui_glfw_single_instance_include_block}"
                    eui_glfw_single_instance_include_pos)
        if(eui_glfw_single_instance_include_pos LESS 0)
            string(FIND "${eui_glfw_app_content}"
                        "${eui_glfw_include_block}"
                        eui_glfw_include_pos)
            if(eui_glfw_include_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_glfw_include_block}"
                               "${eui_glfw_single_instance_include_block}"
                               eui_glfw_app_content
                               "${eui_glfw_app_content}")
                set(eui_patched_app_shortcuts ON)
            endif()
        endif()

        string(CONCAT eui_glfw_timer_then_dpi_block
            "struct TimerResolutionGuard {\n"
            "    TimerResolutionGuard() {\n"
            "#ifdef _WIN32\n"
            "        timeBeginPeriod(1);\n"
            "#endif\n"
            "    }\n"
            "\n"
            "    ~TimerResolutionGuard() {\n"
            "#ifdef _WIN32\n"
            "        timeEndPeriod(1);\n"
            "#endif\n"
            "    }\n"
            "};\n"
            "\n"
            "float getDpiScale(GLFWwindow* window) {\n"
        )
        string(CONCAT eui_glfw_single_instance_block
            "struct TimerResolutionGuard {\n"
            "    TimerResolutionGuard() {\n"
            "#ifdef _WIN32\n"
            "        timeBeginPeriod(1);\n"
            "#endif\n"
            "    }\n"
            "\n"
            "    ~TimerResolutionGuard() {\n"
            "#ifdef _WIN32\n"
            "        timeEndPeriod(1);\n"
            "#endif\n"
            "    }\n"
            "};\n"
            "\n"
            "#ifdef _WIN32\n"
            "constexpr UINT kRelayDeskTrayShowCommand = 1000;\n"
            "\n"
            "struct SingleInstanceGuard {\n"
            "    HANDLE mutex = nullptr;\n"
            "    bool alreadyRunning = false;\n"
            "\n"
            "    SingleInstanceGuard() {\n"
            "        mutex = CreateMutexW(nullptr, TRUE, L\"Local\\\\RelayDesk.SingleInstance\");\n"
            "        alreadyRunning = mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;\n"
            "    }\n"
            "\n"
            "    ~SingleInstanceGuard() {\n"
            "        if (mutex != nullptr) {\n"
            "            CloseHandle(mutex);\n"
            "        }\n"
            "    }\n"
            "};\n"
            "\n"
            "BOOL CALLBACK requestExistingRelayDeskWindow(HWND window, LPARAM) {\n"
            "    wchar_t className[64]{};\n"
            "    const int classLength =\n"
            "        GetClassNameW(window, className, static_cast<int>(std::size(className)));\n"
            "    if (classLength > 0 && std::wstring(className) == L\"TRAY\") {\n"
            "        SendMessageW(window, WM_COMMAND, kRelayDeskTrayShowCommand, 0);\n"
            "        return FALSE;\n"
            "    }\n"
            "\n"
            "    wchar_t title[128]{};\n"
            "    const int titleLength =\n"
            "        GetWindowTextW(window, title, static_cast<int>(std::size(title)));\n"
            "    if (titleLength > 0 && std::wstring(title) == L\"RelayDesk\") {\n"
            "        ShowWindow(window, SW_RESTORE);\n"
            "        SetForegroundWindow(window);\n"
            "        return FALSE;\n"
            "    }\n"
            "    return TRUE;\n"
            "}\n"
            "\n"
            "void activateExistingRelayDeskInstance() {\n"
            "    EnumWindows(requestExistingRelayDeskWindow, 0);\n"
            "}\n"
            "#endif\n"
            "\n"
            "float getDpiScale(GLFWwindow* window) {\n"
        )
        string(FIND "${eui_glfw_app_content}"
                    "SingleInstanceGuard"
                    eui_glfw_single_instance_pos)
        if(eui_glfw_single_instance_pos LESS 0)
            string(FIND "${eui_glfw_app_content}"
                        "${eui_glfw_timer_then_dpi_block}"
                        eui_glfw_timer_then_dpi_pos)
            if(eui_glfw_timer_then_dpi_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_glfw_timer_then_dpi_block}"
                               "${eui_glfw_single_instance_block}"
                               eui_glfw_app_content
                               "${eui_glfw_app_content}")
                set(eui_patched_app_shortcuts ON)
            endif()
        endif()

        string(CONCAT eui_glfw_main_block
            "int main() {\n"
            "    core::render::initializeRenderBackendLoader();\n"
        )
        string(CONCAT eui_glfw_single_instance_main_block
            "int main() {\n"
            "#ifdef _WIN32\n"
            "    SingleInstanceGuard singleInstance;\n"
            "    if (singleInstance.alreadyRunning) {\n"
            "        activateExistingRelayDeskInstance();\n"
            "        return 0;\n"
            "    }\n"
            "#endif\n"
            "\n"
            "    core::render::initializeRenderBackendLoader();\n"
        )
        string(FIND "${eui_glfw_app_content}"
                    "singleInstance.alreadyRunning"
                    eui_glfw_single_instance_main_pos)
        if(eui_glfw_single_instance_main_pos LESS 0)
            string(FIND "${eui_glfw_app_content}"
                        "${eui_glfw_main_block}"
                        eui_glfw_main_pos)
            if(eui_glfw_main_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_glfw_main_block}"
                               "${eui_glfw_single_instance_main_block}"
                               eui_glfw_app_content
                               "${eui_glfw_app_content}")
                set(eui_patched_app_shortcuts ON)
            endif()
        endif()

        string(CONCAT eui_glfw_escape_close_block
            "        if (windowState.modalChildWindow == nullptr"
            " && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {\n"
            "            if (windowState.trayAvailable) {\n"
            "                windowState.hideToTrayRequested = true;\n"
            "            } else {\n"
            "                glfwSetWindowShouldClose(window, 1);\n"
            "                break;\n"
            "            }\n"
            "        }\n"
            "\n"
        )
        string(FIND "${eui_glfw_app_content}"
                    "${eui_glfw_escape_close_block}"
                    eui_glfw_escape_close_pos)
        if(eui_glfw_escape_close_pos GREATER_EQUAL 0)
            string(REPLACE "${eui_glfw_escape_close_block}"
                           ""
                           eui_glfw_app_content
                           "${eui_glfw_app_content}")
            file(WRITE "${eui_glfw_app_source}" "${eui_glfw_app_content}")
            set(eui_patched_app_shortcuts ON)
        endif()
    endif()

    set(eui_sdl_app_source "${eui_source_dir}/core/app/sdl2_app_main.cpp")
    if(EXISTS "${eui_sdl_app_source}")
        file(READ "${eui_sdl_app_source}" eui_sdl_app_content)
        string(CONCAT eui_sdl_escape_close_block
            "        if (event.key.keysym.sym == SDLK_ESCAPE"
            " && !state.trayAvailable) {\n"
            "            state.running = false;\n"
            "        } else if (event.key.keysym.sym == SDLK_ESCAPE) {\n"
            "            hideToTray(window, state);\n"
            "        }\n"
        )
        string(FIND "${eui_sdl_app_content}"
                    "${eui_sdl_escape_close_block}"
                    eui_sdl_escape_close_pos)
        if(eui_sdl_escape_close_pos GREATER_EQUAL 0)
            string(REPLACE "${eui_sdl_escape_close_block}"
                           ""
                           eui_sdl_app_content
                           "${eui_sdl_app_content}")
            file(WRITE "${eui_sdl_app_source}" "${eui_sdl_app_content}")
            set(eui_patched_app_shortcuts ON)
        endif()
    endif()

    set(eui_tray_source "${eui_source_dir}/3rd/tray/tray.h")
    if(EXISTS "${eui_tray_source}")
        file(READ "${eui_tray_source}" eui_tray_content)
        string(CONCAT eui_tray_popup_on_left_or_right_block
            "    if (lparam == WM_LBUTTONUP || lparam == WM_RBUTTONUP) {\n"
            "      POINT p;\n"
            "      GetCursorPos(&p);\n"
            "      SetForegroundWindow(hwnd);\n"
            "      WORD cmd = TrackPopupMenu(hmenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON |\n"
            "                                           TPM_RETURNCMD | TPM_NONOTIFY,\n"
            "                                p.x, p.y, 0, hwnd, NULL);\n"
            "      SendMessage(hwnd, WM_COMMAND, cmd, 0);\n"
            "      return 0;\n"
            "    }\n"
        )
        string(CONCAT eui_tray_left_show_right_popup_block
            "    if (lparam == WM_LBUTTONUP) {\n"
            "      SendMessage(hwnd, WM_COMMAND, ID_TRAY_FIRST, 0);\n"
            "      return 0;\n"
            "    }\n"
            "    if (lparam == WM_RBUTTONUP) {\n"
            "      POINT p;\n"
            "      GetCursorPos(&p);\n"
            "      SetForegroundWindow(hwnd);\n"
            "      WORD cmd = TrackPopupMenu(hmenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON |\n"
            "                                           TPM_RETURNCMD | TPM_NONOTIFY,\n"
            "                                p.x, p.y, 0, hwnd, NULL);\n"
            "      SendMessage(hwnd, WM_COMMAND, cmd, 0);\n"
            "      return 0;\n"
            "    }\n"
        )
        string(FIND "${eui_tray_content}"
                    "${eui_tray_popup_on_left_or_right_block}"
                    eui_tray_popup_pos)
        if(eui_tray_popup_pos GREATER_EQUAL 0)
            string(REPLACE "${eui_tray_popup_on_left_or_right_block}"
                           "${eui_tray_left_show_right_popup_block}"
                           eui_tray_content
                           "${eui_tray_content}")
            file(WRITE "${eui_tray_source}" "${eui_tray_content}")
            set(eui_patched_app_shortcuts ON)
        endif()
    endif()

    if(eui_patched_app_shortcuts)
        message(STATUS "Patched EUI-NEO app main to ignore Escape close shortcut")
    endif()
endfunction()

if(DEFINED EUI_NEO_SOURCE_DIR)
    relaydesk_patch_eui_neo_app_shortcuts("${EUI_NEO_SOURCE_DIR}")
endif()
