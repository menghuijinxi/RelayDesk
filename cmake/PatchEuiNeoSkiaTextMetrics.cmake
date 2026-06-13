function(relaydesk_patch_eui_neo_skia_text_metrics eui_source_dir)
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

    set(eui_text_source "${eui_source_dir}/core/render/text.cpp")
    if(NOT EXISTS "${eui_text_source}")
        message(WARNING "EUI-NEO text metrics patch target was not found")
        return()
    endif()

    file(READ "${eui_text_source}" eui_text_content)

    set(eui_render_include_anchor [=[#include "core/render/text.h"
#include "core/render/render_backend.h"

#include <ft2build.h>]=])
    set(eui_render_include_with_skia [=[#include "core/render/text.h"
#include "core/render/render_backend.h"

#if defined(RELAYDESK_EUI_SKIA_BACKEND)
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTypeface.h"
#ifdef _WIN32
#include "include/ports/SkTypeface_win.h"
#endif
#endif

#include <ft2build.h>]=])
    string(FIND "${eui_text_content}" "#include \"include/core/SkFont.h\"" skia_include_pos)
    if(skia_include_pos EQUAL -1)
        string(FIND "${eui_text_content}" "${eui_render_include_anchor}" include_anchor_pos)
        if(include_anchor_pos EQUAL -1)
            message(WARNING "EUI-NEO text include block changed upstream; Skia text metrics patch was not applied")
            return()
        endif()
        string(REPLACE "${eui_render_include_anchor}"
                       "${eui_render_include_with_skia}"
                       eui_text_content
                       "${eui_text_content}")
    endif()

    set(eui_read_utf8_anchor [=[unsigned int readUtf8Codepoint(const std::string& text, size_t& index) {
    const unsigned char first = static_cast<unsigned char>(text[index++]);
    if (first < 0x80) {
        return first;
    }
    if ((first >> 5) == 0x6 && index < text.size()) {
        return ((first & 0x1F) << 6) | (static_cast<unsigned char>(text[index++]) & 0x3F);
    }
    if ((first >> 4) == 0xE && index + 1 < text.size()) {
        unsigned int cp = (first & 0x0F) << 12;
        cp |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 6;
        cp |= static_cast<unsigned char>(text[index++]) & 0x3F;
        return cp;
    }
    if ((first >> 3) == 0x1E && index + 2 < text.size()) {
        unsigned int cp = (first & 0x07) << 18;
        cp |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 12;
        cp |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 6;
        cp |= static_cast<unsigned char>(text[index++]) & 0x3F;
        return cp;
    }
    return '?';
}

]=])
    set(eui_skia_metrics_helper [=[#if defined(RELAYDESK_EUI_SKIA_BACKEND)
int skiaMetricsFontWeight(int fontWeight) {
    return std::clamp(fontWeight,
                      static_cast<int>(SkFontStyle::kThin_Weight),
                      static_cast<int>(SkFontStyle::kBlack_Weight));
}

sk_sp<SkFontMgr> sharedSkiaMetricsFontMgr() {
#ifdef _WIN32
    static sk_sp<SkFontMgr> fontMgr = SkFontMgr_New_DirectWrite();
#else
    static sk_sp<SkFontMgr> fontMgr = SkFontMgr::RefDefault();
#endif
    return fontMgr;
}

sk_sp<SkTypeface> skiaMetricsTypeface(const std::string& fontPath, int fontWeight) {
    sk_sp<SkFontMgr> fontMgr = sharedSkiaMetricsFontMgr();
    if (fontMgr == nullptr) {
        return nullptr;
    }

    if (!fontPath.empty()) {
        static std::unordered_map<std::string, sk_sp<SkTypeface>> typefaceCache;
        const auto cached = typefaceCache.find(fontPath);
        if (cached != typefaceCache.end()) {
            return cached->second;
        }

        sk_sp<SkTypeface> typeface = fontMgr->makeFromFile(fontPath.c_str());
        if (typeface != nullptr) {
            typefaceCache.emplace(fontPath, typeface);
            return typeface;
        }
    }

    const SkFontStyle style(skiaMetricsFontWeight(fontWeight),
                            SkFontStyle::kNormal_Width,
                            SkFontStyle::kUpright_Slant);
    sk_sp<SkTypeface> typeface = fontMgr->matchFamilyStyle("Microsoft YaHei", style);
    if (typeface == nullptr) {
        typeface = fontMgr->matchFamilyStyle(nullptr, style);
    }
    return typeface;
}

bool skiaMetricsTypefaceContainsText(SkTypeface& typeface, const std::string& text) {
    const std::size_t glyphCount = typeface.textToGlyphs(
        text.data(),
        text.size(),
        SkTextEncoding::kUTF8,
        SkSpan<SkGlyphID>());
    if (glyphCount <= 0) {
        return false;
    }

    std::vector<SkGlyphID> glyphs(glyphCount);
    const std::size_t mappedGlyphCount = typeface.textToGlyphs(
        text.data(),
        text.size(),
        SkTextEncoding::kUTF8,
        SkSpan<SkGlyphID>(glyphs.data(), glyphs.size()));
    if (mappedGlyphCount != glyphCount) {
        return false;
    }

    return std::all_of(glyphs.begin(), glyphs.end(), [](SkGlyphID glyph) {
        return glyph != 0;
    });
}

SkFont skiaMetricsFont(sk_sp<SkTypeface> typeface, float fontSize) {
    SkFont font(typeface, fontSize);
    font.setSubpixel(true);
    font.setLinearMetrics(true);
    font.setEmbeddedBitmaps(false);
    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    font.setHinting(SkFontHinting::kNormal);
    return font;
}

bool hasLineBreak(const std::string& text) {
    return text.find('\n') != std::string::npos ||
           text.find('\r') != std::string::npos;
}

std::vector<int> utf8CaretStops(const std::string& text) {
    std::vector<int> stops;
    stops.push_back(0);
    size_t index = 0;
    while (index < text.size()) {
        const size_t previous = index;
        (void)readUtf8Codepoint(text, index);
        if (index <= previous) {
            index = previous + 1;
        }
        stops.push_back(static_cast<int>(std::min(index, text.size())));
    }
    if (stops.back() != static_cast<int>(text.size())) {
        stops.push_back(static_cast<int>(text.size()));
    }
    return stops;
}

bool measureTextMetricsWithSkia(const std::string& text,
                                const std::string& fontPath,
                                float fontSize,
                                int fontWeight,
                                TextPrimitive::TextMetrics& metrics) {
    if (hasLineBreak(text)) {
        return false;
    }

    sk_sp<SkTypeface> typeface = skiaMetricsTypeface(fontPath, fontWeight);
    if (typeface == nullptr || !skiaMetricsTypefaceContainsText(*typeface, text)) {
        return false;
    }

    const SkFont font = skiaMetricsFont(typeface, fontSize);
    metrics.byteIndices.clear();
    metrics.caretX.clear();

    const std::vector<int> stops = utf8CaretStops(text);
    metrics.byteIndices.reserve(stops.size());
    metrics.caretX.reserve(stops.size());
    for (int stop : stops) {
        const size_t byteCount = static_cast<size_t>(std::clamp(stop, 0, static_cast<int>(text.size())));
        const float width = byteCount == 0
            ? 0.0f
            : font.measureText(text.data(), byteCount, SkTextEncoding::kUTF8);
        metrics.byteIndices.push_back(static_cast<int>(byteCount));
        metrics.caretX.push_back(width);
    }

    metrics.width = metrics.caretX.empty() ? 0.0f : metrics.caretX.back();
    return true;
}
#endif

]=])
    string(FIND "${eui_text_content}" "bool measureTextMetricsWithSkia(" skia_metrics_helper_pos)
    if(skia_metrics_helper_pos EQUAL -1)
        string(FIND "${eui_text_content}" "${eui_read_utf8_anchor}" read_utf8_anchor_pos)
        if(read_utf8_anchor_pos EQUAL -1)
            message(WARNING "EUI-NEO UTF-8 helper block changed upstream; Skia text metrics patch was not applied")
            return()
        endif()
        string(REPLACE "${eui_read_utf8_anchor}"
                       "${eui_read_utf8_anchor}${eui_skia_metrics_helper}"
                       eui_text_content
                       "${eui_text_content}")
    endif()

    set(eui_old_metrics_block [=[    const float size = std::max(1.0f, fontSize);
    const std::string fontPath = resolveFontPath(fontFamily, fontWeight);
    auto holder = loadSharedFontStack(fontPath, size);
    if (!holder || holder->faces.empty()) {
        return empty;
    }

    return makeTextMetrics(text, shapeTextWithFontStack(*holder, text, size));]=])
    set(eui_new_metrics_block [=[    const float size = std::max(1.0f, fontSize);
    const std::string fontPath = resolveFontPath(fontFamily, fontWeight);
#if defined(RELAYDESK_EUI_SKIA_BACKEND)
    TextMetrics skiaMetrics;
    if (measureTextMetricsWithSkia(text, fontPath, size, fontWeight, skiaMetrics)) {
        return skiaMetrics;
    }
#endif
    auto holder = loadSharedFontStack(fontPath, size);
    if (!holder || holder->faces.empty()) {
        return empty;
    }

    return makeTextMetrics(text, shapeTextWithFontStack(*holder, text, size));]=])
    string(FIND "${eui_text_content}" "measureTextMetricsWithSkia(text, fontPath" skia_metrics_call_pos)
    if(skia_metrics_call_pos EQUAL -1)
        string(FIND "${eui_text_content}" "${eui_old_metrics_block}" metrics_block_pos)
        if(metrics_block_pos EQUAL -1)
            message(WARNING "EUI-NEO measureTextMetrics block changed upstream; Skia text metrics patch was not applied")
            return()
        endif()
        string(REPLACE "${eui_old_metrics_block}"
                       "${eui_new_metrics_block}"
                       eui_text_content
                       "${eui_text_content}")
    endif()

    file(WRITE "${eui_text_source}" "${eui_text_content}")
    message(STATUS "Patched EUI-NEO text metrics to use Skia when the Skia backend is enabled")
endfunction()

