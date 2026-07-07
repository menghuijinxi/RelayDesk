#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkPngEncoder.h"
#include "skui_win32_app.h"

#include "core/platform/async.h"

namespace {

constexpr SkColor kDemoClearColor = SkColorSetRGB(248, 250, 252);
constexpr std::string_view kDevices[] = {
    "device-alex",
    "device-desktop",
    "device-laptop",
    "device-server",
    "device-mark",
};
constexpr std::string_view kTabs[] = {
    "tab-chat",
    "tab-history",
    "tab-transfer",
};
constexpr int kRelayDeskHtmlResourceId = 101;
constexpr int kDefaultCaptureWidth = 1600;
constexpr int kDefaultCaptureHeight = 900;
constexpr float kDefaultCaptureDpiScale = 1.0f;

struct CaptureOptions {
    std::filesystem::path outputPath;
    int width = kDefaultCaptureWidth;
    int height = kDefaultCaptureHeight;
    float dpiScale = kDefaultCaptureDpiScale;
};

COLORREF colorRefFromSkColor(SkColor color)
{
    return RGB(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
}

std::string loadTextResource(int resourceId)
{
    HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return {};
    }

    HRSRC resource =
        FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (resource == nullptr) {
        return {};
    }

    HGLOBAL dataHandle = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    if (dataHandle == nullptr || size == 0) {
        return {};
    }

    const void* data = LockResource(dataHandle);
    if (data == nullptr) {
        return {};
    }

    const auto* text = static_cast<const char*>(data);
    return std::string(text, text + size);
}

void replaceAll(std::string& text,
                std::string_view from,
                std::string_view to)
{
    if (from.empty()) {
        return;
    }

    std::size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

bool removeCssBlock(std::string& text, std::string_view marker)
{
    const std::size_t blockStart = text.find(marker);
    if (blockStart == std::string::npos) {
        return false;
    }

    const std::size_t openBrace = text.find('{', blockStart);
    if (openBrace == std::string::npos) {
        return false;
    }

    int depth = 0;
    for (std::size_t position = openBrace; position < text.size(); ++position) {
        if (text[position] == '{') {
            ++depth;
        } else if (text[position] == '}') {
            --depth;
            if (depth == 0) {
                text.erase(blockStart, position - blockStart + 1);
                return true;
            }
        }
    }

    return false;
}

void keepDesktopLayout(std::string& html)
{
    removeCssBlock(html, "@media (max-width: 1599px)");
    removeCssBlock(html, "@media (max-width: 940px)");
    removeCssBlock(html, "@media (max-width: 720px)");
}

void injectDocumentIconStyles(std::string& html)
{
    constexpr std::string_view marker = "</style>";
    constexpr std::string_view styles = R"(
    .doc-icon {
      border-radius: 3px;
    }
    .doc-icon-pdf {
      background-color: #0aa39e;
    }
    .doc-icon-zip {
      background-color: #ec8d00;
    }
    .doc-fold {
      position: absolute;
      right: 0px;
      top: 0px;
      width: 12px;
      height: 12px;
      background-color: rgba(255,255,255,0.28);
    }
    .mini-icon .doc-fold {
      width: 8px;
      height: 8px;
    }
)";

    const std::size_t position = html.find(marker);
    if (position != std::string::npos) {
        html.insert(position, styles);
    }
}

std::string makeEmbeddedDocument()
{
    std::string html = loadTextResource(kRelayDeskHtmlResourceId);
    if (html.empty()) {
        return {};
    }
    keepDesktopLayout(html);
    injectDocumentIconStyles(html);

    replaceAll(
        html,
        R"(<img class="file-icon" src="icons/file_type_pdf.svg">)",
        R"(<div class="file-icon doc-icon doc-icon-pdf">)"
        R"(<div class="doc-fold"></div></div>)");
    replaceAll(
        html,
        R"(<img class="mini-icon" src="icons/file_type_pdf.svg">)",
        R"(<div class="mini-icon doc-icon doc-icon-pdf">)"
        R"(<div class="doc-fold"></div></div>)");
    replaceAll(
        html,
        R"(<img class="file-icon" src="icons/file_type_zip.svg">)",
        R"(<div class="file-icon doc-icon doc-icon-zip">)"
        R"(<div class="doc-fold"></div></div>)");
    replaceAll(
        html,
        R"(<img class="mini-icon" src="icons/file_type_zip.svg">)",
        R"(<div class="mini-icon doc-icon doc-icon-zip">)"
        R"(<div class="doc-fold"></div></div>)");
    return html;
}

void selectDevice(skui::Runtime& runtime, std::string_view id)
{
    for (std::string_view device : kDevices) {
        runtime.removeClassById(device, "device-selected");
    }
    runtime.addClassById(id, "device-selected");
}

void selectTab(skui::Runtime& runtime, std::string_view id)
{
    for (std::string_view tab : kTabs) {
        runtime.removeClassById(tab, "tab-active");
    }
    runtime.addClassById(id, "tab-active");
}

void installDemoInteractions(skui::Runtime& runtime)
{
    runtime.setElementEventCallback([&runtime](const skui::ElementEvent& event) {
        if (event.type != skui::ElementEventType::Click || event.action.empty()) {
            return;
        }

        constexpr std::string_view devicePrefix = "select-device:";
        constexpr std::string_view tabPrefix = "tab:";
        const std::string_view action(event.action);
        if (action.starts_with(devicePrefix)) {
            selectDevice(runtime, action.substr(devicePrefix.size()));
        } else if (action.starts_with(tabPrefix)) {
            selectTab(runtime, action.substr(tabPrefix.size()));
        } else if (action == "send-message") {
            runtime.setAttributeById("composer", "value", "");
            runtime.setAttributeById(
                "composer",
                "placeholder",
                "消息已发送，可以继续输入...");
        } else if (action == "finish-transfer") {
            runtime.setAttributeById("upload-progress", "value", "100");
            runtime.setAttributeById("download-progress", "value", "100");
            runtime.setAttributeById(
                "upload-percent",
                "class",
                "transfer-percent done");
            runtime.setAttributeById(
                "download-percent",
                "class",
                "transfer-percent done");
        }
    });
}

bool parsePositiveInt(const wchar_t* text, int& value)
{
    if (text == nullptr || text[0] == L'\0') {
        return false;
    }

    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text, &end, 10);
    if (end == text || *end != L'\0' || parsed <= 0) {
        return false;
    }
    if (parsed > std::numeric_limits<int>::max()) {
        return false;
    }

    value = static_cast<int>(parsed);
    return true;
}

bool parsePositiveFloat(const wchar_t* text, float& value)
{
    if (text == nullptr || text[0] == L'\0') {
        return false;
    }

    wchar_t* end = nullptr;
    const float parsed = std::wcstof(text, &end);
    if (end == text || *end != L'\0' || !std::isfinite(parsed) ||
        parsed <= 0.0f) {
        return false;
    }

    value = parsed;
    return true;
}

std::optional<CaptureOptions> parseCaptureOptions()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return std::nullopt;
    }

    CaptureOptions options;
    bool captureRequested = false;
    bool valid = true;

    for (int index = 1; index < argc && valid; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--capture-skiaui" || argument == L"--render-to-png") {
            if (index + 1 >= argc) {
                valid = false;
                break;
            }
            captureRequested = true;
            options.outputPath = std::filesystem::path(argv[++index]);
        } else if (argument == L"--capture-width") {
            valid = index + 1 < argc &&
                    parsePositiveInt(argv[++index], options.width);
        } else if (argument == L"--capture-height") {
            valid = index + 1 < argc &&
                    parsePositiveInt(argv[++index], options.height);
        } else if (argument == L"--capture-dpi-scale") {
            valid = index + 1 < argc &&
                    parsePositiveFloat(argv[++index], options.dpiScale);
        }
    }

    LocalFree(argv);

    if (!valid) {
        return CaptureOptions{};
    }
    if (!captureRequested) {
        return std::nullopt;
    }
    return options;
}

bool writePngFile(const std::filesystem::path& outputPath,
                  const std::vector<std::uint32_t>& pixels,
                  int width,
                  int height,
                  std::size_t rowBytes)
{
    const SkImageInfo imageInfo =
        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                          kPremul_SkAlphaType);
    const SkPixmap pixmap(imageInfo, pixels.data(), rowBytes);
    const sk_sp<SkData> pngData =
        SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!pngData || pngData->empty()) {
        return false;
    }

    const std::filesystem::path parentPath = outputPath.parent_path();
    if (!parentPath.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parentPath, error);
        if (error) {
            return false;
        }
    }

    std::ofstream output(outputPath, std::ios::binary);
    if (!output) {
        return false;
    }

    output.write(static_cast<const char*>(pngData->data()),
                 static_cast<std::streamsize>(pngData->size()));
    return output.good();
}

int captureSkiaUiPng(const CaptureOptions& options)
{
    if (options.outputPath.empty()) {
        return 2;
    }

    const std::string html = makeEmbeddedDocument();
    if (html.empty()) {
        return 3;
    }

    skui::RuntimeOptions runtimeOptions;
    runtimeOptions.clearColor = kDemoClearColor;

    skui::Runtime runtime(runtimeOptions);
    installDemoInteractions(runtime);
    runtime.resize(options.width, options.height, options.dpiScale);
    if (!runtime.loadDocumentFromString(html)) {
        return 4;
    }

    const std::size_t rowBytes =
        static_cast<std::size_t>(options.width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(options.width) *
        static_cast<std::size_t>(options.height));
    if (!runtime.renderToBgraPixels(pixels.data(), options.width,
                                    options.height, rowBytes,
                                    options.dpiScale)) {
        return 5;
    }

    return writePngFile(options.outputPath, pixels, options.width,
                        options.height, rowBytes)
        ? 0
        : 6;
}

int runSkiaUiApp(HINSTANCE instance, int showCmd)
{
    if (const std::optional<CaptureOptions> captureOptions =
            parseCaptureOptions()) {
        const int result = captureSkiaUiPng(*captureOptions);
        core::async::shutdown();
        return result;
    }

    const auto html =
        std::make_shared<const std::string>(makeEmbeddedDocument());
    if (html->empty()) {
        return 1;
    }
    const auto documentLoaded = std::make_shared<bool>(false);

    skui::win32::WindowOptions options;
    options.title = L"RelayDesk";
    options.logicalWidth = 1600;
    options.logicalHeight = 900;
    options.useSystemDpiScale = true;
    options.clearColor = colorRefFromSkColor(kDemoClearColor);
    options.runtime.clearColor = kDemoClearColor;
    options.onRuntimeReady = [](skui::Runtime& runtime) {
        installDemoInteractions(runtime);
    };
    options.onRuntimeResize = [html, documentLoaded](skui::Runtime& runtime) {
        if (!*documentLoaded && runtime.loadDocumentFromString(*html)) {
            *documentLoaded = true;
        }
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    const int result = app.run(instance, showCmd);
    core::async::shutdown();
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd)
{
    try {
        return runSkiaUiApp(instance, showCmd);
    } catch (...) {
        core::async::shutdown();
        return 1;
    }
}
