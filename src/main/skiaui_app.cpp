#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdio>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <exception>
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
#include "platform/text_encoding.h"

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
constexpr int kChatMessagePaneLeft = 402;
constexpr int kChatContextMenuWidth = 132;
constexpr int kChatContextMenuHeight = 38;
constexpr UINT kSkiaUiRequestRedrawMessage = WM_APP + 0x531;
constexpr UINT kRelayDeskSkiaUiRefreshMs = 500;

struct CaptureOptions {
    std::filesystem::path outputPath;
    int width = kDefaultCaptureWidth;
    int height = kDefaultCaptureHeight;
    float dpiScale = kDefaultCaptureDpiScale;
    int initialWidth = 0;
    int initialHeight = 0;
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
std::string gMessageContextText;
bool gMessageContextMenuVisible = false;

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

std::string escapeHtml(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

std::string truncateUtf8Bytes(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes) {
        return std::string(text);
    }

    std::size_t length = maxBytes;
    while (length > 0 &&
           (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    std::string truncated(text.substr(0, length));
    truncated += "...";
    return truncated;
}

std::string trimMessageWhitespace(std::string_view text)
{
    std::size_t first = 0;
    while (first < text.size()) {
        const char value = text[first];
        if (value != ' ' && value != '\r' && value != '\n' && value != '\t') {
            break;
        }
        ++first;
    }

    std::size_t last = text.size();
    while (last > first) {
        const char value = text[last - 1];
        if (value != ' ' && value != '\r' && value != '\n' && value != '\t') {
            break;
        }
        --last;
    }
    return std::string(text.substr(first, last - first));
}

bool isUrlStart(std::string_view text, std::size_t offset)
{
    const std::string_view rest = text.substr(offset);
    return rest.starts_with("http://") || rest.starts_with("https://");
}

bool isUrlTerminator(char value)
{
    return value == ' ' || value == '\r' || value == '\n' || value == '\t' ||
           value == '<' || value == '>' || value == '"' || value == '\'';
}

std::vector<std::pair<std::size_t, std::size_t>> findUrlRanges(std::string_view text)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        if (!isUrlStart(text, offset)) {
            continue;
        }
        std::size_t end = offset;
        while (end < text.size() && !isUrlTerminator(text[end])) {
            ++end;
        }
        while (end > offset &&
               (text[end - 1] == '.' || text[end - 1] == ',' ||
                text[end - 1] == ';' || text[end - 1] == ')' ||
                text[end - 1] == ']')) {
            --end;
        }
        if (end > offset) {
            ranges.emplace_back(offset, end);
            offset = end - 1;
        }
    }
    return ranges;
}

std::string makeUrlLinkAttributes(std::string_view text)
{
    const std::vector<std::pair<std::size_t, std::size_t>> ranges =
        findUrlRanges(text);
    if (ranges.empty()) {
        return {};
    }

    std::string attributes;
    attributes += R"( data-links=")";
    bool first = true;
    for (const auto& [start, end] : ranges) {
        if (!first) {
            attributes += "\n";
        }
        first = false;
        attributes += std::to_string(start);
        attributes += ':';
        attributes += std::to_string(end);
        attributes += ":open-url:";
        attributes += escapeHtml(text.substr(start, end - start));
    }
    attributes += R"(")";
    return attributes;
}

int runtimeLogicalWidth(const skui::Runtime& runtime)
{
    return std::max(1,
                    static_cast<int>(std::lround(
                        static_cast<float>(runtime.width()) /
                        runtime.effectiveScale())));
}

int runtimeLogicalHeight(const skui::Runtime& runtime)
{
    return std::max(1,
                    static_cast<int>(std::lround(
                        static_cast<float>(runtime.height()) /
                        runtime.effectiveScale())));
}

bool writeClipboardText(std::string_view text)
{
    std::wstring wide;
    try {
        wide = relaydesk::platform::utf8ToWide(std::string(text));
    } catch (const std::exception&) {
        return false;
    }

    if (OpenClipboard(nullptr) == FALSE) {
        return false;
    }

    EmptyClipboard();
    const std::size_t bytes = (wide.size() + 1u) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (handle == nullptr) {
        CloseClipboard();
        return false;
    }

    if (void* memory = GlobalLock(handle)) {
        std::memcpy(memory, wide.c_str(), bytes);
        GlobalUnlock(handle);
    } else {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }

    if (SetClipboardData(CF_UNICODETEXT, handle) == nullptr) {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

void hideMessageContextMenu(skui::Runtime& runtime)
{
    gMessageContextMenuVisible = false;
    runtime.setStyleById("message-context-menu", "display: none;");
}

void showMessageContextMenu(skui::Runtime& runtime, float x, float y)
{
    const int mainWidth =
        std::max(1, runtimeLogicalWidth(runtime) - kChatMessagePaneLeft);
    const int menuLeft =
        std::clamp(static_cast<int>(std::lround(x)) - kChatMessagePaneLeft,
                   0,
                   std::max(0, mainWidth - kChatContextMenuWidth));
    const int menuTop =
        std::clamp(static_cast<int>(std::lround(y)),
                   0,
                   std::max(0, runtimeLogicalHeight(runtime) - kChatContextMenuHeight));
    std::string style = "display: flex; left: ";
    style += std::to_string(menuLeft);
    style += "px; top: ";
    style += std::to_string(menuTop);
    style += "px;";
    gMessageContextMenuVisible = true;
    runtime.setStyleById("message-context-menu", style);
}

bool eventHasClass(const skui::ElementEvent& event, std::string_view className)
{
    return std::find(event.classes.begin(), event.classes.end(), className) !=
           event.classes.end();
}

bool isMessageContextMenuEvent(const skui::ElementEvent& event)
{
    return event.id == "message-context-menu" ||
           event.id == "message-context-copy" ||
           event.action == "copy-message-context" ||
           eventHasClass(event, "message-context-menu") ||
           eventHasClass(event, "message-context-item");
}

std::string shortMessageTime(const relaydesk::storage::ChatMessageRecord& message)
{
    const std::string& createdAt = message.GetCreatedAt();
    if (createdAt.size() >= 16 && createdAt[10] == 'T') {
        return createdAt.substr(11, 5);
    }
    if (createdAt.size() >= 5) {
        return createdAt.substr(0, 5);
    }
    return {};
}

std::string formatFileSize(std::uintmax_t size)
{
    char buffer[32]{};
    if (size >= 1024ull * 1024ull) {
        const double value = static_cast<double>(size) / (1024.0 * 1024.0);
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", value);
        return buffer;
    }
    if (size >= 1024ull) {
        const double value = static_cast<double>(size) / 1024.0;
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", value);
        return buffer;
    }
    return std::to_string(size) + " B";
}

std::string transferStateText(relaydesk::storage::TransferState state)
{
    switch (state) {
    case relaydesk::storage::TransferState::Pending:
        return "等待传输";
    case relaydesk::storage::TransferState::Offered:
        return "等待接收";
    case relaydesk::storage::TransferState::Transferring:
        return "传输中";
    case relaydesk::storage::TransferState::Interrupted:
        return "已中断";
    case relaydesk::storage::TransferState::Completed:
        return "已完成";
    case relaydesk::storage::TransferState::Failed:
        return "传输失败";
    case relaydesk::storage::TransferState::Cancelled:
        return "已取消";
    case relaydesk::storage::TransferState::Rejected:
        return "已拒绝";
    }
    return "未知状态";
}

int transferProgressPercent(const relaydesk::storage::ChatMessagePart& part)
{
    if (part.GetTransferState().has_value() &&
        part.GetTransferState().value() ==
            relaydesk::storage::TransferState::Completed) {
        return 100;
    }
    if (!part.GetFileSize().has_value() || part.GetFileSize().value() == 0 ||
        !part.GetTransferredSize().has_value()) {
        return 0;
    }

    const double ratio = static_cast<double>(part.GetTransferredSize().value()) /
        static_cast<double>(part.GetFileSize().value());
    return std::clamp(static_cast<int>(std::round(ratio * 100.0)), 0, 100);
}

std::string partDisplayText(const relaydesk::storage::ChatMessagePart& part)
{
    switch (part.GetType()) {
    case relaydesk::storage::MessagePartType::Text:
        if (part.GetText().has_value()) {
            return part.GetText().value();
        }
        return "文本消息";
    case relaydesk::storage::MessagePartType::Emoji:
        if (part.GetEmoji().has_value()) {
            return part.GetEmoji().value();
        }
        return "表情消息";
    case relaydesk::storage::MessagePartType::Image:
        if (part.GetFileName().has_value()) {
            return "[图片] " + part.GetFileName().value();
        }
        return "图片消息";
    case relaydesk::storage::MessagePartType::File:
        if (part.GetFileName().has_value()) {
            return part.GetFileName().value();
        }
        return "文件消息";
    case relaydesk::storage::MessagePartType::Folder:
        if (part.GetFileName().has_value()) {
            return part.GetFileName().value();
        }
        return "文件夹消息";
    }
    return "消息";
}

std::string makeTextMessageMarkup(
    const relaydesk::storage::ChatMessageRecord& message,
    const relaydesk::storage::ChatMessagePart& part)
{
    const bool outgoing =
        message.GetDirection() == relaydesk::storage::MessageDirection::Outgoing;
    const std::string text = trimMessageWhitespace(partDisplayText(part));
    const std::string timeText = shortMessageTime(message);

    std::string html;
    html.reserve(620);
    html += R"(<div class="message-row )";
    html += outgoing ? "message-row-right" : "message-row-left";
    html += R"(">)";
    if (outgoing && !timeText.empty()) {
        html += R"(<div class="time-label">)";
        html += escapeHtml(timeText);
        html += R"(</div>)";
    }
    html += R"(<selectable class="bubble )";
    html += outgoing ? "bubble-right" : "bubble-left";
    html += R"(")";
    html += makeUrlLinkAttributes(text);
    html += ">";
    html += escapeHtml(text);
    html += R"(</selectable>)";
    if (!outgoing && !timeText.empty()) {
        html += R"(<div class="time-label">)";
        html += escapeHtml(timeText);
        html += R"(</div>)";
    }
    html += R"(</div>)";
    return html;
}

std::string makeTransferMessageMarkup(
    const relaydesk::storage::ChatMessageRecord& message,
    const relaydesk::storage::ChatMessagePart& part)
{
    const bool outgoing =
        message.GetDirection() == relaydesk::storage::MessageDirection::Outgoing;
    const int progress = transferProgressPercent(part);
    const bool warning = part.GetTransferState().has_value() &&
        (part.GetTransferState().value() ==
             relaydesk::storage::TransferState::Failed ||
         part.GetTransferState().value() ==
             relaydesk::storage::TransferState::Interrupted ||
         part.GetTransferState().value() ==
             relaydesk::storage::TransferState::Cancelled ||
         part.GetTransferState().value() ==
             relaydesk::storage::TransferState::Rejected);
    const std::string title = partDisplayText(part);
    const std::string sizeText =
        part.GetFileSize().has_value() ? formatFileSize(part.GetFileSize().value())
                                       : "文件夹";
    const std::string stateText = part.GetTransferState().has_value()
        ? transferStateText(part.GetTransferState().value())
        : "等待传输";

    std::string html;
    html.reserve(760);
    html += R"(<div class="message-row message-row-transfer">)";
    html += R"(<div class="transfer-card)";
    if (warning) {
        html += " warning";
    }
    html += R"(">)";
    html += R"(<div class="file-icon doc-icon doc-icon-zip"><div class="doc-fold"></div></div>)";
    html += R"(<div class="file-name">)";
    html += escapeHtml(title);
    html += R"(</div><div class="file-size">)";
    html += escapeHtml(sizeText);
    html += R"(</div><div class="transfer-percent)";
    if (progress == 100) {
        html += " done";
    }
    html += R"(">)";
    html += std::to_string(progress);
    html += R"(%</div><progress class="progress-main)";
    if (warning) {
        html += " warning";
    }
    html += R"(" value=")";
    html += std::to_string(progress);
    html += R"(" max="100"></progress><div class="transfer-meta">)";
    html += escapeHtml(stateText);
    html += R"(</div>)";
    const std::string timeText = shortMessageTime(message);
    if (!timeText.empty()) {
        html += R"(<div class="card-time">)";
        html += escapeHtml(timeText);
        html += R"(</div>)";
    }
    if (outgoing) {
        html += R"(<svg class="card-mark" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M2 13.5 6 17.5 15 8.5"></path><path d="M9 15.5 11 17.5 22 6.5"></path></svg>)";
    }
    html += R"(</div></div>)";
    return html;
}

std::string makeChatContentMarkup(
    const relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    std::string html;
    html.reserve(4096);
    html += R"(<div id="chat-content" class="chat-content">)";
    html += R"(<div class="day-row"><div class="day-pill">今天</div></div>)";

    const auto& messages = relayRuntime.GetSelectedPeerMessages();
    if (!relayRuntime.GetSelectedPeer().has_value()) {
        relaydesk::storage::ChatMessageRecord placeholder;
        relaydesk::storage::ChatMessagePart part;
        part.SetText("请选择左侧设备查看聊天记录");
        html += makeTextMessageMarkup(placeholder, part);
    } else if (messages.empty()) {
        relaydesk::storage::ChatMessageRecord placeholder;
        relaydesk::storage::ChatMessagePart part;
        part.SetText("暂无聊天消息");
        html += makeTextMessageMarkup(placeholder, part);
    } else {
        for (const auto& message : messages) {
            bool rendered = false;
            for (const auto& part : message.GetParts()) {
                if (part.GetType() == relaydesk::storage::MessagePartType::File ||
                    part.GetType() == relaydesk::storage::MessagePartType::Folder ||
                    part.GetType() == relaydesk::storage::MessagePartType::Image) {
                    html += makeTransferMessageMarkup(message, part);
                } else {
                    html += makeTextMessageMarkup(message, part);
                }
                rendered = true;
            }

            if (!rendered) {
                relaydesk::storage::ChatMessagePart part;
                part.SetText("空消息");
                html += makeTextMessageMarkup(message, part);
            }
        }
    }

    html += R"(</div>)";
    return html;
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

std::string makeChatSignature(
    const relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    std::string signature = relayRuntime.GetSelectedPeerDeviceId();
    signature += '|';
    signature += relayRuntime.GetSelectedPeerHasMoreMessages() ? '1' : '0';
    for (const auto& message : relayRuntime.GetSelectedPeerMessages()) {
        signature += '\n';
        signature += message.GetMessageId();
        signature += '|';
        signature += message.GetCreatedAt();
        signature += '|';
        signature += std::to_string(static_cast<int>(message.GetDirection()));
        signature += '|';
        signature += std::to_string(static_cast<int>(message.GetDeliveryState()));
        for (const auto& part : message.GetParts()) {
            signature += '|';
            signature += part.GetPartId();
            signature += ':';
            signature += std::to_string(static_cast<int>(part.GetType()));
            if (part.GetText().has_value()) {
                signature += ':';
                signature += part.GetText().value();
            }
            if (part.GetEmoji().has_value()) {
                signature += ':';
                signature += part.GetEmoji().value();
            }
            if (part.GetFileName().has_value()) {
                signature += ':';
                signature += part.GetFileName().value();
            }
            if (part.GetFileSize().has_value()) {
                signature += ':';
                signature += std::to_string(part.GetFileSize().value());
            }
            if (part.GetTransferredSize().has_value()) {
                signature += ':';
                signature += std::to_string(part.GetTransferredSize().value());
            }
            if (part.GetTransferState().has_value()) {
                signature += ':';
                signature +=
                    std::to_string(static_cast<int>(part.GetTransferState().value()));
            }
        }
    }
    return signature;
}

std::string makeRelayDeskUiSignature(
    const relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    std::string signature = "--devices--\n";
    signature += makeDeviceListSignature(relayRuntime);
    signature += "\n--chat--\n";
    signature += makeChatSignature(relayRuntime);
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
    const std::string chatContentHtml = makeChatContentMarkup(relayRuntime);

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
    skiaRuntime.replaceHtmlById("chat-content", chatContentHtml);
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
        if (event.type == skui::ElementEventType::MouseUp &&
            event.button == skui::MouseButton::Right &&
            event.tag == "selectable" &&
            eventHasClass(event, "bubble")) {
            gMessageContextText = event.text;
            showMessageContextMenu(runtime, event.x, event.y);
            return;
        }

        if (gMessageContextMenuVisible &&
            event.type == skui::ElementEventType::MouseDown &&
            event.button == skui::MouseButton::Left &&
            !isMessageContextMenuEvent(event)) {
            hideMessageContextMenu(runtime);
        }

        constexpr std::string_view devicePrefix = "select-device:";
        constexpr std::string_view tabPrefix = "tab:";
        constexpr std::string_view urlPrefix = "open-url:";
        if (event.type != skui::ElementEventType::Click || event.action.empty()) {
            return;
        }

        const std::string_view action(event.action);
        if (action.starts_with(devicePrefix)) {
            hideMessageContextMenu(runtime);
            relayRuntime.selectPeer(std::string(action.substr(devicePrefix.size())));
            applyRelayDeskDevicePanel(runtime, relayRuntime);
        } else if (action.starts_with(tabPrefix)) {
            hideMessageContextMenu(runtime);
            selectTab(runtime, action.substr(tabPrefix.size()));
        } else if (action.starts_with(urlPrefix)) {
            hideMessageContextMenu(runtime);
            const std::string url(action.substr(urlPrefix.size()));
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (action == "copy-message-context") {
            (void)writeClipboardText(gMessageContextText);
            hideMessageContextMenu(runtime);
        } else if (action == "send-message") {
            hideMessageContextMenu(runtime);
            runtime.setAttributeById("composer", "value", "");
            runtime.setAttributeById(
                "composer",
                "placeholder",
                "消息已发送，可以继续输入...");
        } else if (action == "finish-transfer") {
            hideMessageContextMenu(runtime);
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
        makeRelayDeskUiSignature(*binding.relayRuntime);
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

relaydesk::storage::ChatMessagePart makeCaptureTextPart(std::string partId,
                                                        std::string text)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(std::move(partId));
    part.SetType(relaydesk::storage::MessagePartType::Text);
    part.SetText(std::move(text));
    return part;
}

relaydesk::storage::ChatMessagePart makeCaptureFilePart(
    std::string partId,
    relaydesk::storage::MessagePartType type,
    std::string fileName,
    std::uintmax_t fileSize,
    std::uintmax_t transferredSize,
    relaydesk::storage::TransferState transferState)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(std::move(partId));
    part.SetType(type);
    part.SetTransferId("capture-transfer-" + part.GetPartId());
    part.SetFileName(std::move(fileName));
    part.SetFileSize(fileSize);
    part.SetTransferredSize(transferredSize);
    part.SetTransferState(transferState);
    part.SetLocalPath("capture");
    return part;
}

relaydesk::storage::ChatMessageRecord makeCaptureMessage(
    std::string messageId,
    relaydesk::storage::MessageDirection direction,
    std::string createdAt,
    std::vector<relaydesk::storage::ChatMessagePart> parts)
{
    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(std::move(messageId));
    record.SetConversationId("capture-conversation");
    record.SetDirection(direction);
    record.SetSenderDeviceId(direction == relaydesk::storage::MessageDirection::Outgoing
                                 ? "capture-local"
                                 : "capture-peer");
    record.SetReceiverDeviceId(direction == relaydesk::storage::MessageDirection::Outgoing
                                   ? "capture-peer"
                                   : "capture-local");
    record.SetSenderDisplayNameSnapshot(
        direction == relaydesk::storage::MessageDirection::Outgoing
            ? "许靖"
            : "Alex-PC");
    record.SetReceiverDisplayNameSnapshot(
        direction == relaydesk::storage::MessageDirection::Outgoing
            ? "Alex-PC"
            : "许靖");
    record.SetCreatedAt(std::move(createdAt));
    record.SetDeliveryState(direction == relaydesk::storage::MessageDirection::Outgoing
                                ? relaydesk::storage::DeliveryState::Delivered
                                : relaydesk::storage::DeliveryState::Received);
    record.SetParts(std::move(parts));
    return record;
}

std::vector<relaydesk::storage::ChatMessageRecord> makeCaptureChatMessages()
{
    std::vector<relaydesk::storage::ChatMessageRecord> messages;
    messages.push_back(makeCaptureMessage(
        "capture-1",
        relaydesk::storage::MessageDirection::Incoming,
        "2026-07-09T06:48:00Z",
        {makeCaptureTextPart("p1", "emmm")}));
    messages.push_back(makeCaptureMessage(
        "capture-2",
        relaydesk::storage::MessageDirection::Incoming,
        "2026-07-09T06:49:00Z",
        {makeCaptureTextPart(
            "p1",
            "【队友说给我表演空翻】 https://www.bilibili.com/video/BV1jGTC6zEod/?share_source=copy_web&vd_source=eab9a93ad11792ced0b8dd2d8f6d2f1a")}));
    messages.push_back(makeCaptureMessage(
        "capture-3",
        relaydesk::storage::MessageDirection::Outgoing,
        "2026-07-09T06:50:00Z",
        {makeCaptureFilePart("p1",
                             relaydesk::storage::MessagePartType::Image,
                             "clipboard.bmp",
                             8ull * 1024ull * 1024ull + 420000ull,
                             8ull * 1024ull * 1024ull + 420000ull,
                             relaydesk::storage::TransferState::Completed)}));
    messages.push_back(makeCaptureMessage(
        "capture-4",
        relaydesk::storage::MessageDirection::Outgoing,
        "2026-07-09T06:51:00Z",
        {makeCaptureTextPart("p1", "为什么你看不懂还要阅读一下")}));
    messages.push_back(makeCaptureMessage(
        "capture-5",
        relaydesk::storage::MessageDirection::Incoming,
        "2026-07-09T06:52:00Z",
        {makeCaptureTextPart(
            "p1",
            "我就想看看这代码是不是符合你那个规范，之前的代码不都不显示过程对话嘛，谁知道他用没用那个技能")}));
    messages.push_back(makeCaptureMessage(
        "capture-6",
        relaydesk::storage::MessageDirection::Incoming,
        "2026-07-09T09:53:00Z",
        {makeCaptureTextPart("p1", "这个是图标地址 https://igoutu.cn/icons/styles 可以打开")}));
    messages.push_back(makeCaptureMessage(
        "capture-7",
        relaydesk::storage::MessageDirection::Incoming,
        "2026-07-09T09:54:00Z",
        {makeCaptureTextPart(
            "p1",
            "混合长链接 https://igoutu.cn/icon/lchz7JPUz9qU/%E8%AE%BE%E7%BD%AE 后面还有文字")}));
    messages.push_back(makeCaptureMessage(
        "capture-8",
        relaydesk::storage::MessageDirection::Outgoing,
        "2026-07-09T09:55:00Z",
        {makeCaptureTextPart("p1", "这是svg?")}));
    messages.push_back(makeCaptureMessage(
        "capture-9",
        relaydesk::storage::MessageDirection::Outgoing,
        "2026-07-09T09:56:00Z",
        {makeCaptureTextPart(
            "p1",
            "撒大声地撒实打实大大撒大声地撒实打实大大撒大声地撒实打实大大撒大声地撒实打实大大撒大声地撒实打实大大 sdasda大叔大婶大萨达啊实打实大大 撒大声地撒实打实大大")}));
    messages.push_back(makeCaptureMessage(
        "capture-10",
        relaydesk::storage::MessageDirection::Incoming,
        "2026-07-09T09:57:00Z",
        {makeCaptureTextPart(
            "p1",
            R"(<svg xmlns="http://www.w3.org/2000/svg" x="0px" y="0px" width="100" height="100" viewBox="0 0 48 48"> <path d="M 22.5 1 C 21.130937 1 20 2.1309372 20 3.5 L 20 6.5371094 C 18.010362 6.9917419 16.155535 7.7661122 14.476562 8.8203125 L 12.332031 6.6757812 C 11.36392 5.7076702 9.7643118 5.7066907 8.796875 6.6757812 L 6.6757812 8.796875 C 5.7076702 9.7649862 5.7066907 11.364594 6.6757812 12.332031 L 8.8203125 14.476562 C 7.7660236 16.155411 6.9919343 18.010216 6.5371094 20 L 3.5 20 C 2.1309372 20 1 21.130937 1 22.5 L 1 25.5 C 1 26.869063 2.1309372 28 3.5 28 L 6.5371094 28 C 6.9917419 29.989638 7.7661122 31.844465 8.8203125 33.523438 L 6.6757812 35.667969 C 5.7076702 36.63608 5.7066907 38.235688 6.6757812 39.203125 L 8.7949219 41.324219 C 9.7626308 42.291928 11.364322 42.291928 12.332031 41.324219 L 14.476562 39.179688 C 16.15488 40.233632 18.008815 41.006656 19.998047 41.460938 L 19.998047 44.498047 C 19.998047 45.86711 21.128984 46.998047 22.498047 46.998047 L 25.498047 46.998047 C 26.86711 46.998047 27.998047 45.86711 27.998047 44.498047 L 27.998047 41.462891 C 29.987843 41.008272 31.842394 40.233961 33.521484 39.179688 L 35.666016 41.324219 C 36.633669 42.291872 38.233595 42.291935 39.201172 41.324219 L 41.322266 39.203125 C 42.290377 38.235014 42.291356 36.635406 41.322266 35.667969 L 39.177734 33.523438 C 40.232026 31.844568 41.006723 29.989968 41.460938 28 L 44.498047 28 C 45.86711 28 46.998047 26.869063 46.998047 25.5 L 46.998047 22.501953 L 46.998047 22.5 C 47.000284 21.130507 45.868417 20 44.5 20 L 41.462891 20 C 41.008258 18.010362 40.233888 16.155535 39.179688 14.476562 L 41.324219 12.332031 C 42.29233 11.36392 42.293309 9.7643118 41.324219 8.796875 L 39.205078 6.6757812 C 38.237369 5.7080724 36.635678 5.7080724 35.667969 6.6757812 L 33.523438 8.8203125 C 31.844075 7.7661119 29.989786 6.9919346 28 6.5371094 L 28 3.5 C 28 2.1309372 26.869063 1 25.5 1 L 22.5 1 z M 22.5 3 L 25.5 3 C 25.786937 3 26 3.2130628 26 3.5 L 26 7.2792969 A 1.0001 1.0001 0 0 0 26.824219 8.2636719 C 29.139966 8.6770973 31.279492 9.5803763 33.134766 10.873047 A 1.0001 1.0001 0 0 0 34.414062 10.759766 L 37.083984 8.0898438 C 37.288275 7.8855527 37.584772 7.8855527 37.789062 8.0898438 L 39.910156 10.210938 A 1.0001 1.0001 0 0 0 39.912109 10.210938 C 40.115019 10.4135 40.114092 10.71408 39.910156 10.917969 L 37.240234 13.587891 A 1.0001 1.0001 0 0 0 37.126953 14.867188 C 38.419347 16.721122 39.322857 18.859775 39.736328 21.175781 A 1.0001 1.0001 0 0 0 40.720703 22 L 44.5 22 C 44.786937 22 44.998737 22.212412 44.998047 22.498047 A 1.0001 1.0001 0 0 0 44.998047 22.5 L 44.998047 25.5 C 44.998047 25.786937 44.784984 26 44.498047 26 L 40.71875 26 A 1.0001 1.0001 0 0 0 39.734375 26.824219 C 39.321939 29.139649 38.417524 31.27869 37.125 33.132812 A 1.0001 1.0001 0 0 0 37.238281 34.412109 L 39.908203 37.082031 A 1.0001 1.0001 0 0 0 39.910156 37.082031 C 40.113066 37.284594 40.112139 37.585175 39.908203 37.789062 L 37.787109 39.910156 C 37.584546 40.113066 37.28592 40.114045 37.082031 39.910156 L 34.410156 37.240234 A 1.0001 1.0001 0 0 0 33.132812 37.126953 C 31.278878 38.419347 29.140225 39.320903 26.824219 39.734375 A 1.0001 1.0001 0 0 0 25.998047 40.71875 L 25.998047 44.498047 C 25.998047 44.784984 25.784984 44.998047 25.498047 44.998047 L 22.498047 44.998047 C 22.21111 44.998047 21.998047 44.784984 21.998047 44.498047 L 21.998047 40.71875 A 1.0001 1.0001 0 0 0 21.173828 39.734375 C 18.85835 39.321939 16.719357 38.419477 14.865234 37.126953 A 1.0001 1.0001 0 0 0 13.585938 37.240234 L 10.916016 39.910156 C 10.711725 40.114447 10.415229 40.114447 10.210938 39.910156 L 8.0898438 37.789062 A 1.0001 1.0001 0 0 0 8.0878906 37.789062 C 7.884978 37.5865 7.8859549 37.28592 8.0898438 37.082031 L 10.759766 34.412109 A 1.0001 1.0001 0 0 0 10.873047 33.132812 C 9.5806532 31.278878 8.6771435 29.140225 8.2636719 26.824219 A 1.0001 1.0001 0 0 0 7.2792969 26 L 3.5 26 C 3.2130628 26 3 25.786937 3 25.5 L 3 22.5 C 3 22.213063 3.2130628 22 3.5 22 L 7.2792969 22 A 1.0001 1.0001 0 0 0 8.2636719 21.175781 C 8.6770973 18.860034 9.580523 16.72131 10.873047 14.867188 A 1.0001 1.0001 0 0 0 10.759766 13.587891 L 8.0898438 10.917969 C 7.8869343 10.715406 7.8859548 10.414825 8.0898438 10.210938 L 10.210938 8.0898438 A 1.0001 1.0001 0 0 0 10.210938 8.0878906 C 10.4135 7.8849812 10.71408 7.8859581 10.917969 8.0898438 L 13.587891 10.759766 A 1.0001 1.0001 0 0 0 14.867188 10.873047 C 16.721122 9.5806532 18.859775 8.6771435 21.175781 8.2636719 A 1.0001 1.0001 0 0 0 22 7.2792969 L 22 3.5 C 22 3.2130628 22.213063 3 22.5 3 z M 24 14 C 18.488994 14 14 18.488998 14 24 C 14 29.511002 18.488994 34 24 34 C 29.511006 34 34 29.511002 34 24 C 34 18.488998 29.511006 14 24 14 z M 24 16 C 28.430126 16 32 19.569877 32 24 C 32 28.430123 28.430126 32 24 32 C 19.569874 32 16 28.430123 16 24 C 16 19.569877 19.569874 16 24 16 z"></path> </svg>)")}));
    return messages;
}

class CaptureRelayDeskRuntime : public relaydesk::runtime::RelayDeskRuntime {
public:
    CaptureRelayDeskRuntime()
        : RelayDeskRuntime(makeCaptureRuntimeOptions())
    {
        localUser_.SetDisplayName("许靖");
        localUser_.SetHostName("MENG");
        localUser_.SetDeviceId("capture-local");

        relaydesk::runtime::PeerListItem peer;
        peer.SetDeviceId("capture-peer");
        peer.SetDisplayName("Alex-PC");
        peer.SetHostName("Alex-PC");
        peer.SetAddress("192.168.1.24");
        peer.SetOnline(true);
        peers_.clear();
        peers_.push_back(std::move(peer));
        selectedPeerDeviceId_ = "capture-peer";
        selectedPeerHasMoreMessages_ = false;
        selectedPeerMessages_ = makeCaptureChatMessages();
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
        } else if (argument == L"--capture-initial-width") {
            valid = index + 1 < argc &&
                    parsePositiveInt(argv[++index], options.initialWidth);
        } else if (argument == L"--capture-initial-height") {
            valid = index + 1 < argc &&
                    parsePositiveInt(argv[++index], options.initialHeight);
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
    const int initialWidth =
        options.initialWidth > 0 ? options.initialWidth : options.width;
    const int initialHeight =
        options.initialHeight > 0 ? options.initialHeight : options.height;
    runtime.resize(initialWidth, initialHeight, options.dpiScale);
    if (!runtime.loadDocumentFromString(html)) {
        return 4;
    }
    applyRelayDeskDevicePanel(runtime, relayRuntime);
    if (initialWidth != options.width || initialHeight != options.height) {
        runtime.resize(options.width, options.height, options.dpiScale);
        applyRelayDeskDevicePanel(runtime, relayRuntime);
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
        if (!*documentLoaded) {
            if (!runtime.loadDocumentFromString(*html)) {
                return;
            }
            *documentLoaded = true;
            binding->documentLoaded = true;
        }
        (void)refreshRelayDeskDevicePanelIfChanged(*binding, true);
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
