function(relaydesk_patch_eui_neo_skia_text_source eui_source_dir)
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

    set(eui_render_backend_header "${eui_source_dir}/core/render/render_backend.h")
    set(eui_text_source "${eui_source_dir}/core/render/text.cpp")
    if(NOT EXISTS "${eui_render_backend_header}" OR NOT EXISTS "${eui_text_source}")
        message(WARNING "EUI-NEO text source patch targets were not found")
        return()
    endif()

    file(READ "${eui_render_backend_header}" eui_render_backend_content)
    set(eui_backend_include_anchor [=[#include "core/render/primitive_geometry.h"
#include "core/render/render_surface.h"]=])
    set(eui_backend_include_with_text [=[#include "core/render/primitive_geometry.h"
#include "core/render/render_surface.h"
#include "core/render/text_types.h"]=])
    string(FIND "${eui_render_backend_content}" "#include \"core/render/text_types.h\"" text_include_pos)
    if(text_include_pos EQUAL -1)
        string(REPLACE "${eui_backend_include_anchor}"
                       "${eui_backend_include_with_text}"
                       eui_render_backend_content
                       "${eui_render_backend_content}")
    endif()

    set(eui_text_command_anchor [=[struct TextDrawCommand {
    const float* vertices = nullptr;
    std::size_t vertexFloatCount = 0;
    core::Color color{};
    TextAtlasPageData grayAtlas{};
    TextAtlasPageData colorAtlas{};
};]=])
    set(eui_text_command_with_source [=[struct TextSourceDrawData {
    const char* text = nullptr;
    std::size_t textByteCount = 0;
    const char* fontPath = nullptr;
    std::size_t fontPathByteCount = 0;
    core::TransformMatrix transformMatrix{};
    float x = 0.0f;
    float y = 0.0f;
    float fontSize = 16.0f;
    float lineHeight = 0.0f;
    float maxWidth = 0.0f;
    int fontWeight = 400;
    core::HorizontalAlign horizontalAlign = core::HorizontalAlign::Left;
    core::VerticalAlign verticalAlign = core::VerticalAlign::Top;
    bool wrap = false;
    bool hasTransformMatrix = false;
    bool transformed = false;
};

struct TextDrawCommand {
    const float* vertices = nullptr;
    std::size_t vertexFloatCount = 0;
    core::Color color{};
    TextAtlasPageData grayAtlas{};
    TextAtlasPageData colorAtlas{};
    TextSourceDrawData source{};
};]=])
    string(FIND "${eui_render_backend_content}" "struct TextSourceDrawData" source_data_pos)
    string(FIND "${eui_render_backend_content}" "core::TransformMatrix transformMatrix{};" source_matrix_pos)
    if(source_data_pos EQUAL -1)
        string(FIND "${eui_render_backend_content}" "${eui_text_command_anchor}" text_command_pos)
        if(text_command_pos EQUAL -1)
            message(WARNING "EUI-NEO TextDrawCommand block changed upstream; Skia text source patch was not applied")
            return()
        endif()
        string(REPLACE "${eui_text_command_anchor}"
                       "${eui_text_command_with_source}"
                       eui_render_backend_content
                       "${eui_render_backend_content}")
        file(WRITE "${eui_render_backend_header}" "${eui_render_backend_content}")
    elseif(source_matrix_pos EQUAL -1)
        set(eui_text_command_old_source [=[struct TextSourceDrawData {
    const char* text = nullptr;
    std::size_t textByteCount = 0;
    const char* fontPath = nullptr;
    std::size_t fontPathByteCount = 0;
    float x = 0.0f;
    float y = 0.0f;
    float fontSize = 16.0f;
    float lineHeight = 0.0f;
    float maxWidth = 0.0f;
    int fontWeight = 400;
    core::HorizontalAlign horizontalAlign = core::HorizontalAlign::Left;
    core::VerticalAlign verticalAlign = core::VerticalAlign::Top;
    bool wrap = false;
    bool transformed = false;
};

struct TextDrawCommand {
    const float* vertices = nullptr;
    std::size_t vertexFloatCount = 0;
    core::Color color{};
    TextAtlasPageData grayAtlas{};
    TextAtlasPageData colorAtlas{};
    TextSourceDrawData source{};
};]=])
        string(FIND "${eui_render_backend_content}" "${eui_text_command_old_source}" text_command_old_pos)
        if(text_command_old_pos EQUAL -1)
            message(WARNING "EUI-NEO TextSourceDrawData block changed upstream; Skia text source matrix was not applied")
            return()
        endif()
        string(REPLACE "${eui_text_command_old_source}"
                       "${eui_text_command_with_source}"
                       eui_render_backend_content
                       "${eui_render_backend_content}")
        file(WRITE "${eui_render_backend_header}" "${eui_render_backend_content}")
    endif()

    file(READ "${eui_text_source}" eui_text_content)
    set(eui_source_helper_anchor [=[bool nextCodepointIsEmojiPresentation(const std::string& text, size_t index) {]=])
    set(eui_source_helper_block [=[bool isIdentityTextMatrix(const TransformMatrix& matrix) {
    auto close = [](float left, float right) {
        return std::fabs(left - right) <= 0.0001f;
    };
    return close(matrix.m00, 1.0f) &&
           close(matrix.m01, 0.0f) &&
           close(matrix.tx, 0.0f) &&
           close(matrix.m10, 0.0f) &&
           close(matrix.m11, 1.0f) &&
           close(matrix.ty, 0.0f) &&
           close(matrix.px, 0.0f) &&
           close(matrix.py, 0.0f) &&
           close(matrix.pw, 1.0f);
}

]=])
    string(FIND "${eui_text_content}" "bool isIdentityTextMatrix(const TransformMatrix& matrix)" helper_pos)
    if(helper_pos EQUAL -1)
        string(FIND "${eui_text_content}" "${eui_source_helper_anchor}" helper_anchor_pos)
        if(helper_anchor_pos EQUAL -1)
            message(WARNING "EUI-NEO text helper anchor changed upstream; Skia text source helper was not applied")
            return()
        endif()
        string(REPLACE "${eui_source_helper_anchor}"
                       "${eui_source_helper_block}${eui_source_helper_anchor}"
                       eui_text_content
                       "${eui_text_content}")
    endif()

    set(eui_command_without_source [=[    command.color = style_.color;
    command.grayAtlas = {]=])
    set(eui_command_with_source [=[    command.color = style_.color;
    const std::string sourceFontPath = resolveFontPath(style_.fontFamily, style_.fontWeight);
    command.source.text = style_.text.c_str();
    command.source.textByteCount = style_.text.size();
    command.source.fontPath = sourceFontPath.c_str();
    command.source.fontPathByteCount = sourceFontPath.size();
    command.source.transformMatrix = transformMatrix_;
    command.source.x = position_.x;
    command.source.y = position_.y;
    command.source.fontSize = style_.fontSize;
    command.source.lineHeight = style_.lineHeight;
    command.source.maxWidth = style_.maxWidth;
    command.source.fontWeight = style_.fontWeight;
    command.source.horizontalAlign = style_.horizontalAlign;
    command.source.verticalAlign = style_.verticalAlign;
    command.source.wrap = style_.wrap;
    command.source.hasTransformMatrix = hasTransformMatrix_;
    command.source.transformed = hasTransformMatrix_ && !isIdentityTextMatrix(transformMatrix_);
    command.grayAtlas = {]=])
    string(FIND "${eui_text_content}" "command.source.text = style_.text.c_str();" command_source_pos)
    string(FIND "${eui_text_content}" "command.source.transformMatrix = transformMatrix_;" command_matrix_pos)
    if(command_source_pos EQUAL -1)
        string(FIND "${eui_text_content}" "${eui_command_without_source}" command_anchor_pos)
        if(command_anchor_pos EQUAL -1)
            message(WARNING "EUI-NEO text render command block changed upstream; Skia text source patch was not applied")
            return()
        endif()
        string(REPLACE "${eui_command_without_source}"
                       "${eui_command_with_source}"
                       eui_text_content
                       "${eui_text_content}")
        file(WRITE "${eui_text_source}" "${eui_text_content}")
    elseif(command_matrix_pos EQUAL -1)
        set(eui_command_old_source [=[    command.color = style_.color;
    const std::string sourceFontPath = resolveFontPath(style_.fontFamily, style_.fontWeight);
    command.source.text = style_.text.c_str();
    command.source.textByteCount = style_.text.size();
    command.source.fontPath = sourceFontPath.c_str();
    command.source.fontPathByteCount = sourceFontPath.size();
    command.source.x = position_.x;
    command.source.y = position_.y;
    command.source.fontSize = style_.fontSize;
    command.source.lineHeight = style_.lineHeight;
    command.source.maxWidth = style_.maxWidth;
    command.source.fontWeight = style_.fontWeight;
    command.source.horizontalAlign = style_.horizontalAlign;
    command.source.verticalAlign = style_.verticalAlign;
    command.source.wrap = style_.wrap;
    command.source.transformed = hasTransformMatrix_ && !isIdentityTextMatrix(transformMatrix_);
    command.grayAtlas = {]=])
        string(FIND "${eui_text_content}" "${eui_command_old_source}" command_old_source_pos)
        if(command_old_source_pos EQUAL -1)
            message(WARNING "EUI-NEO text source block changed upstream; Skia text source matrix was not applied")
            return()
        endif()
        string(REPLACE "${eui_command_old_source}"
                       "${eui_command_with_source}"
                       eui_text_content
                       "${eui_text_content}")
        file(WRITE "${eui_text_source}" "${eui_text_content}")
    endif()

    message(STATUS "Patched EUI-NEO TextDrawCommand with raw text source data for Skia")
endfunction()

