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

    if(eui_patched_app_shortcuts)
        message(STATUS "Patched EUI-NEO app main to ignore Escape close shortcut")
    endif()
endfunction()

if(DEFINED EUI_NEO_SOURCE_DIR)
    relaydesk_patch_eui_neo_app_shortcuts("${EUI_NEO_SOURCE_DIR}")
endif()
