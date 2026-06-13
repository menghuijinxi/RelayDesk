#include "SkiaOpenGLRenderBackend.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <glad/glad.h>

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSpan.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSurfaceProps.h"
#include "include/core/SkTypeface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/ports/SkTypeface_win.h"

namespace core::render::skia {
namespace {

SkScalar toScalar(float value) {
    return static_cast<SkScalar>(value);
}

SkRect toSkRect(const Rect& rect) {
    return SkRect::MakeXYWH(toScalar(rect.x),
                            toScalar(rect.y),
                            toScalar(rect.width),
                            toScalar(rect.height));
}

SkColor4f toSkColor(const Color& color, float opacity = 1.0f) {
    return {
        std::clamp(color.r, 0.0f, 1.0f),
        std::clamp(color.g, 0.0f, 1.0f),
        std::clamp(color.b, 0.0f, 1.0f),
        std::clamp(color.a * opacity, 0.0f, 1.0f)
    };
}

bool hasVisibleAlpha(const Color& color, float opacity) {
    return color.a * opacity > 0.001f;
}

bool hasPerspectiveVertices(const RoundedRectDrawCommand& command) {
    return std::any_of(command.vertices.begin(),
                       command.vertices.end(),
                       [](const PrimitiveGeometryVertex& vertex) {
                           return std::fabs(vertex.screen.z - 1.0f) > 0.0001f;
                       });
}

bool finiteVertex(const PrimitiveGeometryVertex& vertex) {
    return std::isfinite(vertex.screen.x) &&
           std::isfinite(vertex.screen.y) &&
           std::isfinite(vertex.screen.z) &&
           std::isfinite(vertex.local.x) &&
           std::isfinite(vertex.local.y);
}

bool localToScreenMatrix(const RoundedRectDrawCommand& command, SkMatrix& matrix) {
    constexpr float kGeometryTolerance = 0.25f;
    if (command.vertices.size() < 6 || hasPerspectiveVertices(command)) {
        return false;
    }
    if (!std::all_of(command.vertices.begin(), command.vertices.end(), finiteVertex)) {
        return false;
    }

    const PrimitiveGeometryVertex& topLeft = command.vertices[0];
    const PrimitiveGeometryVertex& topRight = command.vertices[1];
    const PrimitiveGeometryVertex& bottomLeft = command.vertices[5];
    const float localWidth = topRight.local.x - topLeft.local.x;
    const float localHeight = bottomLeft.local.y - topLeft.local.y;
    if (std::fabs(localWidth) <= 0.0001f ||
        std::fabs(localHeight) <= 0.0001f) {
        return false;
    }

    const float m00 = (topRight.screen.x - topLeft.screen.x) / localWidth;
    const float m10 = (topRight.screen.y - topLeft.screen.y) / localWidth;
    const float m01 = (bottomLeft.screen.x - topLeft.screen.x) / localHeight;
    const float m11 = (bottomLeft.screen.y - topLeft.screen.y) / localHeight;
    const float tx = topLeft.screen.x - m00 * topLeft.local.x - m01 * topLeft.local.y;
    const float ty = topLeft.screen.y - m10 * topLeft.local.x - m11 * topLeft.local.y;

    const auto matchesMatrix = [&](const PrimitiveGeometryVertex& vertex) {
        const float screenX = m00 * vertex.local.x + m01 * vertex.local.y + tx;
        const float screenY = m10 * vertex.local.x + m11 * vertex.local.y + ty;
        return std::fabs(screenX - vertex.screen.x) <= kGeometryTolerance &&
               std::fabs(screenY - vertex.screen.y) <= kGeometryTolerance;
    };
    if (!std::all_of(command.vertices.begin(), command.vertices.end(), matchesMatrix)) {
        return false;
    }

    matrix.setAll(toScalar(m00),
                  toScalar(m01),
                  toScalar(tx),
                  toScalar(m10),
                  toScalar(m11),
                  toScalar(ty),
                  0.0f,
                  0.0f,
                  1.0f);
    return true;
}

constexpr std::size_t kTextVertexStride = 5;
constexpr std::size_t kTextVerticesPerGlyph = 6;
constexpr std::size_t kTextGlyphFloatCount = kTextVertexStride * kTextVerticesPerGlyph;
constexpr float kTextQuadTolerance = 0.001f;
constexpr float kTextTransformTolerance = 0.0001f;

struct TextGlyphQuad {
    SkRect destination{};
    SkRect source{};
    bool colored = false;
};

struct DirectTextLine {
    const char* text = nullptr;
    std::size_t byteCount = 0;
    SkScalar width = 0.0f;
};

bool nearlyEqual(float left, float right) {
    return std::fabs(left - right) <= kTextQuadTolerance;
}

bool validTextAtlasPage(const TextAtlasPageData& page) {
    return page.pixels != nullptr &&
           page.width > 0 &&
           page.height > 0 &&
           (page.channels == 1 || page.channels == 4);
}

bool hasRenderableTextPage(const TextAtlasPageData& page, bool colored) {
    return validTextAtlasPage(page) && page.channels == (colored ? 4 : 1);
}

bool hasPerspectiveTextTransform(const TextSourceDrawData& source) {
    if (!source.hasTransformMatrix) {
        return false;
    }
    const core::TransformMatrix& matrix = source.transformMatrix;
    return std::fabs(matrix.px) > kTextTransformTolerance ||
           std::fabs(matrix.py) > kTextTransformTolerance ||
           std::fabs(matrix.pw - 1.0f) > kTextTransformTolerance;
}

bool hasDirectTextSource(const TextSourceDrawData& source) {
    return source.text != nullptr &&
           source.textByteCount > 0 &&
           source.fontSize > 0.0f &&
           (!source.transformed || source.hasTransformMatrix) &&
           !hasPerspectiveTextTransform(source);
}

int skiaFontWeight(int fontWeight) {
    return std::clamp(fontWeight,
                      static_cast<int>(SkFontStyle::kThin_Weight),
                      static_cast<int>(SkFontStyle::kBlack_Weight));
}

std::string textSourceFontPath(const TextSourceDrawData& source) {
    if (source.fontPath == nullptr || source.fontPathByteCount == 0) {
        return {};
    }
    return {source.fontPath, source.fontPathByteCount};
}

SkMatrix toSkMatrix(const core::TransformMatrix& matrix) {
    SkMatrix skMatrix;
    skMatrix.setAll(toScalar(matrix.m00),
                    toScalar(matrix.m01),
                    toScalar(matrix.tx),
                    toScalar(matrix.m10),
                    toScalar(matrix.m11),
                    toScalar(matrix.ty),
                    toScalar(matrix.px),
                    toScalar(matrix.py),
                    toScalar(matrix.pw));
    return skMatrix;
}

bool typefaceContainsText(SkTypeface& typeface, const TextSourceDrawData& source) {
    auto containsRange = [&](const char* text, std::size_t byteCount) {
        if (byteCount == 0) {
            return true;
        }

        const std::size_t glyphCount = typeface.textToGlyphs(
            text,
            byteCount,
            SkTextEncoding::kUTF8,
            SkSpan<SkGlyphID>());
        if (glyphCount <= 0) {
            return false;
        }

        std::vector<SkGlyphID> glyphs(glyphCount);
        const std::size_t mappedGlyphCount = typeface.textToGlyphs(
            text,
            byteCount,
            SkTextEncoding::kUTF8,
            SkSpan<SkGlyphID>(glyphs.data(), glyphs.size()));
        if (mappedGlyphCount != glyphCount) {
            return false;
        }

        return std::all_of(glyphs.begin(), glyphs.end(), [](SkGlyphID glyph) {
            return glyph != 0;
        });
    };

    std::size_t rangeStart = 0;
    while (rangeStart <= source.textByteCount) {
        std::size_t rangeEnd = rangeStart;
        while (rangeEnd < source.textByteCount &&
               source.text[rangeEnd] != '\n' &&
               source.text[rangeEnd] != '\r') {
            ++rangeEnd;
        }
        if (!containsRange(source.text + rangeStart, rangeEnd - rangeStart)) {
            return false;
        }
        if (rangeEnd >= source.textByteCount) {
            return true;
        }
        if (source.text[rangeEnd] == '\r' &&
            rangeEnd + 1 < source.textByteCount &&
            source.text[rangeEnd + 1] == '\n') {
            rangeStart = rangeEnd + 2;
        } else {
            rangeStart = rangeEnd + 1;
        }
    }

    return true;
}

std::size_t utf8CodepointByteCount(const char* text,
                                   std::size_t byteCount,
                                   std::size_t offset) {
    if (offset >= byteCount) {
        return 0;
    }

    const unsigned char first = static_cast<unsigned char>(text[offset]);
    std::size_t expected = 1;
    if ((first >> 5) == 0x6) {
        expected = 2;
    } else if ((first >> 4) == 0xE) {
        expected = 3;
    } else if ((first >> 3) == 0x1E) {
        expected = 4;
    }
    return offset + expected <= byteCount ? expected : 1;
}

std::vector<std::size_t> utf8CaretStops(const char* text, std::size_t byteCount) {
    std::vector<std::size_t> stops;
    stops.push_back(0);

    std::size_t offset = 0;
    while (offset < byteCount) {
        const std::size_t step = utf8CodepointByteCount(text, byteCount, offset);
        offset += std::max<std::size_t>(step, 1);
        stops.push_back(std::min(offset, byteCount));
    }
    if (stops.back() != byteCount) {
        stops.push_back(byteCount);
    }
    return stops;
}

SkScalar measureDirectText(const SkFont& font, const char* text, std::size_t byteCount) {
    if (byteCount == 0) {
        return 0.0f;
    }
    return font.measureText(text, byteCount, SkTextEncoding::kUTF8);
}

void appendDirectTextLine(std::vector<DirectTextLine>& lines,
                          const SkFont& font,
                          const char* text,
                          std::size_t byteCount) {
    lines.push_back({text, byteCount, measureDirectText(font, text, byteCount)});
}

void appendWrappedDirectTextLines(std::vector<DirectTextLine>& lines,
                                  const SkFont& font,
                                  const char* text,
                                  std::size_t byteCount,
                                  SkScalar maxWidth) {
    if (byteCount == 0 || maxWidth <= 1.0f) {
        appendDirectTextLine(lines, font, text, byteCount);
        return;
    }

    const std::vector<std::size_t> stops = utf8CaretStops(text, byteCount);
    if (stops.size() <= 2) {
        appendDirectTextLine(lines, font, text, byteCount);
        return;
    }

    std::vector<SkScalar> caretX;
    caretX.reserve(stops.size());
    for (std::size_t stop : stops) {
        caretX.push_back(measureDirectText(font, text, stop));
    }
    if (caretX.back() <= maxWidth) {
        appendDirectTextLine(lines, font, text, byteCount);
        return;
    }

    std::size_t segmentStart = 0;
    SkScalar segmentStartX = 0.0f;
    std::size_t previousStop = 0;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        const std::size_t stop = stops[i];
        const SkScalar x = caretX[i];
        if (previousStop > segmentStart && x - segmentStartX > maxWidth) {
            appendDirectTextLine(lines, font, text + segmentStart, previousStop - segmentStart);
            segmentStart = previousStop;
            segmentStartX = measureDirectText(font, text, segmentStart);
        }
        previousStop = stop;
    }

    if (segmentStart < byteCount) {
        appendDirectTextLine(lines, font, text + segmentStart, byteCount - segmentStart);
    }
}

std::vector<DirectTextLine> directTextLines(const TextSourceDrawData& source,
                                            const SkFont& font) {
    std::vector<DirectTextLine> lines;
    const SkScalar maxWidth = source.wrap ? std::max<SkScalar>(0.0f, source.maxWidth) : 0.0f;
    std::size_t lineStart = 0;
    while (lineStart <= source.textByteCount) {
        std::size_t lineEnd = lineStart;
        while (lineEnd < source.textByteCount &&
               source.text[lineEnd] != '\n' &&
               source.text[lineEnd] != '\r') {
            ++lineEnd;
        }
        appendWrappedDirectTextLines(lines,
                                     font,
                                     source.text + lineStart,
                                     lineEnd - lineStart,
                                     maxWidth);
        if (lineEnd >= source.textByteCount) {
            break;
        }
        if (source.text[lineEnd] == '\r' &&
            lineEnd + 1 < source.textByteCount &&
            source.text[lineEnd + 1] == '\n') {
            lineStart = lineEnd + 2;
        } else {
            lineStart = lineEnd + 1;
        }
        if (lineStart == source.textByteCount) {
            appendDirectTextLine(lines, font, source.text + lineStart, 0);
            break;
        }
    }
    return lines;
}

float atlasPixel(float normalizedCoordinate, int atlasSize) {
    return std::clamp(std::round(normalizedCoordinate * static_cast<float>(atlasSize)),
                      0.0f,
                      static_cast<float>(atlasSize));
}

void keepUnscaledGrayGlyph(TextGlyphQuad& quad) {
    if (quad.colored) {
        return;
    }

    const float sourceWidth = quad.source.width();
    const float sourceHeight = quad.source.height();
    const float destinationWidth = quad.destination.width();
    const float destinationHeight = quad.destination.height();
    if (sourceWidth <= 0.0f || sourceHeight <= 0.0f) {
        return;
    }
    if (std::fabs(destinationWidth - sourceWidth) > 1.01f ||
        std::fabs(destinationHeight - sourceHeight) > 1.01f) {
        return;
    }

    quad.destination = SkRect::MakeXYWH(quad.destination.left(),
                                        quad.destination.top(),
                                        sourceWidth,
                                        sourceHeight);
}

bool unpackTextGlyphQuad(const float* vertices,
                         const TextAtlasPageData& page,
                         TextGlyphQuad& quad) {
    for (std::size_t vertexIndex = 0; vertexIndex < kTextVerticesPerGlyph; ++vertexIndex) {
        const std::size_t base = vertexIndex * kTextVertexStride;
        const float x = vertices[base];
        const float y = vertices[base + 1];
        const float u = vertices[base + 2];
        const float v = vertices[base + 3];
        if (!std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(u) || !std::isfinite(v)) {
            return false;
        }
    }

    const float p0X = vertices[0];
    const float p0Y = vertices[1];
    const float p0U = vertices[2];
    const float p0V = vertices[3];
    const float p1X = vertices[5];
    const float p1Y = vertices[6];
    const float p1U = vertices[7];
    const float p1V = vertices[8];
    const float p2X = vertices[10];
    const float p2Y = vertices[11];
    const float p2U = vertices[12];
    const float p2V = vertices[13];
    const float p3X = vertices[25];
    const float p3Y = vertices[26];
    const float p3U = vertices[27];
    const float p3V = vertices[28];

    if (!nearlyEqual(vertices[15], p0X) ||
        !nearlyEqual(vertices[16], p0Y) ||
        !nearlyEqual(vertices[17], p0U) ||
        !nearlyEqual(vertices[18], p0V) ||
        !nearlyEqual(vertices[20], p2X) ||
        !nearlyEqual(vertices[21], p2Y) ||
        !nearlyEqual(vertices[22], p2U) ||
        !nearlyEqual(vertices[23], p2V)) {
        return false;
    }

    const bool axisAligned =
        nearlyEqual(p0Y, p1Y) &&
        nearlyEqual(p2Y, p3Y) &&
        nearlyEqual(p0X, p3X) &&
        nearlyEqual(p1X, p2X);
    const bool uvAxisAligned =
        nearlyEqual(p0V, p1V) &&
        nearlyEqual(p2V, p3V) &&
        nearlyEqual(p0U, p3U) &&
        nearlyEqual(p1U, p2U);
    if (!axisAligned || !uvAxisAligned) {
        return false;
    }

    const float left = std::min(p0X, p1X);
    const float right = std::max(p0X, p1X);
    const float top = std::min(p0Y, p2Y);
    const float bottom = std::max(p0Y, p2Y);
    const float minU = std::clamp(std::min(p0U, p1U), 0.0f, 1.0f);
    const float maxU = std::clamp(std::max(p0U, p1U), 0.0f, 1.0f);
    const float minV = std::clamp(std::min(p0V, p2V), 0.0f, 1.0f);
    const float maxV = std::clamp(std::max(p0V, p2V), 0.0f, 1.0f);
    if (maxU <= minU || maxV <= minV) {
        return false;
    }

    quad.destination = SkRect::MakeLTRB(toScalar(left),
                                        toScalar(top),
                                        toScalar(right),
                                        toScalar(bottom));
    quad.source = SkRect::MakeLTRB(toScalar(atlasPixel(minU, page.width)),
                                   toScalar(atlasPixel(minV, page.height)),
                                   toScalar(atlasPixel(maxU, page.width)),
                                   toScalar(atlasPixel(maxV, page.height)));
    quad.colored = vertices[4] > 0.5f;
    keepUnscaledGrayGlyph(quad);
    return true;
}

} // namespace

SkiaOpenGLRenderBackend::SkiaOpenGLRenderBackend(core::window::Handle window,
                                                 RenderBackend* shareContext)
    : fallback_(window, shareContext) {}

SkiaOpenGLRenderBackend::~SkiaOpenGLRenderBackend() {
    flushSkia();
    surface_.reset();
    context_.reset();
}

bool SkiaOpenGLRenderBackend::initialize() {
    if (!fallback_.initialize()) {
        return false;
    }

    fallback_.makeCurrent();
    context_ = GrDirectContexts::MakeGL();
    fontMgr_ = SkFontMgr_New_DirectWrite();
    return context_ != nullptr;
}

bool SkiaOpenGLRenderBackend::valid() const {
    return fallback_.valid() && context_ != nullptr;
}

void SkiaOpenGLRenderBackend::makeCurrent() {
    fallback_.makeCurrent();
}

void SkiaOpenGLRenderBackend::beginFrame(const RenderSurface& surface) {
    flushSkia();
    currentRenderSurface_ = surface;
    activeFramebufferWidth_ = surface.framebufferWidth;
    activeFramebufferHeight_ = surface.framebufferHeight;
    discardSurface();
    fallback_.beginFrame(surface);
}

void SkiaOpenGLRenderBackend::present() {
    flushSkia();
    fallback_.present();
}

bool SkiaOpenGLRenderBackend::ensureRenderCache(int width, int height) {
    flushSkia();
    return fallback_.ensureRenderCache(width, height);
}

bool SkiaOpenGLRenderBackend::renderCacheWasRecreated() const {
    return fallback_.renderCacheWasRecreated();
}

void SkiaOpenGLRenderBackend::releaseRenderCache() {
    flushSkia();
    fallback_.releaseRenderCache();
    discardSurface();
}

void SkiaOpenGLRenderBackend::beginRenderCacheFrame(int width, int height) {
    flushSkia();
    activeFramebufferWidth_ = width;
    activeFramebufferHeight_ = height;
    discardSurface();
    fallback_.beginRenderCacheFrame(width, height);
}

void SkiaOpenGLRenderBackend::endRenderCacheFrame() {
    flushSkia();
    fallback_.endRenderCacheFrame();
    discardSurface();
    activeFramebufferWidth_ = currentRenderSurface_.framebufferWidth;
    activeFramebufferHeight_ = currentRenderSurface_.framebufferHeight;
}

void SkiaOpenGLRenderBackend::blitRenderCache(int width, int height) {
    flushSkia();
    fallback_.blitRenderCache(width, height);
    discardSurface();
}

void SkiaOpenGLRenderBackend::clear(const core::Color& color) {
    flushSkia();
    prepareFallbackOpenGL();
    fallback_.clear(color);
    discardSurface();
}

void SkiaOpenGLRenderBackend::setScissor(bool enabled,
                                         const core::Rect& rect,
                                         int framebufferHeight) {
    scissorEnabled_ = enabled;
    scissorRect_ = rect;
    fallback_.setScissor(enabled, rect, framebufferHeight);
}

void SkiaOpenGLRenderBackend::prepareBackdropBlur(const core::Rect& bounds,
                                                  float blur,
                                                  int windowWidth,
                                                  int windowHeight) {
    flushSkia();
    prepareFallbackOpenGL();
    fallback_.prepareBackdropBlur(bounds, blur, windowWidth, windowHeight);
}

void SkiaOpenGLRenderBackend::drawRoundedRect(const RoundedRectDrawCommand& command,
                                              int windowWidth,
                                              int windowHeight) {
    SkMatrix localToScreen;
    if (shouldFallbackRoundedRect(command) || !localToScreenMatrix(command, localToScreen)) {
        prepareFallbackOpenGL();
        fallback_.drawRoundedRect(command, windowWidth, windowHeight);
        discardSurface();
        return;
    }

    SkSurface* surface = currentSurface(windowWidth, windowHeight);
    if (surface == nullptr) {
        prepareFallbackOpenGL();
        fallback_.drawRoundedRect(command, windowWidth, windowHeight);
        discardSurface();
        return;
    }

    SkCanvas* canvas = surface->getCanvas();
    canvas->save();
    if (scissorEnabled_) {
        canvas->clipRect(toSkRect(scissorRect_), SkClipOp::kIntersect, true);
    }
    canvas->concat(localToScreen);

    const SkRect rect = toSkRect(command.rect);
    const float radius = std::clamp(command.radius,
                                    0.0f,
                                    std::min(command.rect.width, command.rect.height) * 0.5f);
    SkRRect rrect;
    rrect.setRectXY(rect, toScalar(radius), toScalar(radius));

    if (command.gradient.enabled) {
        const Color fillColor = mixColor(command.gradient.start, command.gradient.end, 0.5f);
        SkPaint fillPaint;
        fillPaint.setAntiAlias(true);
        fillPaint.setColor4f(toSkColor(fillColor, command.opacity), nullptr);
        canvas->drawRRect(rrect, fillPaint);
    } else if (hasVisibleAlpha(command.fillColor, command.opacity)) {
        SkPaint fillPaint;
        fillPaint.setAntiAlias(true);
        fillPaint.setColor4f(toSkColor(command.fillColor, command.opacity), nullptr);
        canvas->drawRRect(rrect, fillPaint);
    }

    if (command.border.width > 0.001f && hasVisibleAlpha(command.border.color, command.opacity)) {
        const float halfStroke = command.border.width * 0.5f;
        const SkRect strokeRect = rect.makeInset(toScalar(halfStroke), toScalar(halfStroke));
        const float strokeRadius = std::max(0.0f, radius - halfStroke);
        SkRRect strokeRRect;
        strokeRRect.setRectXY(strokeRect, toScalar(strokeRadius), toScalar(strokeRadius));

        SkPaint strokePaint;
        strokePaint.setAntiAlias(true);
        strokePaint.setStyle(SkPaint::kStroke_Style);
        strokePaint.setStrokeWidth(toScalar(command.border.width));
        strokePaint.setColor4f(toSkColor(command.border.color, command.opacity), nullptr);
        canvas->drawRRect(strokeRRect, strokePaint);
    }

    canvas->restore();
}

void SkiaOpenGLRenderBackend::drawText(const TextDrawCommand& command,
                                       int windowWidth,
                                       int windowHeight) {
    if (drawDirectSkiaText(command, windowWidth, windowHeight)) {
        return;
    }
    if (drawSkiaText(command, windowWidth, windowHeight)) {
        return;
    }

    prepareFallbackOpenGL();
    fallback_.drawText(command, windowWidth, windowHeight);
    discardSurface();
}

RenderBackend::TextureHandle SkiaOpenGLRenderBackend::createTexture(const unsigned char* pixels,
                                                                    int width,
                                                                    int height) {
    prepareFallbackOpenGL();
    return fallback_.createTexture(pixels, width, height);
}

bool SkiaOpenGLRenderBackend::updateTexture(TextureHandle handle,
                                            const unsigned char* pixels,
                                            int width,
                                            int height) {
    prepareFallbackOpenGL();
    return fallback_.updateTexture(handle, pixels, width, height);
}

void SkiaOpenGLRenderBackend::destroyTexture(TextureHandle handle) {
    prepareFallbackOpenGL();
    fallback_.destroyTexture(handle);
}

void SkiaOpenGLRenderBackend::drawTexture(TextureHandle handle,
                                          const float* vertices,
                                          std::size_t vertexFloatCount,
                                          const core::Color& tint,
                                          const core::Rect& rect,
                                          float radius,
                                          int windowWidth,
                                          int windowHeight) {
    prepareFallbackOpenGL();
    fallback_.drawTexture(handle,
                          vertices,
                          vertexFloatCount,
                          tint,
                          rect,
                          radius,
                          windowWidth,
                          windowHeight);
    discardSurface();
}

SkSurface* SkiaOpenGLRenderBackend::currentSurface(int width, int height) {
    if (context_ == nullptr || width <= 0 || height <= 0) {
        return nullptr;
    }

    GLint framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (surface_ != nullptr &&
        surfaceWidth_ == width &&
        surfaceHeight_ == height &&
        surfaceFramebuffer_ == framebuffer) {
        context_->resetContext();
        return surface_.get();
    }

    flushSkia();

    GrGLFramebufferInfo framebufferInfo;
    framebufferInfo.fFBOID = static_cast<GrGLuint>(framebuffer);
    framebufferInfo.fFormat = GL_RGBA8;

    const GrBackendRenderTarget target =
        GrBackendRenderTargets::MakeGL(width, height, 0, 8, framebufferInfo);
    SkSurfaceProps surfaceProps(0, kUnknown_SkPixelGeometry);
    surface_ = SkSurfaces::WrapBackendRenderTarget(context_.get(),
                                                   target,
                                                   kBottomLeft_GrSurfaceOrigin,
                                                   kRGBA_8888_SkColorType,
                                                   SkColorSpace::MakeSRGB(),
                                                   &surfaceProps);
    surfaceWidth_ = surface_ != nullptr ? width : 0;
    surfaceHeight_ = surface_ != nullptr ? height : 0;
    surfaceFramebuffer_ = surface_ != nullptr ? framebuffer : -1;

    if (surface_ != nullptr) {
        context_->resetContext();
    }
    return surface_.get();
}

void SkiaOpenGLRenderBackend::discardSurface() {
    surface_.reset();
    surfaceWidth_ = 0;
    surfaceHeight_ = 0;
    surfaceFramebuffer_ = -1;
}

void SkiaOpenGLRenderBackend::flushSkia() {
    if (context_ == nullptr) {
        return;
    }

    if (surface_ != nullptr) {
        context_->flush(surface_.get());
    }
    context_->flushAndSubmit();
    context_->resetContext();
}

void SkiaOpenGLRenderBackend::prepareFallbackOpenGL() {
    flushSkia();
    fallback_.makeCurrent();
    const int viewportWidth = std::max(1, activeFramebufferWidth_);
    const int viewportHeight = std::max(1, activeFramebufferHeight_);
    glViewport(0, 0, viewportWidth, viewportHeight);
}

bool SkiaOpenGLRenderBackend::shouldFallbackRoundedRect(
    const RoundedRectDrawCommand& command) const {
    if (command.shadowPass || command.insetShadowPass || command.backdropBlur > 0.001f) {
        return true;
    }
    return hasPerspectiveVertices(command);
}

bool SkiaOpenGLRenderBackend::drawDirectSkiaText(const TextDrawCommand& command,
                                                 int windowWidth,
                                                 int windowHeight) {
    const TextSourceDrawData& source = command.source;
    if (!hasDirectTextSource(source) ||
        !hasVisibleAlpha(command.color, 1.0f) ||
        windowWidth <= 0 ||
        windowHeight <= 0) {
        return false;
    }

    SkSurface* surface = currentSurface(windowWidth, windowHeight);
    if (surface == nullptr) {
        return false;
    }

    sk_sp<SkTypeface> typeface = typefaceForTextSource(source);
    if (typeface == nullptr || !typefaceContainsText(*typeface, source)) {
        return false;
    }

    SkFont font(typeface, toScalar(source.fontSize));
    font.setSubpixel(true);
    font.setLinearMetrics(true);
    font.setEmbeddedBitmaps(false);
    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    font.setHinting(SkFontHinting::kNormal);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setBlendMode(SkBlendMode::kSrcOver);
    paint.setColor4f(toSkColor(command.color), nullptr);

    const std::vector<DirectTextLine> lines = directTextLines(source, font);
    if (lines.empty()) {
        return true;
    }

    SkFontMetrics metrics;
    font.getMetrics(&metrics);

    const SkScalar lineHeight = source.lineHeight > 0.0f
        ? toScalar(source.lineHeight)
        : toScalar(source.fontSize * 1.2f);
    const SkScalar textHeight = lines.size() <= 1
        ? metrics.fDescent - metrics.fAscent
        : (static_cast<SkScalar>(lines.size() - 1) * lineHeight) +
              metrics.fDescent - metrics.fAscent;
    SkScalar firstBaseline = toScalar(source.y);
    if (source.verticalAlign == VerticalAlign::Top) {
        firstBaseline -= metrics.fAscent;
    } else if (source.verticalAlign == VerticalAlign::Center) {
        firstBaseline -= (textHeight * 0.5f) + metrics.fAscent;
    } else if (source.verticalAlign == VerticalAlign::Bottom) {
        firstBaseline -= textHeight - metrics.fDescent;
    }

    SkCanvas* canvas = surface->getCanvas();
    canvas->save();
    if (scissorEnabled_) {
        canvas->clipRect(toSkRect(scissorRect_), SkClipOp::kIntersect, true);
    }
    if (source.hasTransformMatrix) {
        canvas->concat(toSkMatrix(source.transformMatrix));
    }
    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const DirectTextLine& line = lines[lineIndex];
        if (line.byteCount == 0) {
            continue;
        }

        SkScalar x = toScalar(source.x);
        if (source.horizontalAlign == HorizontalAlign::Center) {
            x -= line.width * 0.5f;
        } else if (source.horizontalAlign == HorizontalAlign::Right) {
            x -= line.width;
        }

        canvas->drawSimpleText(line.text,
                               line.byteCount,
                               SkTextEncoding::kUTF8,
                               x,
                               firstBaseline + static_cast<SkScalar>(lineIndex) * lineHeight,
                               font,
                               paint);
    }
    canvas->restore();
    return true;
}

bool SkiaOpenGLRenderBackend::drawSkiaText(const TextDrawCommand& command,
                                           int windowWidth,
                                           int windowHeight) {
    if (command.vertices == nullptr ||
        command.vertexFloatCount == 0 ||
        windowWidth <= 0 ||
        windowHeight <= 0) {
        return true;
    }
    if (command.vertexFloatCount % kTextGlyphFloatCount != 0) {
        return false;
    }

    SkSurface* surface = currentSurface(windowWidth, windowHeight);
    if (surface == nullptr) {
        return false;
    }

    SkCanvas* canvas = surface->getCanvas();
    canvas->save();
    if (scissorEnabled_) {
        canvas->clipRect(toSkRect(scissorRect_), SkClipOp::kIntersect, true);
    }

    const SkSamplingOptions sampling(SkFilterMode::kLinear);
    const std::size_t glyphCount = command.vertexFloatCount / kTextGlyphFloatCount;
    for (std::size_t glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex) {
        const float* glyphVertices = command.vertices + glyphIndex * kTextGlyphFloatCount;
        const bool colored = glyphVertices[4] > 0.5f;
        const TextAtlasPageData& page = colored ? command.colorAtlas : command.grayAtlas;
        if (!hasRenderableTextPage(page, colored)) {
            canvas->restore();
            return false;
        }

        TextGlyphQuad quad;
        if (!unpackTextGlyphQuad(glyphVertices, page, quad)) {
            canvas->restore();
            return false;
        }

        sk_sp<SkImage> image = ensureTextAtlasImage(page);
        if (image == nullptr) {
            canvas->restore();
            return false;
        }

        SkPaint paint;
        paint.setAntiAlias(false);
        paint.setBlendMode(SkBlendMode::kSrcOver);
        if (colored) {
            paint.setAlphaf(std::clamp(command.color.a, 0.0f, 1.0f));
        } else {
            paint.setColor4f(toSkColor(command.color), nullptr);
        }

        canvas->drawImageRect(image.get(),
                              quad.source,
                              quad.destination,
                              sampling,
                              &paint,
                              SkCanvas::kStrict_SrcRectConstraint);
    }

    canvas->restore();
    return true;
}

sk_sp<SkTypeface> SkiaOpenGLRenderBackend::typefaceForTextSource(
    const TextSourceDrawData& source) {
    if (fontMgr_ == nullptr) {
        return nullptr;
    }

    const std::string fontPath = textSourceFontPath(source);
    if (!fontPath.empty()) {
        const auto cached = textTypefaceCache_.find(fontPath);
        if (cached != textTypefaceCache_.end()) {
            return cached->second;
        }

        sk_sp<SkTypeface> typeface = fontMgr_->makeFromFile(fontPath.c_str());
        if (typeface != nullptr) {
            textTypefaceCache_.emplace(fontPath, typeface);
            return typeface;
        }
    }

    const SkFontStyle style(skiaFontWeight(source.fontWeight),
                            SkFontStyle::kNormal_Width,
                            SkFontStyle::kUpright_Slant);
    sk_sp<SkTypeface> typeface = fontMgr_->matchFamilyStyle("Microsoft YaHei", style);
    if (typeface == nullptr) {
        typeface = fontMgr_->matchFamilyStyle(nullptr, style);
    }
    return typeface;
}

sk_sp<SkImage> SkiaOpenGLRenderBackend::ensureTextAtlasImage(const TextAtlasPageData& page) {
    if (!validTextAtlasPage(page)) {
        return nullptr;
    }

    sk_sp<SkImage>* cachedImage = page.kind == TextAtlasPageKind::Color
        ? &colorTextAtlas_
        : &grayTextAtlas_;
    std::uint64_t* cachedGeneration = page.kind == TextAtlasPageKind::Color
        ? &colorTextAtlasGeneration_
        : &grayTextAtlasGeneration_;
    int* cachedWidth = page.kind == TextAtlasPageKind::Color
        ? &colorTextAtlasWidth_
        : &grayTextAtlasWidth_;
    int* cachedHeight = page.kind == TextAtlasPageKind::Color
        ? &colorTextAtlasHeight_
        : &grayTextAtlasHeight_;
    int* cachedChannels = page.kind == TextAtlasPageKind::Color
        ? &colorTextAtlasChannels_
        : &grayTextAtlasChannels_;

    if (*cachedImage != nullptr &&
        *cachedGeneration == page.generation &&
        *cachedWidth == page.width &&
        *cachedHeight == page.height &&
        *cachedChannels == page.channels) {
        return *cachedImage;
    }

    const auto rowBytes = static_cast<std::size_t>(page.width) *
                          static_cast<std::size_t>(page.channels);
    const auto byteCount = rowBytes * static_cast<std::size_t>(page.height);
    sk_sp<SkData> pixels = SkData::MakeWithCopy(page.pixels, byteCount);
    if (pixels == nullptr) {
        return nullptr;
    }

    const SkImageInfo info = page.channels == 1
        ? SkImageInfo::MakeA8(page.width, page.height)
        : SkImageInfo::Make(page.width,
                            page.height,
                            kRGBA_8888_SkColorType,
                            kUnpremul_SkAlphaType,
                            SkColorSpace::MakeSRGB());

    *cachedImage = SkImages::RasterFromData(info, std::move(pixels), rowBytes);
    if (*cachedImage == nullptr) {
        *cachedGeneration = 0;
        *cachedWidth = 0;
        *cachedHeight = 0;
        *cachedChannels = 0;
        return nullptr;
    }

    *cachedGeneration = page.generation;
    *cachedWidth = page.width;
    *cachedHeight = page.height;
    *cachedChannels = page.channels;
    return *cachedImage;
}

} // namespace core::render::skia

