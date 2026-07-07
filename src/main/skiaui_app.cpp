#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
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
#include "main/app_runtime.h"

namespace {

constexpr SkColor kDemoClearColor = SkColorSetRGB(248, 250, 252);
constexpr std::string_view kTabs[] = {
    "tab-chat",
    "tab-history",
    "tab-transfer",
};
constexpr int kRelayDeskHtmlResourceId = 101;
constexpr int kDefaultCaptureWidth = 1600;
constexpr int kDefaultCaptureHeight = 900;
constexpr float kDefaultCaptureDpiScale = 1.0f;
constexpr int kDeviceRowPoolSize = 128;
constexpr int kDeviceTitleTop = 16;
constexpr int kDeviceFirstRowTop = 56;
constexpr int kDeviceRowHeight = 86;
constexpr int kDeviceRowGap = 88;
constexpr int kDeviceSectionGap = 22;
constexpr int kDeviceSectionRowGap = 40;
constexpr int kDeviceContentBottomPadding = 12;
constexpr int kDeviceMinimumVirtualHeight = 180;
constexpr UINT kSkiaUiRequestRedrawMessage = WM_APP + 0x531;
constexpr UINT kRelayDeskSkiaUiRefreshMs = 500;

struct CaptureOptions {
    std::filesystem::path outputPath;
    int width = kDefaultCaptureWidth;
    int height = kDefaultCaptureHeight;
    float dpiScale = kDefaultCaptureDpiScale;
};

struct SkiaUiRuntimeBinding {
    relaydesk::runtime::RelayDeskRuntime* relayRuntime = nullptr;
    skui::Runtime* skiaRuntime = nullptr;
    HWND window = nullptr;
    UINT_PTR timerId = 0;
    bool documentLoaded = false;
    std::string lastDeviceSignature;
};

SkiaUiRuntimeBinding* gRuntimeBinding = nullptr;

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

std::string indexedId(std::string_view prefix, int index)
{
    std::string id(prefix);
    id += std::to_string(index);
    return id;
}

std::string makeDeviceRowMarkup(int index)
{
    const std::string rowIndex = std::to_string(index);
    std::string html;
    html.reserve(900);
    html += R"(        <div id="device-row-)";
    html += rowIndex;
    html += R"(" class="device" style="display: none;" data-action="">)";
    html += R"(<div id="device-dot-)";
    html += rowIndex;
    html += R"(" class="status-dot"></div>)";
    html += R"(<svg class="device-icon" viewBox="0 0 24 24" fill="none">)";
    html += R"(<rect x="3" y="4.71" width="18" height="11.76" rx="1.56")";
    html += R"( fill="currentColor"></rect>)";
    html += R"(<rect x="5.88" y="7.06" width="12.24" height="6.47" fill="#ffffff"></rect>)";
    html += R"(<rect x="10.56" y="16" width="2.88" height="3.84" fill="currentColor"></rect>)";
    html += R"(<rect x="5.4" y="19.41" width="13.2" height="1.92")";
    html += R"( rx="0.96" fill="currentColor"></rect>)";
    html += R"(</svg><div id="device-name-)";
    html += rowIndex;
    html += R"(" class="device-name"></div><div id="device-ip-)";
    html += rowIndex;
    html += R"(" class="device-ip"></div></div>)";
    html += '\n';
    return html;
}

std::string makeDeviceRowsMarkup()
{
    std::string html;
    html.reserve(static_cast<std::size_t>(kDeviceRowPoolSize) * 900u);
    for (int index = 0; index < kDeviceRowPoolSize; ++index) {
        html += makeDeviceRowMarkup(index);
    }
    return html;
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
    replaceAll(html, "<!-- DEVICE_ROWS -->", makeDeviceRowsMarkup());

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

std::string peerDisplayName(const relaydesk::runtime::PeerListItem& peer)
{
    if (!peer.GetDisplayName().empty()) {
        return peer.GetDisplayName();
    }
    if (!peer.GetHostName().empty()) {
        return peer.GetHostName();
    }
    return peer.GetDeviceId();
}

std::string peerAddressText(const relaydesk::runtime::PeerListItem& peer)
{
    if (!peer.GetAddress().empty() && peer.GetAddress() != "unknown") {
        return peer.GetAddress();
    }
    return "未知地址";
}

void addTextUpdate(skui::RuntimeUpdates& updates,
                   std::string id,
                   std::string text)
{
    updates.texts.push_back({std::move(id), std::move(text)});
}

void addStyleUpdate(skui::RuntimeUpdates& updates,
                    std::string id,
                    std::string declarations)
{
    updates.styles.push_back({std::move(id), std::move(declarations)});
}

void addAttributeUpdate(skui::RuntimeUpdates& updates,
                        std::string id,
                        std::string name,
                        std::string value)
{
    updates.attributes.push_back(
        {std::move(id), std::move(name), std::move(value)});
}

std::string makeDeviceListSignature(
    const relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    const auto& localUser = relayRuntime.GetLocalUser();
    std::string signature = localUser.GetDeviceId();
    signature += '\n';
    signature += localUser.GetDisplayName();
    signature += '\n';
    signature += localUser.GetHostName();
    signature += '\n';
    signature += relayRuntime.GetSelectedPeerDeviceId();

    for (const auto& peer : relayRuntime.GetPeers()) {
        signature += '\n';
        signature += peer.GetDeviceId();
        signature += '|';
        signature += peer.GetDisplayName();
        signature += '|';
        signature += peer.GetHostName();
        signature += '|';
        signature += peer.GetAddress();
        signature += '|';
        signature += peer.GetOnline() ? '1' : '0';
        signature += '|';
        signature += std::to_string(peer.GetUnreadMessageCount());
    }
    return signature;
}

void hideDeviceRow(skui::RuntimeUpdates& updates, int rowIndex)
{
    addStyleUpdate(updates, indexedId("device-row-", rowIndex), "display: none;");
    addAttributeUpdate(updates,
                       indexedId("device-row-", rowIndex),
                       "class",
                       "device");
    addAttributeUpdate(updates,
                       indexedId("device-row-", rowIndex),
                       "data-action",
                       "");
    addAttributeUpdate(updates,
                       indexedId("device-dot-", rowIndex),
                       "class",
                       "status-dot");
    addTextUpdate(updates, indexedId("device-name-", rowIndex), "");
    addTextUpdate(updates, indexedId("device-ip-", rowIndex), "");
}

void showDeviceRow(skui::RuntimeUpdates& updates,
                   const relaydesk::runtime::PeerListItem& peer,
                   const std::string& selectedDeviceId,
                   int rowIndex,
                   int top)
{
    const std::string rowId = indexedId("device-row-", rowIndex);
    const bool selected = peer.GetDeviceId() == selectedDeviceId;
    addStyleUpdate(updates,
                   rowId,
                   "display: flex; top: " + std::to_string(top) + "px;");
    addAttributeUpdate(updates,
                       rowId,
                       "class",
                       selected ? "device device-selected" : "device");
    addAttributeUpdate(updates,
                       rowId,
                       "data-action",
                       "select-device:" + peer.GetDeviceId());
    addAttributeUpdate(updates,
                       indexedId("device-dot-", rowIndex),
                       "class",
                       peer.GetOnline() ? "status-dot" : "status-dot offline");
    addTextUpdate(updates, indexedId("device-name-", rowIndex), peerDisplayName(peer));
    addTextUpdate(updates, indexedId("device-ip-", rowIndex), peerAddressText(peer));
}

int deviceContentHeightForLastRowTop(int rowTop)
{
    return std::max(kDeviceMinimumVirtualHeight,
                    rowTop + kDeviceRowHeight + kDeviceContentBottomPadding);
}

void applyRelayDeskDevicePanel(skui::Runtime& skiaRuntime,
                               relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    relayRuntime.refreshPeersIfNeeded();

    const auto& localUser = relayRuntime.GetLocalUser();
    skui::RuntimeUpdates updates;
    const std::string localDisplayName =
        localUser.GetDisplayName().empty() ? localUser.GetHostName()
                                           : localUser.GetDisplayName();
    addTextUpdate(updates, "profile-name", localDisplayName);
    addTextUpdate(updates, "profile-sub", "本机 · " + localUser.GetHostName());

    const std::optional<relaydesk::runtime::PeerListItem> selectedPeer =
        relayRuntime.GetSelectedPeer();
    if (selectedPeer.has_value()) {
        addTextUpdate(updates, "header-title", peerDisplayName(*selectedPeer));
        addTextUpdate(updates, "header-ip", peerAddressText(*selectedPeer));
        addStyleUpdate(updates,
                       "header-status-dot",
                       selectedPeer->GetOnline()
                           ? "background-color: #26bd31;"
                           : "background-color: #a8b3c0;");
        addAttributeUpdate(updates,
                           "composer",
                           "placeholder",
                           "给 " + peerDisplayName(*selectedPeer) + " 发送消息");
    } else {
        addTextUpdate(updates, "header-title", "未选择设备");
        addTextUpdate(updates, "header-ip", "等待发现设备");
        addStyleUpdate(updates, "header-status-dot", "background-color: #a8b3c0;");
        addAttributeUpdate(updates, "composer", "placeholder", "选择设备后发送消息");
    }

    std::vector<const relaydesk::runtime::PeerListItem*> onlinePeers;
    std::vector<const relaydesk::runtime::PeerListItem*> offlinePeers;
    for (const auto& peer : relayRuntime.GetPeers()) {
        if (peer.GetOnline()) {
            onlinePeers.push_back(&peer);
        } else {
            offlinePeers.push_back(&peer);
        }
    }

    addTextUpdate(updates,
                  "device-online-title",
                  "在线设备 (" + std::to_string(onlinePeers.size()) + ")");
    addStyleUpdate(updates,
                   "device-online-title",
                   "display: flex; top: " + std::to_string(kDeviceTitleTop)
                       + "px;");

    int rowIndex = 0;
    int lastVisibleRowTop = -1;
    for (std::size_t index = 0;
         index < onlinePeers.size() && rowIndex < kDeviceRowPoolSize;
         ++index) {
        const int top = kDeviceFirstRowTop + static_cast<int>(index) * kDeviceRowGap;
        showDeviceRow(updates,
                      *onlinePeers[index],
                      relayRuntime.GetSelectedPeerDeviceId(),
                      rowIndex,
                      top);
        lastVisibleRowTop = top;
        ++rowIndex;
    }

    if (!offlinePeers.empty() && rowIndex < kDeviceRowPoolSize) {
        const int offlineTitleTop = rowIndex > 0
            ? kDeviceFirstRowTop + rowIndex * kDeviceRowGap + kDeviceSectionGap
            : kDeviceFirstRowTop;
        addTextUpdate(updates,
                      "device-offline-title",
                      "离线设备 (" + std::to_string(offlinePeers.size()) + ")");
        addStyleUpdate(updates,
                       "device-offline-title",
                       "display: flex; top: " + std::to_string(offlineTitleTop)
                           + "px;");
        const int offlineFirstRowTop = offlineTitleTop + kDeviceSectionRowGap;
        for (std::size_t index = 0;
             index < offlinePeers.size() && rowIndex < kDeviceRowPoolSize;
             ++index) {
            const int top =
                offlineFirstRowTop + static_cast<int>(index) * kDeviceRowGap;
            showDeviceRow(updates,
                          *offlinePeers[index],
                          relayRuntime.GetSelectedPeerDeviceId(),
                          rowIndex,
                          top);
            lastVisibleRowTop = top;
            ++rowIndex;
        }
    } else {
        addStyleUpdate(updates, "device-offline-title", "display: none;");
    }

    const bool hasAnyPeer = !onlinePeers.empty() || !offlinePeers.empty();
    addStyleUpdate(updates, "device-empty", hasAnyPeer ? "display: none;"
                                                       : "display: flex;");
    addTextUpdate(updates,
                  "device-empty",
                  relayRuntime.GetStartupErrorMessage().empty()
                      ? "暂无已发现设备"
                      : "发现服务暂不可用");

    for (; rowIndex < kDeviceRowPoolSize; ++rowIndex) {
        hideDeviceRow(updates, rowIndex);
    }

    const int contentHeight = lastVisibleRowTop >= 0
        ? deviceContentHeightForLastRowTop(lastVisibleRowTop)
        : kDeviceMinimumVirtualHeight;
    addAttributeUpdate(updates,
                       "device-list",
                       "data-virtual-height",
                       std::to_string(contentHeight));
    addStyleUpdate(updates,
                   "device-list-content",
                   "height: " + std::to_string(contentHeight) + "px;");

    skiaRuntime.applyUpdates(updates);
}

void selectTab(skui::Runtime& runtime, std::string_view id)
{
    for (std::string_view tab : kTabs) {
        runtime.removeClassById(tab, "tab-active");
    }
    runtime.addClassById(id, "tab-active");
}

void installRelayDeskInteractions(skui::Runtime& runtime,
                                  relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    runtime.setElementEventCallback([&runtime, &relayRuntime](
                                        const skui::ElementEvent& event) {
        if (event.type != skui::ElementEventType::Click || event.action.empty()) {
            return;
        }

        constexpr std::string_view devicePrefix = "select-device:";
        constexpr std::string_view tabPrefix = "tab:";
        const std::string_view action(event.action);
        if (action.starts_with(devicePrefix)) {
            relayRuntime.selectPeer(std::string(action.substr(devicePrefix.size())));
            applyRelayDeskDevicePanel(runtime, relayRuntime);
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

struct SkiaUiWindowSearchContext {
    DWORD processId = GetCurrentProcessId();
    HWND window = nullptr;
};

BOOL CALLBACK findSkiaUiRelayDeskWindowProc(HWND window, LPARAM contextAddress)
{
    auto* context = reinterpret_cast<SkiaUiWindowSearchContext*>(contextAddress);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != context->processId || !IsWindowVisible(window)) {
        return TRUE;
    }

    wchar_t className[64]{};
    const int classLength = GetClassNameW(
        window,
        className,
        static_cast<int>(sizeof(className) / sizeof(className[0])));
    if (classLength <= 0 || std::wstring_view(className) != L"SkiaUiDeskWindow") {
        return TRUE;
    }

    wchar_t title[64]{};
    GetWindowTextW(window,
                   title,
                   static_cast<int>(sizeof(title) / sizeof(title[0])));
    if (std::wstring_view(title) != L"RelayDesk") {
        return TRUE;
    }

    context->window = window;
    return FALSE;
}

HWND findSkiaUiRelayDeskWindow()
{
    SkiaUiWindowSearchContext context;
    EnumWindows(findSkiaUiRelayDeskWindowProc, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

void requestSkiaUiWindowRedraw(SkiaUiRuntimeBinding& binding)
{
    if (binding.window == nullptr || !IsWindow(binding.window)) {
        binding.window = findSkiaUiRelayDeskWindow();
    }
    if (binding.window != nullptr) {
        PostMessageW(binding.window, kSkiaUiRequestRedrawMessage, 0, 0);
    }
}

bool refreshRelayDeskDevicePanelIfChanged(SkiaUiRuntimeBinding& binding,
                                          bool force)
{
    if (binding.relayRuntime == nullptr ||
        binding.skiaRuntime == nullptr ||
        !binding.documentLoaded) {
        return false;
    }

    core::async::dispatchReady();
    binding.relayRuntime->refreshPeersIfNeeded();
    const std::string nextSignature =
        makeDeviceListSignature(*binding.relayRuntime);
    if (!force && nextSignature == binding.lastDeviceSignature) {
        return false;
    }

    binding.lastDeviceSignature = nextSignature;
    applyRelayDeskDevicePanel(*binding.skiaRuntime, *binding.relayRuntime);
    return true;
}

void CALLBACK refreshRelayDeskSkiaUiTimer(HWND, UINT, UINT_PTR, DWORD)
{
    if (gRuntimeBinding == nullptr) {
        return;
    }
    if (refreshRelayDeskDevicePanelIfChanged(*gRuntimeBinding, false)) {
        requestSkiaUiWindowRedraw(*gRuntimeBinding);
    }
}

relaydesk::runtime::RelayDeskRuntimeOptions makeCaptureRuntimeOptions()
{
    relaydesk::runtime::RelayDeskRuntimeOptions options;
    options.SetTcpListenPort(0);
    options.SetDiscoveryUdpPort(0);
    options.SetDiscoveryBroadcastEnabled(false);
    options.SetDiscoveryAnnounceOnStart(false);
    options.SetNetworkEnabled(false);
    options.SetAsyncRefreshEnabled(false);
    return options;
}

class CaptureRelayDeskRuntime : public relaydesk::runtime::RelayDeskRuntime {
public:
    CaptureRelayDeskRuntime()
        : RelayDeskRuntime(makeCaptureRuntimeOptions())
    {
    }
};

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
    const std::filesystem::path outputPath =
        std::filesystem::absolute(options.outputPath);

    const std::string html = makeEmbeddedDocument();
    if (html.empty()) {
        return 3;
    }

    skui::RuntimeOptions runtimeOptions;
    runtimeOptions.clearColor = kDemoClearColor;

    CaptureRelayDeskRuntime relayRuntime;
    skui::Runtime runtime(runtimeOptions);
    installRelayDeskInteractions(runtime, relayRuntime);
    runtime.resize(options.width, options.height, options.dpiScale);
    if (!runtime.loadDocumentFromString(html)) {
        return 4;
    }
    applyRelayDeskDevicePanel(runtime, relayRuntime);

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

    return writePngFile(outputPath, pixels, options.width, options.height, rowBytes)
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
    auto& relayRuntime = relaydesk::runtime::getRelayDeskRuntime();
    auto binding = std::make_shared<SkiaUiRuntimeBinding>();
    binding->relayRuntime = &relayRuntime;
    gRuntimeBinding = binding.get();

    skui::win32::WindowOptions options;
    options.title = L"RelayDesk";
    options.logicalWidth = 1600;
    options.logicalHeight = 900;
    options.useSystemDpiScale = true;
    options.clearColor = colorRefFromSkColor(kDemoClearColor);
    options.runtime.clearColor = kDemoClearColor;
    options.onRuntimeReady = [binding](skui::Runtime& runtime) {
        binding->skiaRuntime = &runtime;
        installRelayDeskInteractions(runtime, *binding->relayRuntime);
        binding->timerId = SetTimer(
            nullptr,
            0,
            kRelayDeskSkiaUiRefreshMs,
            refreshRelayDeskSkiaUiTimer);
    };
    options.onRuntimeResize = [html, documentLoaded, binding](skui::Runtime& runtime) {
        if (!*documentLoaded && runtime.loadDocumentFromString(*html)) {
            *documentLoaded = true;
            binding->documentLoaded = true;
            (void)refreshRelayDeskDevicePanelIfChanged(*binding, true);
        }
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    const int result = app.run(instance, showCmd);
    if (binding->timerId != 0) {
        KillTimer(nullptr, binding->timerId);
    }
    if (gRuntimeBinding == binding.get()) {
        gRuntimeBinding = nullptr;
    }
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
