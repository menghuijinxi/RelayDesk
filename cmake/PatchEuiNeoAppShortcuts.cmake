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
    set(eui_runtime_lifecycle_source "${eui_source_dir}/core/runtime/runtime_lifecycle.h")
    if(EXISTS "${eui_runtime_lifecycle_source}")
        file(READ "${eui_runtime_lifecycle_source}" eui_runtime_lifecycle_content)
        string(CONCAT eui_focus_clear_on_interactive_block
            "    if (event.pressedThisFrame) {\n"
            "        setFocusedId(hitTestFocusable(event, dpiScale));\n"
            "    }\n"
            "\n"
            "    const std::string capturedId = capturedInteractionId();\n"
            "    const std::string hoverTargetId = !capturedId.empty() ? capturedId : hitTestInteractive(event, dpiScale);\n"
        )
        string(CONCAT eui_preserve_focus_on_interactive_block
            "    const std::string interactiveTargetId = hitTestInteractive(event, dpiScale);\n"
            "    if (event.pressedThisFrame) {\n"
            "        const std::string focusTargetId = hitTestFocusable(event, dpiScale);\n"
            "        if (!focusTargetId.empty() || interactiveTargetId.empty()) {\n"
            "            setFocusedId(focusTargetId);\n"
            "        }\n"
            "    }\n"
            "\n"
            "    const std::string capturedId = capturedInteractionId();\n"
            "    const std::string hoverTargetId = !capturedId.empty() ? capturedId : interactiveTargetId;\n"
        )
        string(FIND "${eui_runtime_lifecycle_content}"
                    "interactiveTargetId = hitTestInteractive(event, dpiScale)"
                    eui_preserve_focus_pos)
        if(eui_preserve_focus_pos LESS 0)
            string(FIND "${eui_runtime_lifecycle_content}"
                        "${eui_focus_clear_on_interactive_block}"
                        eui_focus_clear_on_interactive_pos)
            if(eui_focus_clear_on_interactive_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_focus_clear_on_interactive_block}"
                               "${eui_preserve_focus_on_interactive_block}"
                               eui_runtime_lifecycle_content
                               "${eui_runtime_lifecycle_content}")
                file(WRITE "${eui_runtime_lifecycle_source}"
                           "${eui_runtime_lifecycle_content}")
                set(eui_patched_app_shortcuts ON)
            endif()
        endif()
    endif()

    set(eui_input_types_source "${eui_source_dir}/core/input/input_types.h")
    if(EXISTS "${eui_input_types_source}")
        file(READ "${eui_input_types_source}" eui_input_types_content)
        set(eui_input_types_changed OFF)
        string(FIND "${eui_input_types_content}"
                    "    bool paste = false;\n"
                    eui_input_types_paste_member_pos)
        if(eui_input_types_paste_member_pos LESS 0)
            string(REPLACE "    std::string pasteText;\n    bool backspace = false;\n"
                           "    std::string pasteText;\n    bool paste = false;\n    bool backspace = false;\n"
                           eui_input_types_content
                           "${eui_input_types_content}")
            set(eui_input_types_changed ON)
        endif()

        string(FIND "${eui_input_types_content}"
                    "return !text.empty() || !pasteText.empty() || paste || backspace"
                    eui_input_types_paste_has_input_pos)
        if(eui_input_types_paste_has_input_pos LESS 0)
            string(REPLACE "return !text.empty() || !pasteText.empty() || backspace"
                           "return !text.empty() || !pasteText.empty() || paste || backspace"
                           eui_input_types_content
                           "${eui_input_types_content}")
            set(eui_input_types_changed ON)
        endif()

        if(eui_input_types_changed)
            file(WRITE "${eui_input_types_source}" "${eui_input_types_content}")
            set(eui_patched_app_shortcuts ON)
        endif()
    endif()

    set(eui_input_state_source "${eui_source_dir}/core/input/input_state.h")
    if(EXISTS "${eui_input_state_source}")
        file(READ "${eui_input_state_source}" eui_input_state_content)
        set(eui_input_state_changed OFF)
        string(FIND "${eui_input_state_content}"
                    "    bool paste = false;\n"
                    eui_input_state_paste_member_pos)
        if(eui_input_state_paste_member_pos LESS 0)
            string(REPLACE "    std::string pasteText;\n    double scrollX = 0.0;\n"
                           "    std::string pasteText;\n    bool paste = false;\n    double scrollX = 0.0;\n"
                           eui_input_state_content
                           "${eui_input_state_content}")
            set(eui_input_state_changed ON)
        endif()

        string(CONCAT eui_input_state_text_paste_block
            "    if (ctrl && key == InputKey::V) {\n"
            "        queue.pasteText += core::window::clipboardText(window);\n"
            "        return;\n"
            "    }\n"
        )
        string(CONCAT eui_input_state_marked_paste_block
            "    if (ctrl && key == InputKey::V) {\n"
            "        queue.paste = true;\n"
            "        queue.pasteText += core::window::clipboardText(window);\n"
            "        return;\n"
            "    }\n"
        )
        string(FIND "${eui_input_state_content}"
                    "        queue.paste = true;\n"
                    eui_input_state_queue_paste_pos)
        if(eui_input_state_queue_paste_pos LESS 0)
            string(FIND "${eui_input_state_content}"
                        "${eui_input_state_text_paste_block}"
                        eui_input_state_text_paste_pos)
            if(eui_input_state_text_paste_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_input_state_text_paste_block}"
                               "${eui_input_state_marked_paste_block}"
                               eui_input_state_content
                               "${eui_input_state_content}")
                set(eui_input_state_changed ON)
            endif()
        endif()

        string(FIND "${eui_input_state_content}"
                    "    keyboard.paste = queue.paste;\n"
                    eui_input_state_keyboard_paste_pos)
        if(eui_input_state_keyboard_paste_pos LESS 0)
            string(REPLACE "    keyboard.pasteText = std::move(queue.pasteText);\n"
                           "    keyboard.pasteText = std::move(queue.pasteText);\n    keyboard.paste = queue.paste;\n"
                           eui_input_state_content
                           "${eui_input_state_content}")
            set(eui_input_state_changed ON)
        endif()

        if(eui_input_state_changed)
            file(WRITE "${eui_input_state_source}" "${eui_input_state_content}")
            set(eui_patched_app_shortcuts ON)
        endif()
    endif()

    set(eui_glfw_app_source "${eui_source_dir}/core/app/glfw_app_main.cpp")
    if(EXISTS "${eui_glfw_app_source}")
        file(READ "${eui_glfw_app_source}" eui_glfw_app_content)
        set(eui_glfw_app_changed OFF)
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
                set(eui_glfw_app_changed ON)
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
            "constexpr const wchar_t* kRelayDeskTrayWindowClassName = L\"RelayDeskTrayWindow\";\n"
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
            "struct SingleInstanceActivationContext {\n"
            "    bool activated = false;\n"
            "};\n"
            "\n"
            "BOOL CALLBACK requestExistingRelayDeskTrayWindow(HWND window, LPARAM contextAddress) {\n"
            "    auto* context = reinterpret_cast<SingleInstanceActivationContext*>(contextAddress);\n"
            "    wchar_t className[64]{};\n"
            "    const int classLength =\n"
            "        GetClassNameW(window, className, static_cast<int>(std::size(className)));\n"
            "    if (classLength <= 0 || std::wstring(className) != kRelayDeskTrayWindowClassName) {\n"
            "        return TRUE;\n"
            "    }\n"
            "    SendMessageW(window, WM_COMMAND, kRelayDeskTrayShowCommand, 0);\n"
            "    context->activated = true;\n"
            "    return FALSE;\n"
            "}\n"
            "\n"
            "BOOL CALLBACK requestExistingRelayDeskMainWindow(HWND window, LPARAM contextAddress) {\n"
            "    auto* context = reinterpret_cast<SingleInstanceActivationContext*>(contextAddress);\n"
            "    wchar_t title[128]{};\n"
            "    const int titleLength =\n"
            "        GetWindowTextW(window, title, static_cast<int>(std::size(title)));\n"
            "    if (titleLength <= 0 || std::wstring(title) != L\"RelayDesk\") {\n"
            "        return TRUE;\n"
            "    }\n"
            "    ShowWindow(window, SW_RESTORE);\n"
            "    SetForegroundWindow(window);\n"
            "    context->activated = true;\n"
            "    return FALSE;\n"
            "}\n"
            "\n"
            "void activateExistingRelayDeskInstance() {\n"
            "    SingleInstanceActivationContext context;\n"
            "    EnumWindows(requestExistingRelayDeskTrayWindow, reinterpret_cast<LPARAM>(&context));\n"
            "    if (!context.activated) {\n"
            "        EnumWindows(requestExistingRelayDeskMainWindow, reinterpret_cast<LPARAM>(&context));\n"
            "    }\n"
            "}\n"
            "#endif\n"
            "\n"
            "float getDpiScale(GLFWwindow* window) {\n"
        )
        string(CONCAT eui_glfw_old_single_instance_block
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
                    "${eui_glfw_old_single_instance_block}"
                    eui_glfw_old_single_instance_pos)
        if(eui_glfw_old_single_instance_pos GREATER_EQUAL 0)
            string(REPLACE "${eui_glfw_old_single_instance_block}"
                           "${eui_glfw_single_instance_block}"
                           eui_glfw_app_content
                           "${eui_glfw_app_content}")
            set(eui_patched_app_shortcuts ON)
            set(eui_glfw_app_changed ON)
        endif()

        string(FIND "${eui_glfw_app_content}"
                    "kRelayDeskTrayWindowClassName"
                    eui_glfw_tray_class_name_pos)
        if(eui_glfw_tray_class_name_pos LESS 0)
            string(CONCAT eui_glfw_tray_command_and_class_block
                "constexpr UINT kRelayDeskTrayShowCommand = 1000;\n"
                "constexpr const wchar_t* kRelayDeskTrayWindowClassName = L\"RelayDeskTrayWindow\";"
            )
            string(REPLACE
                   "constexpr UINT kRelayDeskTrayShowCommand = 1000;"
                   "${eui_glfw_tray_command_and_class_block}"
                   eui_glfw_app_content
                   "${eui_glfw_app_content}")
            set(eui_patched_app_shortcuts ON)
            set(eui_glfw_app_changed ON)
        endif()

        string(CONCAT eui_glfw_old_single_instance_activation_block
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
        )
        string(CONCAT eui_glfw_single_instance_activation_block
            "struct SingleInstanceActivationContext {\n"
            "    bool activated = false;\n"
            "};\n"
            "\n"
            "BOOL CALLBACK requestExistingRelayDeskTrayWindow(HWND window, LPARAM contextAddress) {\n"
            "    auto* context = reinterpret_cast<SingleInstanceActivationContext*>(contextAddress);\n"
            "    wchar_t className[64]{};\n"
            "    const int classLength =\n"
            "        GetClassNameW(window, className, static_cast<int>(std::size(className)));\n"
            "    if (classLength <= 0 || std::wstring(className) != kRelayDeskTrayWindowClassName) {\n"
            "        return TRUE;\n"
            "    }\n"
            "    SendMessageW(window, WM_COMMAND, kRelayDeskTrayShowCommand, 0);\n"
            "    context->activated = true;\n"
            "    return FALSE;\n"
            "}\n"
            "\n"
            "BOOL CALLBACK requestExistingRelayDeskMainWindow(HWND window, LPARAM contextAddress) {\n"
            "    auto* context = reinterpret_cast<SingleInstanceActivationContext*>(contextAddress);\n"
            "    wchar_t title[128]{};\n"
            "    const int titleLength =\n"
            "        GetWindowTextW(window, title, static_cast<int>(std::size(title)));\n"
            "    if (titleLength <= 0 || std::wstring(title) != L\"RelayDesk\") {\n"
            "        return TRUE;\n"
            "    }\n"
            "    ShowWindow(window, SW_RESTORE);\n"
            "    SetForegroundWindow(window);\n"
            "    context->activated = true;\n"
            "    return FALSE;\n"
            "}\n"
            "\n"
            "void activateExistingRelayDeskInstance() {\n"
            "    SingleInstanceActivationContext context;\n"
            "    EnumWindows(requestExistingRelayDeskTrayWindow, reinterpret_cast<LPARAM>(&context));\n"
            "    if (!context.activated) {\n"
            "        EnumWindows(requestExistingRelayDeskMainWindow, reinterpret_cast<LPARAM>(&context));\n"
            "    }\n"
            "}\n"
        )
        string(FIND "${eui_glfw_app_content}"
                    "${eui_glfw_old_single_instance_activation_block}"
                    eui_glfw_old_single_instance_activation_pos)
        if(eui_glfw_old_single_instance_activation_pos GREATER_EQUAL 0)
            string(REPLACE "${eui_glfw_old_single_instance_activation_block}"
                           "${eui_glfw_single_instance_activation_block}"
                           eui_glfw_app_content
                           "${eui_glfw_app_content}")
            set(eui_patched_app_shortcuts ON)
            set(eui_glfw_app_changed ON)
        endif()

        string(CONCAT eui_glfw_restore_from_tray_block
            "void restoreWindowFromTray(GLFWwindow* window, WindowState& windowState) {\n"
            "    if (!windowState.hiddenToTray) {\n"
            "        return;\n"
            "    }\n"
            "\n"
            "    glfwRestoreWindow(window);\n"
            "    glfwShowWindow(window);\n"
            "    glfwFocusWindow(window);\n"
            "    windowState.hiddenToTray = false;\n"
            "    windowState.hideToTrayRequested = false;\n"
            "    windowState.needsRender = true;\n"
            "    windowState.nextFrameTime = glfwGetTime();\n"
            "}\n"
        )
        string(CONCAT eui_glfw_restore_from_tray_with_mode_block
            "void restoreWindowFromTray(GLFWwindow* window,\n"
            "                           WindowState& windowState,\n"
            "                           bool minimized = false) {\n"
            "    if (!windowState.hiddenToTray) {\n"
            "        return;\n"
            "    }\n"
            "\n"
            "    if (minimized) {\n"
            "#ifdef _WIN32\n"
            "        HWND nativeWindow = glfwGetWin32Window(window);\n"
            "        if (nativeWindow != nullptr) {\n"
            "            ShowWindow(nativeWindow, SW_SHOWMINNOACTIVE);\n"
            "            FLASHWINFO flashInfo{};\n"
            "            flashInfo.cbSize = sizeof(flashInfo);\n"
            "            flashInfo.hwnd = nativeWindow;\n"
            "            flashInfo.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;\n"
            "            flashInfo.uCount = 0;\n"
            "            flashInfo.dwTimeout = 0;\n"
            "            (void)FlashWindowEx(&flashInfo);\n"
            "        } else\n"
            "#endif\n"
            "        {\n"
            "            glfwShowWindow(window);\n"
            "            glfwIconifyWindow(window);\n"
            "        }\n"
            "    } else {\n"
            "        glfwRestoreWindow(window);\n"
            "        glfwShowWindow(window);\n"
            "        glfwFocusWindow(window);\n"
            "    }\n"
            "    windowState.hiddenToTray = false;\n"
            "    windowState.hideToTrayRequested = false;\n"
            "    windowState.needsRender = true;\n"
            "    windowState.nextFrameTime = glfwGetTime();\n"
            "}\n"
        )
        string(CONCAT eui_glfw_restore_from_tray_old_minimized_block
            "void restoreWindowFromTray(GLFWwindow* window,\n"
            "                           WindowState& windowState,\n"
            "                           bool minimized = false) {\n"
            "    if (!windowState.hiddenToTray) {\n"
            "        return;\n"
            "    }\n"
            "\n"
            "    glfwRestoreWindow(window);\n"
            "    glfwShowWindow(window);\n"
            "    if (minimized) {\n"
            "#ifdef _WIN32\n"
            "        HWND nativeWindow = glfwGetWin32Window(window);\n"
            "        if (nativeWindow != nullptr) {\n"
            "            ShowWindow(nativeWindow, SW_SHOWMINNOACTIVE);\n"
            "        } else\n"
            "#endif\n"
            "        {\n"
            "            glfwIconifyWindow(window);\n"
            "        }\n"
            "    } else {\n"
            "        glfwFocusWindow(window);\n"
            "    }\n"
            "    windowState.hiddenToTray = false;\n"
            "    windowState.hideToTrayRequested = false;\n"
            "    windowState.needsRender = true;\n"
            "    windowState.nextFrameTime = glfwGetTime();\n"
            "}\n"
        )
        string(FIND "${eui_glfw_app_content}"
                    "${eui_glfw_restore_from_tray_old_minimized_block}"
                    eui_glfw_restore_from_tray_old_minimized_pos)
        if(eui_glfw_restore_from_tray_old_minimized_pos GREATER_EQUAL 0)
            string(REPLACE "${eui_glfw_restore_from_tray_old_minimized_block}"
                           "${eui_glfw_restore_from_tray_with_mode_block}"
                           eui_glfw_app_content
                           "${eui_glfw_app_content}")
            set(eui_patched_app_shortcuts ON)
            set(eui_glfw_app_changed ON)
        endif()
        string(FIND "${eui_glfw_app_content}"
                    "glfwGetWin32Window"
                    eui_glfw_win32_native_pos)
        if(eui_glfw_win32_native_pos LESS 0)
            string(FIND "${eui_glfw_app_content}"
                        "#include <GLFW/glfw3.h>\n"
                        eui_glfw_header_include_pos)
            if(eui_glfw_header_include_pos GREATER_EQUAL 0)
                string(REPLACE "#include <GLFW/glfw3.h>\n"
                               "#include <GLFW/glfw3.h>\n#ifdef _WIN32\n#define GLFW_EXPOSE_NATIVE_WIN32\n#include <GLFW/glfw3native.h>\n#endif\n"
                               eui_glfw_app_content
                               "${eui_glfw_app_content}")
                set(eui_patched_app_shortcuts ON)
                set(eui_glfw_app_changed ON)
            endif()
        endif()
        string(FIND "${eui_glfw_app_content}"
                    "bool minimized = false"
                    eui_glfw_restore_mode_pos)
        if(eui_glfw_restore_mode_pos LESS 0)
            string(FIND "${eui_glfw_app_content}"
                        "${eui_glfw_restore_from_tray_block}"
                        eui_glfw_restore_from_tray_pos)
            if(eui_glfw_restore_from_tray_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_glfw_restore_from_tray_block}"
                               "${eui_glfw_restore_from_tray_with_mode_block}"
                               eui_glfw_app_content
                               "${eui_glfw_app_content}")
                set(eui_patched_app_shortcuts ON)
                set(eui_glfw_app_changed ON)
            endif()
        endif()

        string(CONCAT eui_glfw_consume_tray_show_block
            "        if (windowState.consumeTrayShowRequested()) {\n"
            "            restoreWindowFromTray(window, windowState);\n"
            "        }\n"
        )
        string(CONCAT eui_glfw_consume_tray_show_with_minimize_block
            "        if (windowState.consumeTrayShowRequested()) {\n"
            "            restoreWindowFromTray(window, windowState);\n"
            "        }\n"
            "        if (windowState.consumeTrayShowMinimizedRequested()) {\n"
            "            restoreWindowFromTray(window, windowState, true);\n"
            "        }\n"
        )
        string(FIND "${eui_glfw_app_content}"
                    "consumeTrayShowMinimizedRequested"
                    eui_glfw_minimized_request_pos)
        if(eui_glfw_minimized_request_pos LESS 0)
            string(FIND "${eui_glfw_app_content}"
                        "${eui_glfw_consume_tray_show_block}"
                        eui_glfw_consume_tray_show_pos)
            if(eui_glfw_consume_tray_show_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_glfw_consume_tray_show_block}"
                               "${eui_glfw_consume_tray_show_with_minimize_block}"
                               eui_glfw_app_content
                               "${eui_glfw_app_content}")
                set(eui_patched_app_shortcuts ON)
                set(eui_glfw_app_changed ON)
            endif()
        endif()

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
                set(eui_glfw_app_changed ON)
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
                set(eui_glfw_app_changed ON)
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
            set(eui_glfw_app_changed ON)
        endif()

        string(CONCAT eui_glfw_iconify_hide_to_tray_block
            "    glfwSetWindowIconifyCallback(window, [](GLFWwindow* currentWindow, int iconified) {\n"
            "        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));\n"
            "        if (state && state->trayAvailable && iconified && !state->forceClose) {\n"
            "            state->hideToTrayRequested = true;\n"
            "        }\n"
            "    });\n"
        )
        string(FIND "${eui_glfw_app_content}"
                    "${eui_glfw_iconify_hide_to_tray_block}"
                    eui_glfw_iconify_hide_to_tray_pos)
        if(eui_glfw_iconify_hide_to_tray_pos GREATER_EQUAL 0)
            string(REPLACE "${eui_glfw_iconify_hide_to_tray_block}"
                           ""
                           eui_glfw_app_content
                           "${eui_glfw_app_content}")
            file(WRITE "${eui_glfw_app_source}" "${eui_glfw_app_content}")
            set(eui_patched_app_shortcuts ON)
            set(eui_glfw_app_changed ON)
        endif()

        if(eui_glfw_app_changed)
            file(WRITE "${eui_glfw_app_source}" "${eui_glfw_app_content}")
        endif()
    endif()

    set(eui_app_runner_source "${eui_source_dir}/core/app/app_runner.h")
    if(EXISTS "${eui_app_runner_source}")
        file(READ "${eui_app_runner_source}" eui_app_runner_content)
        string(CONCAT eui_app_runner_show_method_block
            "    bool consumeTrayShowRequested() {\n"
            "        return core::platform::consumeTrayShowRequested();\n"
            "    }\n"
        )
        string(CONCAT eui_app_runner_show_methods_block
            "    bool consumeTrayShowRequested() {\n"
            "        return core::platform::consumeTrayShowRequested();\n"
            "    }\n"
            "\n"
            "    bool consumeTrayShowMinimizedRequested() {\n"
            "        return core::platform::consumeTrayShowMinimizedRequested();\n"
            "    }\n"
        )
        string(FIND "${eui_app_runner_content}"
                    "consumeTrayShowMinimizedRequested"
                    eui_app_runner_minimized_pos)
        if(eui_app_runner_minimized_pos LESS 0)
            string(FIND "${eui_app_runner_content}"
                        "${eui_app_runner_show_method_block}"
                        eui_app_runner_show_method_pos)
            if(eui_app_runner_show_method_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_app_runner_show_method_block}"
                               "${eui_app_runner_show_methods_block}"
                               eui_app_runner_content
                               "${eui_app_runner_content}")
                file(WRITE "${eui_app_runner_source}"
                           "${eui_app_runner_content}")
                set(eui_patched_app_shortcuts ON)
            endif()
        endif()
    endif()

    set(eui_platform_header_source "${eui_source_dir}/core/platform/platform.h")
    if(EXISTS "${eui_platform_header_source}")
        file(READ "${eui_platform_header_source}" eui_platform_header_content)
        string(FIND "${eui_platform_header_content}"
                    "requestTrayShowMinimized"
                    eui_platform_header_request_minimized_pos)
        if(eui_platform_header_request_minimized_pos LESS 0)
            string(REPLACE "bool consumeTrayShowRequested();\n"
                           "bool consumeTrayShowRequested();\nbool consumeTrayShowMinimizedRequested();\nvoid requestTrayShowMinimized();\n"
                           eui_platform_header_content
                           "${eui_platform_header_content}")
            file(WRITE "${eui_platform_header_source}"
                       "${eui_platform_header_content}")
            set(eui_patched_app_shortcuts ON)
        endif()
    endif()

    set(eui_platform_source "${eui_source_dir}/core/platform/platform.cpp")
    if(EXISTS "${eui_platform_source}")
        file(READ "${eui_platform_source}" eui_platform_content)
        string(FIND "${eui_platform_content}"
                    "consumeTrayShowMinimizedRequested"
                    eui_platform_minimized_pos)
        if(eui_platform_minimized_pos LESS 0)
            string(CONCAT eui_platform_show_function_block
                "bool consumeTrayShowRequested() {\n"
                "    return eui_tray_consume_show_requested() != 0;\n"
                "}\n"
            )
            string(CONCAT eui_platform_show_functions_block
                "bool consumeTrayShowRequested() {\n"
                "    return eui_tray_consume_show_requested() != 0;\n"
                "}\n"
                "\n"
                "bool consumeTrayShowMinimizedRequested() {\n"
                "    return eui_tray_consume_show_minimized_requested() != 0;\n"
                "}\n"
                "\n"
                "void requestTrayShowMinimized() {\n"
                "    eui_tray_request_show_minimized();\n"
                "}\n"
            )
            string(FIND "${eui_platform_content}"
                        "${eui_platform_show_function_block}"
                        eui_platform_show_function_pos)
            if(eui_platform_show_function_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_platform_show_function_block}"
                               "${eui_platform_show_functions_block}"
                               eui_platform_content
                               "${eui_platform_content}")
                file(WRITE "${eui_platform_source}" "${eui_platform_content}")
                set(eui_patched_app_shortcuts ON)
            endif()
        endif()
    endif()

    set(eui_tray_bridge_header_source "${eui_source_dir}/core/platform/tray_bridge.h")
    if(EXISTS "${eui_tray_bridge_header_source}")
        file(READ "${eui_tray_bridge_header_source}" eui_tray_bridge_header_content)
        string(FIND "${eui_tray_bridge_header_content}"
                    "eui_tray_request_show_minimized"
                    eui_tray_bridge_header_minimized_pos)
        if(eui_tray_bridge_header_minimized_pos LESS 0)
            string(REPLACE "int eui_tray_consume_show_requested(void);\n"
                           "int eui_tray_consume_show_requested(void);\nint eui_tray_consume_show_minimized_requested(void);\nvoid eui_tray_request_show_minimized(void);\n"
                           eui_tray_bridge_header_content
                           "${eui_tray_bridge_header_content}")
            file(WRITE "${eui_tray_bridge_header_source}"
                       "${eui_tray_bridge_header_content}")
            set(eui_patched_app_shortcuts ON)
        endif()
    endif()

    set(eui_tray_bridge_source "${eui_source_dir}/core/platform/tray_bridge.c")
    if(EXISTS "${eui_tray_bridge_source}")
        file(READ "${eui_tray_bridge_source}" eui_tray_bridge_content)
        string(FIND "${eui_tray_bridge_content}"
                    "eui_tray_request_show_minimized"
                    eui_tray_bridge_minimized_pos)
        if(eui_tray_bridge_minimized_pos LESS 0)
            string(REPLACE "static int g_show_requested = 0;\n"
                           "static int g_show_requested = 0;\n#if defined(EUI_TRAY_WINAPI)\nstatic volatile LONG g_show_minimized_requested = 0;\n#else\nstatic int g_show_minimized_requested = 0;\n#endif\n"
                           eui_tray_bridge_content
                           "${eui_tray_bridge_content}")
            string(REPLACE "    g_show_requested = 0;\n    g_exit_requested = 0;\n"
                           "    g_show_requested = 0;\n    g_show_minimized_requested = 0;\n    g_exit_requested = 0;\n"
                           eui_tray_bridge_content
                           "${eui_tray_bridge_content}")
            string(CONCAT eui_tray_bridge_consume_show_block
                "int eui_tray_consume_show_requested(void) {\n"
                "    int requested = g_show_requested;\n"
                "    g_show_requested = 0;\n"
                "    return requested;\n"
                "}\n"
            )
            string(CONCAT eui_tray_bridge_consume_show_functions_block
                "int eui_tray_consume_show_requested(void) {\n"
                "    int requested = g_show_requested;\n"
                "    g_show_requested = 0;\n"
                "    return requested;\n"
                "}\n"
                "\n"
                "int eui_tray_consume_show_minimized_requested(void) {\n"
                "#if defined(EUI_TRAY_WINAPI)\n"
                "    return InterlockedExchange(&g_show_minimized_requested, 0) != 0;\n"
                "#else\n"
                "    int requested = g_show_minimized_requested;\n"
                "    g_show_minimized_requested = 0;\n"
                "    return requested;\n"
                "#endif\n"
                "}\n"
                "\n"
                "void eui_tray_request_show_minimized(void) {\n"
                "#if defined(EUI_TRAY_WINAPI)\n"
                "    InterlockedExchange(&g_show_minimized_requested, 1);\n"
                "#else\n"
                "    g_show_minimized_requested = 1;\n"
                "#endif\n"
                "}\n"
            )
            string(FIND "${eui_tray_bridge_content}"
                        "${eui_tray_bridge_consume_show_block}"
                        eui_tray_bridge_consume_show_pos)
            if(eui_tray_bridge_consume_show_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_tray_bridge_consume_show_block}"
                               "${eui_tray_bridge_consume_show_functions_block}"
                               eui_tray_bridge_content
                               "${eui_tray_bridge_content}")
            endif()
            string(CONCAT eui_tray_bridge_stub_consume_show_block
                "int eui_tray_consume_show_requested(void) {\n"
                "    return 0;\n"
                "}\n"
            )
            string(CONCAT eui_tray_bridge_stub_consume_show_functions_block
                "int eui_tray_consume_show_requested(void) {\n"
                "    return 0;\n"
                "}\n"
                "\n"
                "int eui_tray_consume_show_minimized_requested(void) {\n"
                "    return 0;\n"
                "}\n"
                "\n"
                "void eui_tray_request_show_minimized(void) {\n"
                "}\n"
            )
            string(FIND "${eui_tray_bridge_content}"
                        "${eui_tray_bridge_stub_consume_show_block}"
                        eui_tray_bridge_stub_consume_show_pos)
            if(eui_tray_bridge_stub_consume_show_pos GREATER_EQUAL 0)
                string(REPLACE "${eui_tray_bridge_stub_consume_show_block}"
                               "${eui_tray_bridge_stub_consume_show_functions_block}"
                               eui_tray_bridge_content
                               "${eui_tray_bridge_content}")
            endif()
            file(WRITE "${eui_tray_bridge_source}"
                       "${eui_tray_bridge_content}")
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
        string(FIND "${eui_tray_content}"
                    "#define WC_TRAY_CLASS_NAME \"TRAY\""
                    eui_tray_default_class_pos)
        if(eui_tray_default_class_pos GREATER_EQUAL 0)
            string(REPLACE "#define WC_TRAY_CLASS_NAME \"TRAY\""
                           "#define WC_TRAY_CLASS_NAME \"RelayDeskTrayWindow\""
                           eui_tray_content
                           "${eui_tray_content}")
            file(WRITE "${eui_tray_source}" "${eui_tray_content}")
            set(eui_patched_app_shortcuts ON)
        endif()

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
        message(STATUS "Patched EUI-NEO app main shortcuts and tray behavior")
    endif()
endfunction()

if(DEFINED EUI_NEO_SOURCE_DIR)
    relaydesk_patch_eui_neo_app_shortcuts("${EUI_NEO_SOURCE_DIR}")
endif()
