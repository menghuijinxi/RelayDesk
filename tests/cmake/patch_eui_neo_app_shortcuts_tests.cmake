if(NOT DEFINED RELAYDESK_SOURCE_DIR)
    message(FATAL_ERROR "RELAYDESK_SOURCE_DIR is required")
endif()

if(NOT DEFINED RELAYDESK_PATCH_TEST_WORK_DIR)
    message(FATAL_ERROR "RELAYDESK_PATCH_TEST_WORK_DIR is required")
endif()

set(eui_source_dir "${RELAYDESK_PATCH_TEST_WORK_DIR}/eui-neo")
set(eui_glfw_app_source "${eui_source_dir}/core/app/glfw_app_main.cpp")
set(eui_app_runner_source "${eui_source_dir}/core/app/app_runner.h")
set(eui_platform_header_source "${eui_source_dir}/core/platform/platform.h")
set(eui_tray_bridge_header_source "${eui_source_dir}/core/platform/tray_bridge.h")
set(eui_platform_source "${eui_source_dir}/core/platform/platform.cpp")
set(eui_tray_bridge_source "${eui_source_dir}/core/platform/tray_bridge.c")
set(eui_runtime_lifecycle_source
    "${eui_source_dir}/core/runtime/runtime_lifecycle.h")
set(eui_input_types_source "${eui_source_dir}/core/input/input_types.h")
set(eui_input_state_source "${eui_source_dir}/core/input/input_state.h")
set(eui_tray_source "${eui_source_dir}/3rd/tray/tray.h")

file(REMOVE_RECURSE "${RELAYDESK_PATCH_TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${eui_source_dir}/core/app")
file(MAKE_DIRECTORY "${eui_source_dir}/core/input")
file(MAKE_DIRECTORY "${eui_source_dir}/core/platform")
file(MAKE_DIRECTORY "${eui_source_dir}/core/runtime")
file(MAKE_DIRECTORY "${eui_source_dir}/3rd/tray")

string(CONCAT eui_glfw_app_content
    "#include <GLFW/glfw3.h>\n"
    "\n"
    "#include <chrono>\n"
    "#include <cstdio>\n"
    "#include <memory>\n"
    "#include <thread>\n"
    "#include <vector>\n"
    "\n"
    "struct WindowState : app::AppRunner {\n"
    "    bool hideToTrayRequested = false;\n"
    "    bool forceClose = false;\n"
    "    GLFWwindow* modalChildWindow = nullptr;\n"
    "};\n"
    "\n"
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
    "    return 1.0f;\n"
    "}\n"
    "\n"
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
    "\n"
    "int main() {\n"
    "    core::render::initializeRenderBackendLoader();\n"
    "        if (windowState.consumeTrayShowRequested()) {\n"
    "            restoreWindowFromTray(window, windowState);\n"
    "        }\n"
    "        if (windowState.modalChildWindow == nullptr && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {\n"
    "            if (windowState.trayAvailable) {\n"
    "                windowState.hideToTrayRequested = true;\n"
    "            } else {\n"
    "                glfwSetWindowShouldClose(window, 1);\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "\n"
    "    glfwSetWindowCloseCallback(window, [](GLFWwindow* currentWindow) {\n"
    "        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));\n"
    "        if (state && state->modalChildWindow != nullptr && !glfwWindowShouldClose(state->modalChildWindow)) {\n"
    "            glfwFocusWindow(state->modalChildWindow);\n"
    "            glfwSetWindowShouldClose(currentWindow, GLFW_FALSE);\n"
    "            return;\n"
    "        }\n"
    "        if (state && state->trayAvailable && !state->forceClose) {\n"
    "            state->hideToTrayRequested = true;\n"
    "            glfwSetWindowShouldClose(currentWindow, GLFW_FALSE);\n"
    "        }\n"
    "    });\n"
    "    glfwSetWindowIconifyCallback(window, [](GLFWwindow* currentWindow, int iconified) {\n"
    "        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));\n"
    "        if (state && state->trayAvailable && iconified && !state->forceClose) {\n"
    "            state->hideToTrayRequested = true;\n"
    "        }\n"
    "    });\n"
    "}\n"
)

file(WRITE "${eui_glfw_app_source}" "${eui_glfw_app_content}")

string(CONCAT eui_app_runner_content
    "#pragma once\n"
    "\n"
    "struct AppRunner {\n"
    "    bool consumeTrayShowRequested() {\n"
    "        return core::platform::consumeTrayShowRequested();\n"
    "    }\n"
    "};\n"
)

file(WRITE "${eui_app_runner_source}" "${eui_app_runner_content}")

string(CONCAT eui_platform_header_content
    "#pragma once\n"
    "\n"
    "namespace core::platform {\n"
    "\n"
    "bool consumeTrayShowRequested();\n"
    "bool consumeTrayExitRequested();\n"
    "\n"
    "} // namespace core::platform\n"
)

file(WRITE "${eui_platform_header_source}" "${eui_platform_header_content}")

string(CONCAT eui_platform_content
    "#include \"core/platform/platform.h\"\n"
    "\n"
    "namespace core::platform {\n"
    "\n"
    "bool consumeTrayShowRequested() {\n"
    "    return eui_tray_consume_show_requested() != 0;\n"
    "}\n"
    "\n"
    "bool consumeTrayExitRequested() {\n"
    "    return eui_tray_consume_exit_requested() != 0;\n"
    "}\n"
    "\n"
    "} // namespace core::platform\n"
)

file(WRITE "${eui_platform_source}" "${eui_platform_content}")

string(CONCAT eui_tray_bridge_header_content
    "#pragma once\n"
    "\n"
    "int eui_tray_consume_show_requested(void);\n"
    "int eui_tray_consume_exit_requested(void);\n"
)

file(WRITE "${eui_tray_bridge_header_source}" "${eui_tray_bridge_header_content}")

string(CONCAT eui_tray_bridge_content
    "#include \"core/platform/tray_bridge.h\"\n"
    "\n"
    "static int g_initialized = 0;\n"
    "static int g_show_requested = 0;\n"
    "static int g_exit_requested = 0;\n"
    "\n"
    "int eui_tray_init(const char* icon_path) {\n"
    "    (void)icon_path;\n"
    "    g_show_requested = 0;\n"
    "    g_exit_requested = 0;\n"
    "    g_initialized = 1;\n"
    "    return 1;\n"
    "}\n"
    "\n"
    "int eui_tray_consume_show_requested(void) {\n"
    "    int requested = g_show_requested;\n"
    "    g_show_requested = 0;\n"
    "    return requested;\n"
    "}\n"
    "\n"
    "int eui_tray_consume_exit_requested(void) {\n"
    "    int requested = g_exit_requested;\n"
    "    g_exit_requested = 0;\n"
    "    return requested;\n"
    "}\n"
    "\n"
    "void eui_tray_shutdown(void) {\n"
    "    g_initialized = 0;\n"
    "    g_show_requested = 0;\n"
    "    g_exit_requested = 0;\n"
    "}\n"
    "\n"
    "#else\n"
    "\n"
    "int eui_tray_consume_show_requested(void) {\n"
    "    return 0;\n"
    "}\n"
    "\n"
    "int eui_tray_consume_exit_requested(void) {\n"
    "    return 0;\n"
    "}\n"
)

file(WRITE "${eui_tray_bridge_source}" "${eui_tray_bridge_content}")

string(CONCAT eui_runtime_lifecycle_content
    "#pragma once\n"
    "\n"
    "inline bool Runtime::update(core::window::Handle window, float deltaSeconds, float pointerScale, float dpiScale, bool inputEnabled) {\n"
    "    PointerEvent event = readPointerEvent(window, pointerScale);\n"
    "    if (event.pressedThisFrame) {\n"
    "        setFocusedId(hitTestFocusable(event, dpiScale));\n"
    "    }\n"
    "\n"
    "    const std::string capturedId = capturedInteractionId();\n"
    "    const std::string hoverTargetId = !capturedId.empty() ? capturedId : hitTestInteractive(event, dpiScale);\n"
    "    updateElementTree(event, deltaSeconds, dpiScale, hoverTargetId);\n"
    "    return needsRender_;\n"
    "}\n"
)

file(WRITE "${eui_runtime_lifecycle_source}" "${eui_runtime_lifecycle_content}")

string(CONCAT eui_input_types_content
    "#pragma once\n"
    "\n"
    "#include <string>\n"
    "\n"
    "namespace core {\n"
    "\n"
    "struct KeyboardEvent {\n"
    "    std::string text;\n"
    "    std::string pasteText;\n"
    "    bool backspace = false;\n"
    "\n"
    "    bool hasInput() const {\n"
    "        return !text.empty() || !pasteText.empty() || backspace;\n"
    "    }\n"
    "};\n"
    "\n"
    "} // namespace core\n"
)

file(WRITE "${eui_input_types_source}" "${eui_input_types_content}")

string(CONCAT eui_input_state_content
    "#pragma once\n"
    "\n"
    "#include \"core/input/input_types.h\"\n"
    "\n"
    "#include <string>\n"
    "#include <utility>\n"
    "\n"
    "namespace core {\n"
    "\n"
    "namespace window {\n"
    "using Handle = void*;\n"
    "std::string clipboardText(Handle window);\n"
    "} // namespace window\n"
    "\n"
    "enum class InputKey {\n"
    "    V\n"
    "};\n"
    "\n"
    "namespace detail {\n"
    "struct InputQueue {\n"
    "    std::string text;\n"
    "    std::string pasteText;\n"
    "};\n"
    "InputQueue& inputQueue(window::Handle window);\n"
    "} // namespace detail\n"
    "\n"
    "inline void queueKeyInput(window::Handle window, InputKey key, bool ctrl = false) {\n"
    "    detail::InputQueue& queue = detail::inputQueue(window);\n"
    "    if (ctrl && key == InputKey::V) {\n"
    "        queue.pasteText += core::window::clipboardText(window);\n"
    "        return;\n"
    "    }\n"
    "}\n"
    "\n"
    "inline std::pair<KeyboardEvent, int> consumeInputEvents(window::Handle window) {\n"
    "    detail::InputQueue& queue = detail::inputQueue(window);\n"
    "    KeyboardEvent keyboard;\n"
    "    keyboard.text = std::move(queue.text);\n"
    "    keyboard.pasteText = std::move(queue.pasteText);\n"
    "    return {std::move(keyboard), 0};\n"
    "}\n"
    "\n"
    "} // namespace core\n"
)

file(WRITE "${eui_input_state_source}" "${eui_input_state_content}")

string(CONCAT eui_tray_content
    "#define WM_TRAY_CALLBACK_MESSAGE (WM_USER + 1)\n"
    "#define WC_TRAY_CLASS_NAME \"TRAY\"\n"
    "#define ID_TRAY_FIRST 1000\n"
    "static LRESULT CALLBACK _tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {\n"
    "  switch (msg) {\n"
    "  case WM_TRAY_CALLBACK_MESSAGE:\n"
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
    "  }\n"
    "  return DefWindowProc(hwnd, msg, wparam, lparam);\n"
    "}\n"
)

file(WRITE "${eui_tray_source}" "${eui_tray_content}")

include("${RELAYDESK_SOURCE_DIR}/cmake/PatchEuiNeoAppShortcuts.cmake")
relaydesk_patch_eui_neo_app_shortcuts("${eui_source_dir}")

file(READ "${eui_glfw_app_source}" patched_content)

string(FIND "${patched_content}"
            "glfwSetWindowCloseCallback(window"
            close_callback_pos)
if(close_callback_pos LESS 0)
    message(FATAL_ERROR "Close callback was unexpectedly removed")
endif()

string(FIND "${patched_content}"
            "if (state && state->trayAvailable && !state->forceClose)"
            close_hide_to_tray_pos)
if(close_hide_to_tray_pos LESS 0)
    message(FATAL_ERROR "Close-to-tray behavior was unexpectedly removed")
endif()

string(FIND "${patched_content}"
            "iconified && !state->forceClose"
            iconify_hide_condition_pos)
if(iconify_hide_condition_pos GREATER_EQUAL 0)
    message(FATAL_ERROR "Minimize still requests hide-to-tray")
endif()

string(FIND "${patched_content}"
            "singleInstance.alreadyRunning"
            single_instance_pos)
if(single_instance_pos LESS 0)
    message(FATAL_ERROR "Single-instance patch was not applied")
endif()

string(FIND "${patched_content}"
            "requestExistingRelayDeskTrayWindow"
            tray_first_activation_pos)
if(tray_first_activation_pos LESS 0)
    message(FATAL_ERROR "Single-instance activation does not target the tray window first")
endif()

string(FIND "${patched_content}"
            "if (!context.activated)"
            fallback_activation_pos)
if(fallback_activation_pos LESS 0)
    message(FATAL_ERROR "Single-instance activation fallback was not applied")
endif()

string(FIND "${patched_content}"
            "requestExistingRelayDeskWindow"
            old_activation_pos)
if(old_activation_pos GREATER_EQUAL 0)
    message(FATAL_ERROR "Single-instance activation still uses direct mixed window restore")
endif()

string(FIND "${patched_content}"
            "GLFW_EXPOSE_NATIVE_WIN32"
            glfw_native_window_pos)
if(glfw_native_window_pos LESS 0)
    message(FATAL_ERROR "GLFW Win32 native include was not enabled")
endif()

string(FIND "${patched_content}"
            "bool minimized = false"
            minimized_restore_pos)
if(minimized_restore_pos LESS 0)
    message(FATAL_ERROR "Tray restore does not support minimized mode")
endif()

string(FIND "${patched_content}"
            "ShowWindow(nativeWindow, SW_SHOWMINNOACTIVE);"
            direct_minimized_show_pos)
if(direct_minimized_show_pos LESS 0)
    message(FATAL_ERROR "Minimized tray restore does not show the native window directly")
endif()

string(FIND "${patched_content}"
            "FLASHW_TRAY | FLASHW_TIMERNOFG"
            minimized_flash_pos)
if(minimized_flash_pos LESS 0)
    message(FATAL_ERROR "Minimized tray restore does not flash the taskbar icon")
endif()

string(FIND "${patched_content}"
            "    glfwRestoreWindow(window);\n    glfwShowWindow(window);\n    if (minimized) {"
            old_visible_then_minimize_pos)
if(old_visible_then_minimize_pos GREATER_EQUAL 0)
    message(FATAL_ERROR "Minimized tray restore still shows the window before minimizing it")
endif()

string(FIND "${patched_content}"
            "consumeTrayShowMinimizedRequested"
            consume_minimized_request_pos)
if(consume_minimized_request_pos LESS 0)
    message(FATAL_ERROR "Main loop does not consume minimized tray show requests")
endif()

file(READ "${eui_runtime_lifecycle_source}" patched_runtime_lifecycle_content)

string(FIND "${patched_runtime_lifecycle_content}"
            "interactiveTargetId = hitTestInteractive(event, dpiScale)"
            interactive_target_pos)
if(interactive_target_pos LESS 0)
    message(FATAL_ERROR "Focus retention did not reuse the interactive hit target")
endif()

string(FIND "${patched_runtime_lifecycle_content}"
            "if (!focusTargetId.empty() || interactiveTargetId.empty())"
            preserve_focus_condition_pos)
if(preserve_focus_condition_pos LESS 0)
    message(FATAL_ERROR "Non-focusable interactive clicks still clear focus")
endif()

file(READ "${eui_input_types_source}" patched_input_types_content)
string(FIND "${patched_input_types_content}"
            "bool paste = false"
            input_types_paste_member_pos)
if(input_types_paste_member_pos LESS 0)
    message(FATAL_ERROR "KeyboardEvent does not expose a paste event flag")
endif()

string(FIND "${patched_input_types_content}"
            "|| paste || backspace"
            input_types_paste_has_input_pos)
if(input_types_paste_has_input_pos LESS 0)
    message(FATAL_ERROR "Empty-text paste events are still dropped as no input")
endif()

file(READ "${eui_input_state_source}" patched_input_state_content)
string(FIND "${patched_input_state_content}"
            "queue.paste = true;"
            input_state_queue_paste_pos)
if(input_state_queue_paste_pos LESS 0)
    message(FATAL_ERROR "Ctrl+V does not mark paste events independently from text")
endif()

string(FIND "${patched_input_state_content}"
            "keyboard.paste = queue.paste;"
            input_state_keyboard_paste_pos)
if(input_state_keyboard_paste_pos LESS 0)
    message(FATAL_ERROR "Input queue paste flag is not forwarded to KeyboardEvent")
endif()

file(READ "${eui_tray_source}" patched_tray_content)

string(FIND "${patched_tray_content}"
            "#define WC_TRAY_CLASS_NAME \"RelayDeskTrayWindow\""
            relaydesk_tray_class_pos)
if(relaydesk_tray_class_pos LESS 0)
    message(FATAL_ERROR "Tray window class was not made RelayDesk-specific")
endif()

string(FIND "${patched_tray_content}"
            "if (lparam == WM_LBUTTONUP) {"
            tray_left_click_pos)
if(tray_left_click_pos LESS 0)
    message(FATAL_ERROR "Tray left click no longer opens the window directly")
endif()

file(READ "${eui_app_runner_source}" patched_app_runner_content)
string(FIND "${patched_app_runner_content}"
            "consumeTrayShowMinimizedRequested"
            app_runner_minimized_request_pos)
if(app_runner_minimized_request_pos LESS 0)
    message(FATAL_ERROR "AppRunner does not expose minimized tray show requests")
endif()

file(READ "${eui_platform_header_source}" patched_platform_header_content)
string(FIND "${patched_platform_header_content}"
            "void requestTrayShowMinimized();"
            platform_request_minimized_pos)
if(platform_request_minimized_pos LESS 0)
    message(FATAL_ERROR "Platform header does not expose minimized tray show requests")
endif()

file(READ "${eui_platform_source}" patched_platform_content)
string(FIND "${patched_platform_content}"
            "eui_tray_request_show_minimized();"
            platform_request_bridge_pos)
if(platform_request_bridge_pos LESS 0)
    message(FATAL_ERROR "Platform implementation does not forward minimized tray show requests")
endif()

file(READ "${eui_tray_bridge_header_source}" patched_tray_bridge_header_content)
string(FIND "${patched_tray_bridge_header_content}"
            "void eui_tray_request_show_minimized(void);"
            tray_bridge_header_request_pos)
if(tray_bridge_header_request_pos LESS 0)
    message(FATAL_ERROR "Tray bridge header does not expose minimized show requests")
endif()

file(READ "${eui_tray_bridge_source}" patched_tray_bridge_content)
string(FIND "${patched_tray_bridge_content}"
            "InterlockedExchange(&g_show_minimized_requested, 1)"
            tray_bridge_atomic_request_pos)
if(tray_bridge_atomic_request_pos LESS 0)
    message(FATAL_ERROR "Tray bridge minimized show request is not thread-safe")
endif()

set(old_minimized_source_dir
    "${RELAYDESK_PATCH_TEST_WORK_DIR}/eui-neo-old-minimized")
set(old_minimized_glfw_source
    "${old_minimized_source_dir}/core/app/glfw_app_main.cpp")
file(MAKE_DIRECTORY "${old_minimized_source_dir}/core/app")

string(CONCAT old_minimized_glfw_content
    "#include <GLFW/glfw3.h>\n"
    "#ifdef _WIN32\n"
    "#define GLFW_EXPOSE_NATIVE_WIN32\n"
    "#include <GLFW/glfw3native.h>\n"
    "#endif\n"
    "\n"
    "#include <chrono>\n"
    "#include <cstdio>\n"
    "#include <memory>\n"
    "#include <thread>\n"
    "#include <vector>\n"
    "\n"
    "struct WindowState : app::AppRunner {\n"
    "    bool hideToTrayRequested = false;\n"
    "    bool forceClose = false;\n"
    "    GLFWwindow* modalChildWindow = nullptr;\n"
    "};\n"
    "\n"
    "float getDpiScale(GLFWwindow* window) {\n"
    "    return 1.0f;\n"
    "}\n"
    "\n"
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
file(WRITE "${old_minimized_glfw_source}" "${old_minimized_glfw_content}")

relaydesk_patch_eui_neo_app_shortcuts("${old_minimized_source_dir}")

file(READ "${old_minimized_glfw_source}" upgraded_old_minimized_content)
string(FIND "${upgraded_old_minimized_content}"
            "FLASHW_TRAY | FLASHW_TIMERNOFG"
            upgraded_old_minimized_flash_pos)
if(upgraded_old_minimized_flash_pos LESS 0)
    message(FATAL_ERROR "Old minimized tray restore was not upgraded to flash")
endif()

string(FIND "${upgraded_old_minimized_content}"
            "    glfwRestoreWindow(window);\n    glfwShowWindow(window);\n    if (minimized) {"
            upgraded_old_visible_then_minimize_pos)
if(upgraded_old_visible_then_minimize_pos GREATER_EQUAL 0)
    message(FATAL_ERROR "Old minimized tray restore still shows before minimizing")
endif()
