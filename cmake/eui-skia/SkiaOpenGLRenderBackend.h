#pragma once

#include "core/render/opengl/opengl_backend.h"

#include <cstdint>
#include <string>
#include <unordered_map>

#include "include/core/SkRefCnt.h"

class GrDirectContext;
class SkFontMgr;
class SkImage;
class SkSurface;
class SkTypeface;

namespace core::render::skia {

class SkiaOpenGLRenderBackend final : public RenderBackend {
public:
    explicit SkiaOpenGLRenderBackend(core::window::Handle window,
                                     RenderBackend* shareContext = nullptr);
    ~SkiaOpenGLRenderBackend() override;

    SkiaOpenGLRenderBackend(const SkiaOpenGLRenderBackend&) = delete;
    SkiaOpenGLRenderBackend& operator=(const SkiaOpenGLRenderBackend&) = delete;

    bool initialize() override;
    bool valid() const override;

    void makeCurrent() override;
    void beginFrame(const RenderSurface& surface) override;
    void present() override;
    bool ensureRenderCache(int width, int height) override;
    bool renderCacheWasRecreated() const override;
    void releaseRenderCache() override;
    void beginRenderCacheFrame(int width, int height) override;
    void endRenderCacheFrame() override;
    void blitRenderCache(int width, int height) override;
    void clear(const core::Color& color) override;
    void setScissor(bool enabled, const core::Rect& rect, int framebufferHeight) override;
    void prepareBackdropBlur(const core::Rect& bounds,
                             float blur,
                             int windowWidth,
                             int windowHeight) override;
    void drawRoundedRect(const RoundedRectDrawCommand& command,
                         int windowWidth,
                         int windowHeight) override;
    void drawText(const TextDrawCommand& command, int windowWidth, int windowHeight) override;
    TextureHandle createTexture(const unsigned char* pixels, int width, int height) override;
    bool updateTexture(TextureHandle handle, const unsigned char* pixels, int width, int height) override;
    void destroyTexture(TextureHandle handle) override;
    void drawTexture(TextureHandle handle,
                     const float* vertices,
                     std::size_t vertexFloatCount,
                     const core::Color& tint,
                     const core::Rect& rect,
                     float radius,
                     int windowWidth,
                     int windowHeight) override;

private:
    SkSurface* currentSurface(int width, int height);
    void discardSurface();
    void flushSkia();
    void prepareFallbackOpenGL();
    bool shouldFallbackRoundedRect(const RoundedRectDrawCommand& command) const;
    bool drawDirectSkiaText(const TextDrawCommand& command, int windowWidth, int windowHeight);
    bool drawSkiaText(const TextDrawCommand& command, int windowWidth, int windowHeight);
    sk_sp<SkImage> ensureTextAtlasImage(const TextAtlasPageData& page);
    sk_sp<SkTypeface> typefaceForTextSource(const TextSourceDrawData& source);

    opengl::OpenGLRenderBackend fallback_;
    sk_sp<GrDirectContext> context_;
    sk_sp<SkSurface> surface_;
    sk_sp<SkFontMgr> fontMgr_;
    sk_sp<SkImage> grayTextAtlas_;
    sk_sp<SkImage> colorTextAtlas_;
    std::unordered_map<std::string, sk_sp<SkTypeface>> textTypefaceCache_;
    RenderSurface currentRenderSurface_;
    Rect scissorRect_{};
    bool scissorEnabled_ = false;
    int activeFramebufferWidth_ = 0;
    int activeFramebufferHeight_ = 0;
    int surfaceFramebuffer_ = -1;
    int surfaceWidth_ = 0;
    int surfaceHeight_ = 0;
    std::uint64_t grayTextAtlasGeneration_ = 0;
    std::uint64_t colorTextAtlasGeneration_ = 0;
    int grayTextAtlasWidth_ = 0;
    int grayTextAtlasHeight_ = 0;
    int grayTextAtlasChannels_ = 0;
    int colorTextAtlasWidth_ = 0;
    int colorTextAtlasHeight_ = 0;
    int colorTextAtlasChannels_ = 0;
};

} // namespace core::render::skia

