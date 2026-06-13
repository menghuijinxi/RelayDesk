function(relaydesk_patch_eui_neo_skia_backend eui_source_dir)
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

    set(eui_render_backend_source "${eui_source_dir}/core/render/render_backend.cpp")
    if(NOT EXISTS "${eui_render_backend_source}")
        message(WARNING "EUI-NEO render backend source not found: ${eui_render_backend_source}")
        return()
    endif()

    file(READ "${eui_render_backend_source}" eui_render_backend_content)

    set(eui_opengl_include [=[#if defined(EUI_RENDER_BACKEND_OPENGL)
#include "core/render/opengl/opengl_backend.h"]=])
    set(eui_skia_include [=[#if defined(RELAYDESK_EUI_SKIA_BACKEND) && defined(EUI_RENDER_BACKEND_OPENGL)
#include "SkiaOpenGLRenderBackend.h"
#endif

#if defined(EUI_RENDER_BACKEND_OPENGL)
#include "core/render/opengl/opengl_backend.h"]=])
    string(FIND "${eui_render_backend_content}" "SkiaOpenGLRenderBackend.h" skia_include_pos)
    if(skia_include_pos EQUAL -1)
        string(FIND "${eui_render_backend_content}" "${eui_opengl_include}" opengl_include_pos)
        if(opengl_include_pos EQUAL -1)
            message(WARNING "EUI-NEO render backend include block changed upstream; Skia include patch was not applied")
        else()
            string(REPLACE "${eui_opengl_include}" "${eui_skia_include}" eui_render_backend_content "${eui_render_backend_content}")
        endif()
    endif()

    set(eui_opengl_factory [=[#if defined(EUI_RENDER_BACKEND_OPENGL)
    return std::make_unique<opengl::OpenGLRenderBackend>(window, shareBackend);]=])
    set(eui_skia_factory [=[#if defined(RELAYDESK_EUI_SKIA_BACKEND) && defined(EUI_RENDER_BACKEND_OPENGL)
    return std::make_unique<skia::SkiaOpenGLRenderBackend>(window, shareBackend);
#elif defined(EUI_RENDER_BACKEND_OPENGL)
    return std::make_unique<opengl::OpenGLRenderBackend>(window, shareBackend);]=])
    string(FIND "${eui_render_backend_content}" "std::make_unique<skia::SkiaOpenGLRenderBackend>" skia_factory_pos)
    if(skia_factory_pos EQUAL -1)
        string(FIND "${eui_render_backend_content}" "${eui_opengl_factory}" opengl_factory_pos)
        if(opengl_factory_pos EQUAL -1)
            message(WARNING "EUI-NEO render backend factory changed upstream; Skia factory patch was not applied")
        else()
            string(REPLACE "${eui_opengl_factory}" "${eui_skia_factory}" eui_render_backend_content "${eui_render_backend_content}")
        endif()
    endif()

    file(WRITE "${eui_render_backend_source}" "${eui_render_backend_content}")
    message(STATUS "Patched EUI-NEO render backend factory for optional Skia OpenGL backend")
endfunction()

