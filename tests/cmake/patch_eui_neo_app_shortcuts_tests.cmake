if(NOT DEFINED RELAYDESK_SOURCE_DIR)
    message(FATAL_ERROR "RELAYDESK_SOURCE_DIR is required")
endif()

if(NOT DEFINED RELAYDESK_PATCH_TEST_WORK_DIR)
    message(FATAL_ERROR "RELAYDESK_PATCH_TEST_WORK_DIR is required")
endif()

set(eui_source_dir "${RELAYDESK_PATCH_TEST_WORK_DIR}/eui-neo")
set(eui_glfw_app_source "${eui_source_dir}/core/app/glfw_app_main.cpp")
set(eui_runtime_lifecycle_source
    "${eui_source_dir}/core/runtime/runtime_lifecycle.h")

file(REMOVE_RECURSE "${RELAYDESK_PATCH_TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${eui_source_dir}/core/app")
file(MAKE_DIRECTORY "${eui_source_dir}/core/runtime")

string(CONCAT eui_glfw_app_content
    "#include <chrono>\n"
    "#include <cstdio>\n"
    "#include <memory>\n"
    "#include <thread>\n"
    "#include <vector>\n"
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
    "int main() {\n"
    "    core::render::initializeRenderBackendLoader();\n"
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
