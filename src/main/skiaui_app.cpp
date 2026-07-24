#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdio>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
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
#include "core/uuid.h"
#include "main/app_runtime.h"
#include "main/image_attachment_store.h"
#include "main/skiaui_background.h"
#include "platform/attachment_input.h"
#include "platform/text_encoding.h"
#include "storage/app_paths.h"

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
constexpr std::size_t kCaptureHistoryPageSize = 30u;
constexpr std::size_t kCaptureHistoryMessageCount = 90u;
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
constexpr int kImageContextMenuWidth = 160;
constexpr int kImageContextMenuHeight = 114;
constexpr int kFileContextMenuWidth = 160;
constexpr int kFileContextMenuHeight = 114;
constexpr float kMessageImageMaxWidth = 420.0f;
constexpr float kMessageImageMaxHeight = 300.0f;
constexpr float kChatScrollBottomTolerance = 0.5f;
constexpr float kChatLoadMoreTopThreshold = 36.0f;
constexpr std::size_t kMaxComposerAttachmentCount = 8u;
constexpr unsigned int kComposerImageMaximumWidth = 260u;
constexpr unsigned int kComposerImageMaximumHeight = 140u;
constexpr std::string_view kComposerDocumentId = "composer-document";
constexpr std::string_view kComposerInitialParagraphId =
    "composer-text-initial";
constexpr std::string_view kComposerAttachmentElementPrefix =
    "composer-attachment:";
constexpr std::string_view kTransferSendActionPrefix = "transfer-send:";
constexpr std::string_view kTransferResendMessageActionPrefix =
    "transfer-resend-message:";
constexpr std::string_view kTransferCancelActionPrefix = "transfer-cancel:";
constexpr std::string_view kTransferAcceptActionPrefix = "transfer-accept:";
constexpr std::string_view kTransferSaveAsActionPrefix = "transfer-save-as:";
constexpr std::string_view kTransferOverwriteActionPrefix =
    "transfer-overwrite:";
constexpr std::string_view kTransferRejectActionPrefix = "transfer-reject:";
constexpr std::string_view kTransferOpenActionPrefix = "transfer-open:";
constexpr std::string_view kTransferRevealActionPrefix = "transfer-reveal:";
constexpr UINT kSkiaUiRequestRedrawMessage = WM_APP + 0x531;
constexpr UINT kRelayDeskSkiaUiRefreshMs = 100;

enum class ComposerAttachmentKind {
    Image,
    File,
    Folder,
};

enum class ChatScrollUpdateMode {
    PreserveOffset,
    ScrollToLatest,
    PreserveViewportAfterPrepend,
};

struct ComposerAttachment {
    ComposerAttachmentKind kind = ComposerAttachmentKind::File;
    std::string attachmentId;
    std::string displayName;
    std::string localPath;
    std::string sha256;
    std::filesystem::path sourcePath;
    std::filesystem::path previewPath;
    std::uintmax_t fileSize = 0;
    bool fileSizePending = false;
};

struct PendingFolderSizeResult {
    std::string attachmentId;
    std::filesystem::path sourcePath;
    std::uintmax_t fileSize = 0;
};

struct OpenableTransferPath {
    std::filesystem::path path;
    bool folder = false;
};

struct TransferPartActionTarget {
    std::string messageId;
    std::string partId;
};

struct CaptureOptions {
    std::filesystem::path outputPath;
    int width = kDefaultCaptureWidth;
    int height = kDefaultCaptureHeight;
    float dpiScale = kDefaultCaptureDpiScale;
    int initialWidth = 0;
    int initialHeight = 0;
    bool testPeerSwitch = false;
    bool testImageMessage = false;
    bool testFileCard = false;
    bool testComposerAttachments = false;
    bool testComposerKeyboard = false;
    bool testHistoryPagination = false;
};

struct SkiaUiRuntimeBinding {
    relaydesk::runtime::RelayDeskRuntime* relayRuntime = nullptr;
    relaydesk::skiaui::BackgroundController* backgroundController = nullptr;
    skui::Runtime* skiaRuntime = nullptr;
    HWND window = nullptr;
    UINT_PTR timerId = 0;
    UINT_PTR deferredRefreshTimerId = 0;
    bool documentLoaded = false;
    bool chatInitialized = false;
    bool chatHistoryPrependPending = false;
    bool suppressChatHistoryPagination = false;
    std::function<void()> loadMoreSelectedPeerMessages;
    std::function<std::vector<std::filesystem::path>()>
        readClipboardAttachmentPaths;
    std::vector<ComposerAttachment> composerAttachments;
    skui::Selection composerSelection;
    std::string lastDeviceSignature;
};

SkiaUiRuntimeBinding* gRuntimeBinding = nullptr;
std::string gMessageContextText;
std::filesystem::path gImageContextPath;
std::filesystem::path gFileContextPath;
bool gMessageContextMenuVisible = false;
bool gImagePreviewVisible = false;
std::vector<PendingFolderSizeResult> gPendingFolderSizeResults;

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

std::string escapeHtmlMultilineAttribute(std::string_view text)
{
    const std::string escaped = escapeHtml(text);
    std::string value;
    value.reserve(escaped.size());
    for (std::size_t index = 0u; index < escaped.size(); ++index) {
        const char character = escaped[index];
        if (character == '\r') {
            if (index + 1u < escaped.size() && escaped[index + 1u] == '\n') {
                continue;
            }
            value += "&#10;";
        } else if (character == '\n') {
            value += "&#10;";
        } else {
            value += character;
        }
    }
    return value;
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

std::filesystem::path filesystemPathFromUtf8(std::string_view pathText)
{
    return std::filesystem::path(
        relaydesk::platform::utf8ToWide(std::string(pathText)));
}

std::optional<std::filesystem::path> tryFilesystemPathFromUtf8(
    std::string_view pathText)
{
    try {
        std::filesystem::path filePath = filesystemPathFromUtf8(pathText);
        filePath.make_preferred();
        return filePath;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string filesystemPathToGenericUtf8(
    const std::filesystem::path& filePath)
{
    const auto value = filePath.generic_u8string();
    return std::string(value.begin(), value.end());
}

std::filesystem::path resolveWorkRelativePath(
    const relaydesk::storage::AppPaths& appPaths,
    std::string_view pathText)
{
    std::filesystem::path filePath = filesystemPathFromUtf8(pathText);
    if (filePath.is_relative()) {
        filePath = appPaths.GetWorkDirectory() / filePath;
    }
    return filePath.lexically_normal();
}

bool isParentTraversalPath(const std::filesystem::path& filePath)
{
    const auto begin = filePath.begin();
    return begin != filePath.end() && *begin == "..";
}

std::string makeAttachmentLocalPath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& filePath)
{
    std::error_code error;
    const std::filesystem::path relativePath =
        std::filesystem::relative(filePath, appPaths.GetWorkDirectory(), error);
    if (!error && !relativePath.empty() && !relativePath.is_absolute() &&
        !isParentTraversalPath(relativePath)) {
        return filesystemPathToGenericUtf8(relativePath);
    }
    return filesystemPathToGenericUtf8(filePath);
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool isImageAttachmentPath(const std::filesystem::path& filePath)
{
    const std::string extension = lowerAscii(filePath.extension().string());
    return extension == ".png" || extension == ".jpg" ||
        extension == ".jpeg" || extension == ".gif" ||
        extension == ".webp" || extension == ".bmp";
}

std::filesystem::path makeAbsolutePath(
    const std::filesystem::path& filePath)
{
    std::error_code error;
    std::filesystem::path absolutePath =
        std::filesystem::absolute(filePath, error);
    if (error) {
        return filePath.lexically_normal();
    }
    return absolutePath.lexically_normal();
}

std::uintmax_t fileSizeOrZero(const std::filesystem::path& filePath)
{
    std::error_code error;
    const std::uintmax_t fileSize =
        std::filesystem::file_size(filePath, error);
    return error ? 0u : fileSize;
}

std::uintmax_t directoryContentSizeOrZero(
    const std::filesystem::path& directory,
    const core::async::CancelToken& cancelToken)
{
    std::uintmax_t totalSize = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end && !cancelToken.canceled()) {
        std::error_code entryError;
        if (iterator->is_regular_file(entryError) && !entryError) {
            const std::uintmax_t fileSize =
                std::filesystem::file_size(iterator->path(), entryError);
            if (!entryError &&
                fileSize <= std::numeric_limits<std::uintmax_t>::max() -
                    totalSize) {
                totalSize += fileSize;
            }
        }
        iterator.increment(error);
    }
    return totalSize;
}

std::string formatFileSize(std::uintmax_t fileSize);

std::string composerFolderSizeTaskKey(std::string_view attachmentId)
{
    return "composer-folder-size:" + std::string(attachmentId);
}

std::vector<PendingFolderSizeResult> consumePendingFolderSizeResults()
{
    std::vector<PendingFolderSizeResult> results;
    results.swap(gPendingFolderSizeResults);
    return results;
}

void startPendingFolderSizeProbe(const ComposerAttachment& attachment)
{
    if (attachment.kind != ComposerAttachmentKind::Folder ||
        attachment.attachmentId.empty()) {
        return;
    }

    const std::string attachmentId = attachment.attachmentId;
    const std::filesystem::path sourcePath = attachment.sourcePath;
    (void)core::async::runOnce(
        composerFolderSizeTaskKey(attachmentId),
        [attachmentId, sourcePath](
            const core::async::CancelToken& cancelToken) {
            PendingFolderSizeResult result;
            result.attachmentId = attachmentId;
            result.sourcePath = sourcePath;
            result.fileSize = directoryContentSizeOrZero(
                sourcePath, cancelToken);
            return result;
        },
        [](const core::async::Result<PendingFolderSizeResult>& result) {
            if (result.ok) {
                gPendingFolderSizeResults.push_back(result.value);
            }
        });
}

std::size_t composerAttachmentCount(const SkiaUiRuntimeBinding& binding)
{
    return binding.composerAttachments.size();
}

ComposerAttachment* findComposerAttachment(
    SkiaUiRuntimeBinding& binding,
    std::string_view attachmentId)
{
    const auto attachment = std::find_if(
        binding.composerAttachments.begin(),
        binding.composerAttachments.end(),
        [attachmentId](const ComposerAttachment& item) {
            return item.attachmentId == attachmentId;
        });
    return attachment == binding.composerAttachments.end()
        ? nullptr
        : &*attachment;
}

const ComposerAttachment* findComposerAttachment(
    const SkiaUiRuntimeBinding& binding,
    std::string_view attachmentId)
{
    const auto attachment = std::find_if(
        binding.composerAttachments.begin(),
        binding.composerAttachments.end(),
        [attachmentId](const ComposerAttachment& item) {
            return item.attachmentId == attachmentId;
        });
    return attachment == binding.composerAttachments.end()
        ? nullptr
        : &*attachment;
}

std::string composerAttachmentElementId(std::string_view attachmentId)
{
    return std::string(kComposerAttachmentElementPrefix) +
        std::string(attachmentId);
}

std::optional<std::string_view> composerAttachmentIdFromElementId(
    std::string_view elementId)
{
    if (!elementId.starts_with(kComposerAttachmentElementPrefix)) {
        return std::nullopt;
    }
    return elementId.substr(kComposerAttachmentElementPrefix.size());
}

std::vector<std::string> pollPendingFolderSizeResults(
    SkiaUiRuntimeBinding& binding)
{
    std::vector<std::string> changedAttachmentIds;
    for (const PendingFolderSizeResult& result :
         consumePendingFolderSizeResults()) {
        ComposerAttachment* attachment =
            findComposerAttachment(binding, result.attachmentId);
        if (attachment == nullptr ||
            attachment->kind != ComposerAttachmentKind::Folder ||
            attachment->sourcePath != result.sourcePath) {
            continue;
        }
        attachment->fileSize = result.fileSize;
        attachment->fileSizePending = false;
        changedAttachmentIds.push_back(attachment->attachmentId);
    }
    return changedAttachmentIds;
}

std::optional<ComposerAttachment> makeComposerAttachmentFromPath(
    const std::filesystem::path& filePath)
{
    std::error_code error;
    const bool regularFile = std::filesystem::is_regular_file(filePath, error);
    if (error) {
        return std::nullopt;
    }
    error.clear();
    const bool directory = std::filesystem::is_directory(filePath, error);
    if (error || (!regularFile && !directory)) {
        return std::nullopt;
    }

    const std::filesystem::path absolutePath = makeAbsolutePath(filePath);
    const bool image = regularFile && isImageAttachmentPath(absolutePath);
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();

    ComposerAttachment attachment;
    attachment.attachmentId = relaydesk::core::createUuidV4();
    attachment.kind = directory ? ComposerAttachmentKind::Folder
                                : (image ? ComposerAttachmentKind::Image
                                         : ComposerAttachmentKind::File);
    attachment.displayName =
        filesystemPathToGenericUtf8(absolutePath.filename());
    attachment.sourcePath = absolutePath;
    attachment.previewPath = absolutePath;
    attachment.localPath = makeAttachmentLocalPath(appPaths, absolutePath);
    attachment.fileSizePending = directory;
    if (!directory) {
        attachment.fileSize = fileSizeOrZero(absolutePath);
    }

    if (image) {
        try {
            relaydesk::storage::ensureAppDirectories(appPaths);
            const std::optional<relaydesk::runtime::StoredImageAttachment>
                storedImage =
                    relaydesk::runtime::storePreviewableImageAttachment(
                        appPaths,
                        absolutePath,
                        attachment.displayName,
                        false);
            if (storedImage.has_value()) {
                attachment.sourcePath = storedImage->GetImagePath();
                attachment.previewPath =
                    storedImage->GetThumbnailPath().value_or(
                        attachment.sourcePath);
                attachment.localPath = makeAttachmentLocalPath(
                    appPaths, attachment.sourcePath);
                attachment.sha256 = storedImage->GetSha256();
                attachment.fileSize = fileSizeOrZero(attachment.sourcePath);
            }
        } catch (const std::exception&) {
        }
    }
    return attachment;
}

std::string composerAttachmentDetailText(
    const ComposerAttachment& attachment)
{
    if (attachment.fileSizePending) {
        return "正在计算容量...";
    }
    if (attachment.kind == ComposerAttachmentKind::Folder) {
        return "文件夹 · " + formatFileSize(attachment.fileSize);
    }
    if (attachment.kind == ComposerAttachmentKind::Image) {
        return "图片 · " + formatFileSize(attachment.fileSize);
    }
    return formatFileSize(attachment.fileSize);
}

std::string makeClipboardAttachmentAttributes(
    ComposerAttachmentKind kind,
    const std::filesystem::path& sourcePath,
    std::string_view displayName)
{
    if (sourcePath.empty()) {
        return {};
    }
    std::string attributes = R"( data-clipboard-kind=")";
    attributes += kind == ComposerAttachmentKind::Image ? "image" : "file";
    attributes += R"(" data-clipboard-path=")";
    attributes += escapeHtml(filesystemPathToGenericUtf8(sourcePath));
    attributes += R"(" data-clipboard-name=")";
    attributes += escapeHtml(displayName);
    attributes += '"';
    return attributes;
}

relaydesk::platform::ImageSize composerImageDisplaySize(
    const std::filesystem::path& imagePath)
{
    const std::optional<relaydesk::platform::ImageSize> imageSize =
        relaydesk::platform::probeImageSize(imagePath);
    if (!imageSize.has_value() || imageSize->width == 0u ||
        imageSize->height == 0u) {
        return {160u, 100u};
    }

    const double scale = std::min({
        1.0,
        static_cast<double>(kComposerImageMaximumWidth) /
            static_cast<double>(imageSize->width),
        static_cast<double>(kComposerImageMaximumHeight) /
            static_cast<double>(imageSize->height),
    });
    return {
        std::max(1u, static_cast<unsigned int>(
                         std::lround(imageSize->width * scale))),
        std::max(1u, static_cast<unsigned int>(
                         std::lround(imageSize->height * scale))),
    };
}

std::string makeComposerAttachmentMarkup(
    const ComposerAttachment& attachment)
{
    const std::string elementId =
        composerAttachmentElementId(attachment.attachmentId);
    if (attachment.kind == ComposerAttachmentKind::Image) {
        const std::filesystem::path& imagePath =
            attachment.previewPath.empty()
            ? attachment.sourcePath
            : attachment.previewPath;
        const relaydesk::platform::ImageSize displaySize =
            composerImageDisplaySize(imagePath);
        const std::string imagePathText =
            filesystemPathToGenericUtf8(imagePath);
        std::string html = R"(<div id=")";
        html += escapeHtml(elementId);
        html += R"(" class="composer-attachment-image" contenteditable="false" data-node-type="attachment" data-attachment-id=")";
        html += escapeHtml(attachment.attachmentId);
        html += '"';
        html += makeClipboardAttachmentAttributes(
            attachment.kind, attachment.sourcePath, attachment.displayName);
        html += R"( data-action="image-context:)";
        html += escapeHtml(imagePathText);
        html += R"(" style="width: )";
        html += std::to_string(displaySize.width);
        html += R"(px; height: )";
        html += std::to_string(displaySize.height);
        html += R"(px;"><img class="composer-attachment-preview" src=")";
        html += escapeHtml(imagePathText);
        html += R"(" alt="" style="width: )";
        html += std::to_string(displaySize.width);
        html += R"(px; height: )";
        html += std::to_string(displaySize.height);
        html += R"(px;"><div class="composer-attachment-remove" title="移除附件" data-action="remove-attachment:)";
        html += escapeHtml(attachment.attachmentId);
        html += R"(">×</div></div>)";
        return html;
    }

    std::string html = R"(<div id=")";
    html += escapeHtml(elementId);
    html += R"(" class="composer-attachment-card" contenteditable="false" data-node-type="attachment" data-attachment-id=")";
    html += escapeHtml(attachment.attachmentId);
    html += '"';
    html += makeClipboardAttachmentAttributes(
        attachment.kind, attachment.sourcePath, attachment.displayName);
    html += '>';
    html += attachment.kind == ComposerAttachmentKind::Folder
        ? R"(<svg class="composer-attachment-icon folder" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h6l2 2h10v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"></path></svg>)"
        : R"(<svg class="composer-attachment-icon file" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M6 2h8l4 4v16H6Z"></path><path d="M14 2v5h5"></path></svg>)";
    html += R"(<div class="composer-attachment-copy"><div class="composer-attachment-name" title=")";
    html += escapeHtml(attachment.displayName);
    html += R"(">)";
    html += escapeHtml(truncateUtf8Bytes(attachment.displayName, 32u));
    html += R"(</div><div class="composer-attachment-detail)";
    if (attachment.fileSizePending) {
        html += " pending";
    }
    html += R"(">)";
    html += escapeHtml(composerAttachmentDetailText(attachment));
    html += R"(</div></div><div class="composer-attachment-remove" title="移除附件" data-action="remove-attachment:)";
    html += escapeHtml(attachment.attachmentId);
    html += R"(">×</div></div>)";
    return html;
}

std::string makeComposerAttachmentMarkup(
    const std::vector<ComposerAttachment>& attachments)
{
    std::string html;
    for (const ComposerAttachment& attachment : attachments) {
        html += makeComposerAttachmentMarkup(attachment);
    }
    return html;
}

void rememberComposerSelection(skui::Runtime& runtime,
                               SkiaUiRuntimeBinding& binding)
{
    const skui::Selection selection = runtime.selection();
    if (selection.rangeCount == 0) {
        return;
    }
    const std::vector<std::string> children =
        runtime.childElementIdsById(kComposerDocumentId);
    if (std::find(children.begin(), children.end(), selection.anchorNodeId) !=
            children.end() &&
        std::find(children.begin(), children.end(), selection.focusNodeId) !=
            children.end()) {
        binding.composerSelection = selection;
    }
}

bool restoreComposerSelection(skui::Runtime& runtime,
                              const SkiaUiRuntimeBinding& binding)
{
    const skui::Selection& selection = binding.composerSelection;
    if (selection.rangeCount > 0 &&
        runtime.setSelectionBaseAndExtent(selection.anchorNodeId,
                                          selection.anchorOffset,
                                          selection.focusNodeId,
                                          selection.focusOffset)) {
        return true;
    }

    const std::vector<std::string> children =
        runtime.childElementIdsById(kComposerDocumentId);
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
        if (composerAttachmentIdFromElementId(*child).has_value()) {
            continue;
        }
        const std::optional<std::string> text =
            runtime.textContentById(*child);
        if (text.has_value() &&
            runtime.collapseSelection(*child, text->size())) {
            return true;
        }
    }
    return runtime.collapseSelection(kComposerInitialParagraphId, 0u);
}

void discardComposerAttachmentsMissingFromDocument(
    skui::Runtime& runtime,
    SkiaUiRuntimeBinding& binding)
{
    const std::vector<std::string> children =
        runtime.childElementIdsById(kComposerDocumentId);
    for (const ComposerAttachment& attachment :
         binding.composerAttachments) {
        const std::string elementId =
            composerAttachmentElementId(attachment.attachmentId);
        if (std::find(children.begin(), children.end(), elementId) ==
            children.end()) {
            (void)core::async::cancel(
                composerFolderSizeTaskKey(attachment.attachmentId));
        }
    }
    binding.composerAttachments.erase(
        std::remove_if(
            binding.composerAttachments.begin(),
            binding.composerAttachments.end(),
            [&children](const ComposerAttachment& attachment) {
                const std::string elementId =
                    composerAttachmentElementId(attachment.attachmentId);
                return std::find(children.begin(), children.end(), elementId) ==
                    children.end();
            }),
        binding.composerAttachments.end());
}

bool addComposerAttachmentPaths(
    skui::Runtime& runtime,
    SkiaUiRuntimeBinding& binding,
    const std::vector<std::filesystem::path>& filePaths)
{
    std::vector<ComposerAttachment> attachments;
    const std::size_t available = kMaxComposerAttachmentCount -
        std::min(kMaxComposerAttachmentCount, composerAttachmentCount(binding));
    attachments.reserve(std::min(available, filePaths.size()));
    for (const std::filesystem::path& filePath : filePaths) {
        if (attachments.size() >= available) {
            break;
        }
        try {
            std::optional<ComposerAttachment> attachment =
                makeComposerAttachmentFromPath(filePath);
            if (attachment.has_value()) {
                attachments.push_back(std::move(attachment.value()));
            }
        } catch (const std::exception&) {
        }
    }
    if (attachments.empty() || !restoreComposerSelection(runtime, binding)) {
        return false;
    }

    std::string markup;
    for (const ComposerAttachment& attachment : attachments) {
        markup += makeComposerAttachmentMarkup(attachment);
    }
    if (!runtime.insertHtmlAtSelection(kComposerDocumentId, markup)) {
        return false;
    }

    for (ComposerAttachment& attachment : attachments) {
        binding.composerAttachments.push_back(std::move(attachment));
        const ComposerAttachment& added = binding.composerAttachments.back();
        if (added.fileSizePending) {
            startPendingFolderSizeProbe(added);
        }
    }
    rememberComposerSelection(runtime, binding);
    return true;
}

int clipboardHexDigit(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

std::optional<std::filesystem::path> clipboardSourcePath(
    std::string_view source)
{
    std::string pathText(source);
    if (pathText.starts_with("file:///")) {
        pathText.erase(0, 8u);
    } else if (pathText.starts_with("file://")) {
        pathText = "//" + pathText.substr(7u);
    } else if (pathText.find("://") != std::string::npos ||
               pathText.starts_with("data:")) {
        return std::nullopt;
    }

    std::string decoded;
    decoded.reserve(pathText.size());
    for (std::size_t index = 0; index < pathText.size(); ++index) {
        if (pathText[index] != '%' || index + 2u >= pathText.size()) {
            decoded.push_back(pathText[index]);
            continue;
        }
        const int high = clipboardHexDigit(pathText[index + 1u]);
        const int low = clipboardHexDigit(pathText[index + 2u]);
        if (high < 0 || low < 0) {
            decoded.push_back(pathText[index]);
            continue;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2u;
    }
    return tryFilesystemPathFromUtf8(decoded);
}

std::string makeComposerClipboardTextMarkup(std::string_view text)
{
    std::string markup;
    std::size_t lineStart = 0u;
    do {
        const std::size_t lineEnd = text.find('\n', lineStart);
        std::string_view line = text.substr(
            lineStart,
            lineEnd == std::string_view::npos
                ? std::string_view::npos
                : lineEnd - lineStart);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1u);
        }
        markup += R"(<p id="composer-clipboard-text-)";
        markup += relaydesk::core::createUuidV4();
        markup += R"(" class="composer-document-paragraph">)";
        markup += line.empty() ? "<br>" : escapeHtml(line);
        markup += "</p>";
        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 1u;
    } while (lineStart <= text.size());
    return markup;
}

bool pasteComposerClipboardContent(
    skui::Runtime& runtime,
    SkiaUiRuntimeBinding& binding,
    const skui::ClipboardContent& content)
{
    if (content.items.empty() ||
        !restoreComposerSelection(runtime, binding)) {
        return false;
    }

    std::string markup;
    std::vector<ComposerAttachment> attachments;
    std::size_t fallbackPathIndex = 0u;
    bool previousMarkupWasAttachment = false;
    const std::size_t available = kMaxComposerAttachmentCount -
        std::min(kMaxComposerAttachmentCount, composerAttachmentCount(binding));
    for (std::size_t itemIndex = 0u;
         itemIndex < content.items.size();
         ++itemIndex) {
        const skui::ClipboardItem& item = content.items[itemIndex];
        if (item.type == skui::ClipboardItemType::Text) {
            std::string_view text = item.text;
            if (previousMarkupWasAttachment && text.starts_with("\r\n")) {
                text.remove_prefix(2u);
            } else if (previousMarkupWasAttachment && !text.empty() &&
                       (text.front() == '\r' || text.front() == '\n')) {
                text.remove_prefix(1u);
            }
            const bool nextItemIsAttachment =
                itemIndex + 1u < content.items.size() &&
                content.items[itemIndex + 1u].type !=
                    skui::ClipboardItemType::Text;
            if (nextItemIsAttachment && text.ends_with("\r\n")) {
                text.remove_suffix(2u);
            } else if (nextItemIsAttachment && !text.empty() &&
                       (text.back() == '\r' || text.back() == '\n')) {
                text.remove_suffix(1u);
            }
            if (!text.empty()) {
                markup += makeComposerClipboardTextMarkup(text);
                previousMarkupWasAttachment = false;
            }
            continue;
        }
        if (attachments.size() >= available) {
            continue;
        }

        std::optional<std::filesystem::path> sourcePath =
            clipboardSourcePath(item.source);
        if (!sourcePath.has_value() &&
            fallbackPathIndex < content.filePaths.size()) {
            sourcePath = tryFilesystemPathFromUtf8(
                content.filePaths[fallbackPathIndex++]);
        }
        if (!sourcePath.has_value()) {
            if (!item.text.empty()) {
                markup += makeComposerClipboardTextMarkup(item.text);
                previousMarkupWasAttachment = false;
            }
            continue;
        }

        try {
            std::optional<ComposerAttachment> attachment =
                makeComposerAttachmentFromPath(sourcePath.value());
            if (!attachment.has_value()) {
                continue;
            }
            markup += makeComposerAttachmentMarkup(attachment.value());
            attachments.push_back(std::move(attachment.value()));
            previousMarkupWasAttachment = true;
        } catch (const std::exception&) {
        }
    }
    if (markup.empty() ||
        !runtime.insertHtmlAtSelection(kComposerDocumentId, markup)) {
        return false;
    }

    for (ComposerAttachment& attachment : attachments) {
        binding.composerAttachments.push_back(std::move(attachment));
        const ComposerAttachment& added = binding.composerAttachments.back();
        if (added.fileSizePending) {
            startPendingFolderSizeProbe(added);
        }
    }
    rememberComposerSelection(runtime, binding);
    return true;
}

bool removeComposerAttachment(skui::Runtime& runtime,
                              SkiaUiRuntimeBinding& binding,
                              std::string_view attachmentId)
{
    ComposerAttachment* attachment =
        findComposerAttachment(binding, attachmentId);
    if (attachment == nullptr ||
        !runtime.removeElementById(
            composerAttachmentElementId(attachmentId))) {
        return false;
    }
    (void)core::async::cancel(composerFolderSizeTaskKey(attachmentId));
    binding.composerAttachments.erase(
        std::remove_if(
            binding.composerAttachments.begin(),
            binding.composerAttachments.end(),
            [attachmentId](const ComposerAttachment& item) {
                return item.attachmentId == attachmentId;
            }),
        binding.composerAttachments.end());
    return true;
}

void refreshComposerAttachmentCards(
    skui::Runtime& runtime,
    const SkiaUiRuntimeBinding& binding,
    const std::vector<std::string>& attachmentIds)
{
    for (const std::string& attachmentId : attachmentIds) {
        const ComposerAttachment* attachment =
            findComposerAttachment(binding, attachmentId);
        if (attachment == nullptr) {
            continue;
        }
        (void)runtime.replaceHtmlById(
            composerAttachmentElementId(attachmentId),
            makeComposerAttachmentMarkup(*attachment));
    }
}

std::optional<relaydesk::storage::ChatMessagePart> makeComposerAttachmentPart(
    const ComposerAttachment& attachment)
{
    std::string localPath = attachment.localPath;
    std::uintmax_t fileSize = attachment.fileSize;
    try {
        if (localPath.empty()) {
            localPath = makeAttachmentLocalPath(
                relaydesk::storage::createAppPaths(), attachment.sourcePath);
        }
        if (attachment.kind != ComposerAttachmentKind::Folder) {
            fileSize = fileSizeOrZero(attachment.sourcePath);
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }

    relaydesk::storage::ChatMessagePart part;
    switch (attachment.kind) {
    case ComposerAttachmentKind::Image:
        part.SetType(relaydesk::storage::MessagePartType::Image);
        break;
    case ComposerAttachmentKind::File:
        part.SetType(relaydesk::storage::MessagePartType::File);
        break;
    case ComposerAttachmentKind::Folder:
        part.SetType(relaydesk::storage::MessagePartType::Folder);
        break;
    }
    part.SetTransferId(relaydesk::core::createUuidV4());
    part.SetTransferState(relaydesk::storage::TransferState::Pending);
    part.SetFileName(attachment.displayName);
    part.SetFileSize(fileSize);
    part.SetTransferredSize(0u);
    if (!attachment.sha256.empty()) {
        part.SetSha256(attachment.sha256);
    }
    part.SetLocalPath(localPath);
    return part;
}

void appendComposerTextPart(
    std::vector<relaydesk::storage::ChatMessagePart>& parts,
    std::vector<std::string>& paragraphs)
{
    std::size_t first = 0u;
    while (first < paragraphs.size() &&
           trimMessageWhitespace(paragraphs[first]).empty()) {
        ++first;
    }
    std::size_t last = paragraphs.size();
    while (last > first &&
           trimMessageWhitespace(paragraphs[last - 1u]).empty()) {
        --last;
    }
    if (first == last) {
        paragraphs.clear();
        return;
    }

    std::string text = paragraphs[first];
    for (std::size_t index = first + 1u; index < last; ++index) {
        text += '\n';
        text += paragraphs[index];
    }
    relaydesk::storage::ChatMessagePart part;
    part.SetType(relaydesk::storage::MessagePartType::Text);
    part.SetText(std::move(text));
    parts.push_back(std::move(part));
    paragraphs.clear();
}

std::vector<relaydesk::storage::ChatMessagePart> makeComposerMessageParts(
    const skui::Runtime& runtime,
    const SkiaUiRuntimeBinding& binding)
{
    std::vector<relaydesk::storage::ChatMessagePart> parts;
    std::vector<std::string> textParagraphs;
    for (const std::string& elementId :
         runtime.childElementIdsById(kComposerDocumentId)) {
        const std::optional<std::string_view> attachmentId =
            composerAttachmentIdFromElementId(elementId);
        if (!attachmentId.has_value()) {
            const std::optional<std::string> text =
                runtime.textContentById(elementId);
            if (!text.has_value()) {
                continue;
            }
            textParagraphs.push_back(text.value());
            continue;
        }

        appendComposerTextPart(parts, textParagraphs);
        const ComposerAttachment* attachment =
            findComposerAttachment(binding, attachmentId.value());
        if (attachment == nullptr) {
            continue;
        }
        std::optional<relaydesk::storage::ChatMessagePart> part =
            makeComposerAttachmentPart(*attachment);
        if (part.has_value()) {
            parts.push_back(std::move(part.value()));
        }
    }
    appendComposerTextPart(parts, textParagraphs);
    return parts;
}

bool verifyOrderedComposerClipboardPaste(
    const std::filesystem::path& imagePath,
    const std::filesystem::path& filePath)
{
    std::string clipboardHtml = "<p>before</p><img src=\"file:///";
    clipboardHtml += escapeHtml(filesystemPathToGenericUtf8(imagePath));
    clipboardHtml += "\" alt=\"reference\"><p>middle</p>";
    clipboardHtml += "<a href=\"file:///";
    clipboardHtml += escapeHtml(filesystemPathToGenericUtf8(filePath));
    clipboardHtml += "\" download=\"specification.txt\">specification.txt</a>";
    clipboardHtml += "<p>after</p>";

    skui::RuntimeOptions options;
    options.readClipboardContent = [clipboardHtml] {
        skui::ClipboardContent content;
        content.html = clipboardHtml;
        return content;
    };
    skui::Runtime runtime(options);
    runtime.resize(600, 300, 1.0f);
    constexpr std::string_view kComposerTestHtml = R"html(
<html><body>
  <div id="composer-document" contenteditable="true">
    <p id="composer-text-initial">seed</p>
  </div>
</body></html>
)html";
    if (!runtime.loadDocumentFromString(kComposerTestHtml) ||
        !runtime.setSelectionBaseAndExtent(
            kComposerInitialParagraphId,
            0u,
            kComposerInitialParagraphId,
            4u)) {
        return false;
    }
    SkiaUiRuntimeBinding binding;
    rememberComposerSelection(runtime, binding);

    const skui::ClipboardContent content = runtime.readClipboardContent();
    if (content.items.size() != 5u ||
        content.items[0].type != skui::ClipboardItemType::Text ||
        content.items[1].type != skui::ClipboardItemType::Image ||
        content.items[2].type != skui::ClipboardItemType::Text ||
        content.items[3].type != skui::ClipboardItemType::File ||
        content.items[4].type != skui::ClipboardItemType::Text) {
        return false;
    }

    if (!pasteComposerClipboardContent(runtime, binding, content)) {
        return false;
    }
    const std::vector<relaydesk::storage::ChatMessagePart> parts =
        makeComposerMessageParts(runtime, binding);
    return parts.size() == 5u &&
        parts[0].GetType() == relaydesk::storage::MessagePartType::Text &&
        parts[0].GetText().value_or("") == "before" &&
        parts[1].GetType() == relaydesk::storage::MessagePartType::Image &&
        parts[2].GetType() == relaydesk::storage::MessagePartType::Text &&
        parts[2].GetText().value_or("") == "middle" &&
        parts[3].GetType() == relaydesk::storage::MessagePartType::File &&
        parts[4].GetType() == relaydesk::storage::MessagePartType::Text &&
        parts[4].GetText().value_or("") == "after";
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

bool copyFileToPath(const std::filesystem::path& sourcePath,
                    const std::filesystem::path& targetPath)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(sourcePath, error) || error) {
        return false;
    }

    error.clear();
    if (std::filesystem::equivalent(sourcePath, targetPath, error) && !error) {
        return true;
    }

    error.clear();
    return std::filesystem::copy_file(
               sourcePath,
               targetPath,
               std::filesystem::copy_options::overwrite_existing,
               error) &&
           !error;
}

enum class SaveFileResult {
    Cancelled,
    Saved,
    Failed,
};

SaveFileResult saveFileAs(const std::filesystem::path& sourcePath)
{
    const std::optional<std::filesystem::path> targetPath =
        relaydesk::platform::selectSavePathFromDialog(
            sourcePath.parent_path(),
            filesystemPathToGenericUtf8(sourcePath.filename()));
    if (!targetPath.has_value()) {
        return SaveFileResult::Cancelled;
    }
    return copyFileToPath(sourcePath, targetPath.value())
        ? SaveFileResult::Saved
        : SaveFileResult::Failed;
}

bool shellOpenPath(const std::filesystem::path& path)
{
    const HINSTANCE result = ShellExecuteW(nullptr,
                                           L"open",
                                           path.wstring().c_str(),
                                           nullptr,
                                           nullptr,
                                           SW_SHOWNORMAL);
    return reinterpret_cast<std::intptr_t>(result) > 32;
}

void hideMessageContextMenu(skui::Runtime& runtime)
{
    gMessageContextMenuVisible = false;
    runtime.setStyleById("message-context-menu", "display: none;");
    runtime.setStyleById("image-context-menu", "display: none;");
    runtime.setStyleById("file-context-menu", "display: none;");
}

void showMessageContextMenu(skui::Runtime& runtime,
                            std::string_view menuId,
                            float x,
                            float y,
                            int menuWidth,
                            int menuHeight)
{
    const int mainWidth =
        std::max(1, runtimeLogicalWidth(runtime) - kChatMessagePaneLeft);
    const int menuLeft =
        std::clamp(static_cast<int>(std::lround(x)) - kChatMessagePaneLeft,
                   0,
                   std::max(0, mainWidth - menuWidth));
    const int menuTop =
        std::clamp(static_cast<int>(std::lround(y)),
                   0,
                   std::max(0, runtimeLogicalHeight(runtime) - menuHeight));
    std::string style = "display: flex; left: ";
    style += std::to_string(menuLeft);
    style += "px; top: ";
    style += std::to_string(menuTop);
    style += "px;";
    gMessageContextMenuVisible = true;
    runtime.setStyleById(menuId, style);
}

void showTextMessageContextMenu(skui::Runtime& runtime, float x, float y)
{
    runtime.setStyleById("image-context-menu", "display: none;");
    runtime.setStyleById("file-context-menu", "display: none;");
    showMessageContextMenu(runtime,
                           "message-context-menu",
                           x,
                           y,
                           kChatContextMenuWidth,
                           kChatContextMenuHeight);
}

void showImageMessageContextMenu(skui::Runtime& runtime, float x, float y)
{
    runtime.setStyleById("message-context-menu", "display: none;");
    runtime.setStyleById("file-context-menu", "display: none;");
    showMessageContextMenu(runtime,
                           "image-context-menu",
                           x,
                           y,
                           kImageContextMenuWidth,
                           kImageContextMenuHeight);
}

void showFileMessageContextMenu(skui::Runtime& runtime, float x, float y)
{
    runtime.setStyleById("message-context-menu", "display: none;");
    runtime.setStyleById("image-context-menu", "display: none;");
    showMessageContextMenu(runtime,
                           "file-context-menu",
                           x,
                           y,
                           kFileContextMenuWidth,
                           kFileContextMenuHeight);
}

bool eventHasClass(const skui::ElementEvent& event, std::string_view className)
{
    return std::find(event.classes.begin(), event.classes.end(), className) !=
           event.classes.end();
}

bool isMessageContextMenuEvent(const skui::ElementEvent& event)
{
    return event.id == "message-context-menu" ||
           event.id == "image-context-menu" ||
           event.id == "file-context-menu" ||
           event.id == "message-context-copy" ||
           event.id == "image-context-reveal" ||
           event.id == "image-context-copy" ||
           event.id == "image-context-save" ||
           event.id == "file-context-reveal" ||
           event.id == "file-context-copy" ||
           event.id == "file-context-save" ||
           event.action == "copy-message-context" ||
           event.action == "reveal-image-context" ||
           event.action == "copy-image-context" ||
           event.action == "save-image-context" ||
           event.action == "reveal-file-context" ||
           event.action == "copy-file-context" ||
           event.action == "save-file-context" ||
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
    constexpr std::array<const char*, 5> units{"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(size);
    std::size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1u < units.size()) {
        value /= 1024.0;
        ++unitIndex;
    }

    std::ostringstream output;
    if (unitIndex == 0u || value >= 100.0) {
        output << static_cast<std::uintmax_t>(std::round(value));
    } else {
        output << std::fixed << std::setprecision(1) << value;
    }
    output << ' ' << units[unitIndex];
    return output.str();
}

std::string transferStateText(relaydesk::storage::TransferState state)
{
    switch (state) {
    case relaydesk::storage::TransferState::Pending:
        return "待发送";
    case relaydesk::storage::TransferState::Offered:
        return "待接收";
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

std::string transferStateText(relaydesk::storage::TransferState state,
                              bool outgoing)
{
    if (state == relaydesk::storage::TransferState::Failed) {
        return outgoing ? "发送失败" : "接收失败";
    }
    if (state == relaydesk::storage::TransferState::Interrupted) {
        return outgoing ? "发送中断" : "接收中断";
    }
    return transferStateText(state);
}

bool isCompletedAttachmentMissing(
    const relaydesk::storage::ChatMessagePart& part)
{
    if (!part.GetTransferState().has_value() ||
        part.GetTransferState().value() !=
            relaydesk::storage::TransferState::Completed ||
        !part.GetLocalPath().has_value() || part.GetLocalPath()->empty()) {
        return false;
    }

    try {
        const relaydesk::storage::AppPaths appPaths =
            relaydesk::storage::createAppPaths();
        const std::filesystem::path attachmentPath =
            resolveWorkRelativePath(appPaths, part.GetLocalPath().value());
        std::error_code error;
        const bool exists = std::filesystem::exists(attachmentPath, error);
        return !error && !exists;
    } catch (const std::exception&) {
        return false;
    }
}

std::string transferStateText(
    const relaydesk::storage::ChatMessagePart& part,
    bool outgoing)
{
    if (isCompletedAttachmentMissing(part)) {
        return "已清理";
    }
    if (part.GetTransferState().has_value()) {
        return transferStateText(part.GetTransferState().value(), outgoing);
    }
    return "待发送";
}

std::optional<OpenableTransferPath> resolveOpenableTransferPath(
    const relaydesk::storage::ChatMessagePart& part)
{
    const bool folder =
        part.GetType() == relaydesk::storage::MessagePartType::Folder;
    if ((part.GetType() != relaydesk::storage::MessagePartType::File &&
         !folder) ||
        !part.GetLocalPath().has_value() || part.GetLocalPath()->empty()) {
        return std::nullopt;
    }

    try {
        const relaydesk::storage::AppPaths appPaths =
            relaydesk::storage::createAppPaths();
        std::error_code error;
        std::filesystem::path filePath = std::filesystem::absolute(
            resolveWorkRelativePath(appPaths, part.GetLocalPath().value()),
            error);
        if (error) {
            return std::nullopt;
        }
        filePath = filePath.lexically_normal();
        error.clear();
        if (std::filesystem::is_directory(filePath, error) && !error) {
            return OpenableTransferPath{filePath, true};
        }
        error.clear();
        if (std::filesystem::is_regular_file(filePath, error) && !error) {
            return OpenableTransferPath{filePath, false};
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> resolveFileContextPath(
    const relaydesk::storage::ChatMessagePart& part)
{
    const std::optional<OpenableTransferPath> openablePath =
        resolveOpenableTransferPath(part);
    if (!openablePath.has_value() || openablePath->folder) {
        return std::nullopt;
    }
    return openablePath->path;
}

bool shouldShowOpenTransferActions(
    relaydesk::storage::TransferState state,
    bool outgoing)
{
    if (outgoing) {
        return state == relaydesk::storage::TransferState::Completed ||
            state == relaydesk::storage::TransferState::Cancelled ||
            state == relaydesk::storage::TransferState::Rejected;
    }
    return state == relaydesk::storage::TransferState::Completed ||
        state == relaydesk::storage::TransferState::Cancelled;
}

bool shouldShowIncomingTransferAcceptActions(
    relaydesk::storage::TransferState state)
{
    return state == relaydesk::storage::TransferState::Offered ||
        state == relaydesk::storage::TransferState::Interrupted;
}

std::string transferFileNameLeaf(std::string fileName)
{
    const std::size_t position = fileName.find_last_of("/\\");
    if (position != std::string::npos) {
        fileName = fileName.substr(position + 1u);
    }
    if (fileName.empty() || fileName == "." || fileName == "..") {
        return "transfer.bin";
    }
    for (char& value : fileName) {
        if (value == '/' || value == '\\' || value == ':' || value == '*' ||
            value == '?' || value == '"' || value == '<' || value == '>' ||
            value == '|') {
            value = '_';
        }
    }
    return fileName;
}

bool incomingTransferTargetExists(
    const relaydesk::storage::ChatMessagePart& part)
{
    const bool folder =
        part.GetType() == relaydesk::storage::MessagePartType::Folder;
    if ((part.GetType() != relaydesk::storage::MessagePartType::File &&
         !folder) ||
        !part.GetFileName().has_value()) {
        return false;
    }
    try {
        const relaydesk::storage::AppPaths appPaths =
            relaydesk::storage::createAppPaths();
        const std::filesystem::path targetPath =
            appPaths.GetInboxDirectory() /
            filesystemPathFromUtf8(
                transferFileNameLeaf(part.GetFileName().value()));
        std::error_code error;
        return std::filesystem::exists(targetPath, error) && !error;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<std::filesystem::path> selectIncomingTransferSavePath(
    const std::string& fileName)
{
    if (fileName.empty()) {
        return std::nullopt;
    }
    try {
        const relaydesk::storage::AppPaths appPaths =
            relaydesk::storage::createAppPaths();
        return relaydesk::platform::selectSavePathFromDialog(
            appPaths.GetInboxDirectory(), transferFileNameLeaf(fileName));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string makeTransferPartActionValue(
    std::string_view prefix,
    const relaydesk::storage::ChatMessageRecord& message,
    const relaydesk::storage::ChatMessagePart& part)
{
    std::string action(prefix);
    action += message.GetMessageId();
    action += ':';
    action += part.GetPartId();
    return action;
}

std::optional<TransferPartActionTarget> parseTransferPartActionTarget(
    std::string_view action,
    std::string_view prefix)
{
    if (!action.starts_with(prefix)) {
        return std::nullopt;
    }
    const std::string_view payload = action.substr(prefix.size());
    const std::size_t separator = payload.find(':');
    if (separator == std::string_view::npos || separator == 0u ||
        separator + 1u >= payload.size()) {
        return std::nullopt;
    }
    return TransferPartActionTarget{
        std::string(payload.substr(0u, separator)),
        std::string(payload.substr(separator + 1u)),
    };
}

const relaydesk::storage::ChatMessagePart* findSelectedPeerMessagePart(
    const relaydesk::runtime::RelayDeskRuntime& relayRuntime,
    const TransferPartActionTarget& target)
{
    for (const auto& message : relayRuntime.GetSelectedPeerMessages()) {
        if (message.GetMessageId() != target.messageId) {
            continue;
        }
        for (const auto& part : message.GetParts()) {
            if (part.GetPartId() == target.partId) {
                return &part;
            }
        }
        return nullptr;
    }
    return nullptr;
}

void appendTransferActionButtonMarkup(std::string& markup,
                                      std::string_view label,
                                      std::string_view action,
                                      bool primary)
{
    markup += R"(<div class="transfer-action-button)";
    if (primary) {
        markup += " primary";
    }
    markup += R"(" data-action=")";
    markup += escapeHtml(action);
    markup += R"(">)";
    markup += escapeHtml(label);
    markup += R"(</div>)";
}

std::string wrapTransferActionButtons(std::string buttons)
{
    if (buttons.empty()) {
        return {};
    }
    return R"(<div class="transfer-actions">)" +
        std::move(buttons) + R"(</div>)";
}

std::string makeTransferActionsMarkup(
    const relaydesk::storage::ChatMessageRecord& message,
    const relaydesk::storage::ChatMessagePart& part,
    const std::optional<OpenableTransferPath>& openablePath,
    bool incomingTargetExists)
{
    const bool fileOrFolder =
        part.GetType() == relaydesk::storage::MessagePartType::File ||
        part.GetType() == relaydesk::storage::MessagePartType::Folder;
    if (!fileOrFolder || !part.GetTransferState().has_value()) {
        return {};
    }

    const bool outgoing =
        message.GetDirection() == relaydesk::storage::MessageDirection::Outgoing;
    const relaydesk::storage::TransferState state =
        part.GetTransferState().value();
    const bool canAcceptIncoming =
        !outgoing && shouldShowIncomingTransferAcceptActions(state);
    if (!canAcceptIncoming && shouldShowOpenTransferActions(state, outgoing)) {
        if (!openablePath.has_value()) {
            if (outgoing || state == relaydesk::storage::TransferState::Completed) {
                return R"(<div class="transfer-cleaned-notice">已清理</div>)";
            }
            return {};
        }

        std::string buttons;
        const std::string openPath =
            filesystemPathToGenericUtf8(openablePath->path);
        if (!openablePath->folder) {
            appendTransferActionButtonMarkup(
                buttons,
                "打开",
                std::string(kTransferOpenActionPrefix) + openPath,
                true);
        }
        appendTransferActionButtonMarkup(
            buttons,
            "打开文件夹",
            std::string(openablePath->folder
                            ? kTransferOpenActionPrefix
                            : kTransferRevealActionPrefix) +
                openPath,
            false);
        return wrapTransferActionButtons(std::move(buttons));
    }

    std::string buttons;
    if (outgoing && state == relaydesk::storage::TransferState::Interrupted) {
        appendTransferActionButtonMarkup(
            buttons,
            "继续发送",
            makeTransferPartActionValue(kTransferSendActionPrefix,
                                        message,
                                        part),
            true);
    } else if (outgoing && state == relaydesk::storage::TransferState::Failed) {
        const bool deliveryFailed =
            message.GetDeliveryState() == relaydesk::storage::DeliveryState::Failed;
        const std::string action = deliveryFailed
            ? std::string(kTransferResendMessageActionPrefix) +
                message.GetMessageId()
            : makeTransferPartActionValue(kTransferSendActionPrefix,
                                          message,
                                          part);
        appendTransferActionButtonMarkup(
            buttons, "重新发送", action, true);
    } else if (outgoing && state == relaydesk::storage::TransferState::Offered) {
        appendTransferActionButtonMarkup(
            buttons,
            "主动发送",
            makeTransferPartActionValue(kTransferSendActionPrefix,
                                        message,
                                        part),
            true);
        appendTransferActionButtonMarkup(
            buttons,
            "取消",
            makeTransferPartActionValue(kTransferCancelActionPrefix,
                                        message,
                                        part),
            false);
    } else if (state == relaydesk::storage::TransferState::Transferring) {
        appendTransferActionButtonMarkup(
            buttons,
            "取消",
            makeTransferPartActionValue(kTransferCancelActionPrefix,
                                        message,
                                        part),
            false);
    } else if (!outgoing &&
               state == relaydesk::storage::TransferState::Interrupted) {
        appendTransferActionButtonMarkup(
            buttons,
            "继续接收",
            makeTransferPartActionValue(kTransferAcceptActionPrefix,
                                        message,
                                        part),
            true);
    } else if (!outgoing && state == relaydesk::storage::TransferState::Offered) {
        appendTransferActionButtonMarkup(
            buttons,
            "接收",
            makeTransferPartActionValue(kTransferAcceptActionPrefix,
                                        message,
                                        part),
            true);
        appendTransferActionButtonMarkup(
            buttons,
            "另存为",
            makeTransferPartActionValue(kTransferSaveAsActionPrefix,
                                        message,
                                        part),
            false);
        if (incomingTargetExists) {
            appendTransferActionButtonMarkup(
                buttons,
                "覆盖",
                makeTransferPartActionValue(kTransferOverwriteActionPrefix,
                                            message,
                                            part),
                false);
        }
        appendTransferActionButtonMarkup(
            buttons,
            "拒绝",
            makeTransferPartActionValue(kTransferRejectActionPrefix,
                                        message,
                                        part),
            false);
    }
    return wrapTransferActionButtons(std::move(buttons));
}

std::string transferStateTextWithActions(
    const relaydesk::storage::ChatMessagePart& part,
    bool outgoing,
    const std::string& actionsMarkup)
{
    std::string stateText = transferStateText(part, outgoing);
    if (stateText == "已清理" &&
        actionsMarkup.find(R"(class="transfer-cleaned-notice")") !=
            std::string::npos &&
        part.GetTransferState().has_value()) {
        stateText = transferStateText(part.GetTransferState().value(), outgoing);
    }
    return stateText;
}

bool handleTransferCardAction(
    std::string_view action,
    relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    if (action.starts_with(kTransferOpenActionPrefix)) {
        const std::optional<std::filesystem::path> path =
            tryFilesystemPathFromUtf8(action.substr(
                kTransferOpenActionPrefix.size()));
        if (path.has_value()) {
            (void)shellOpenPath(path.value());
        }
        return true;
    }
    if (action.starts_with(kTransferRevealActionPrefix)) {
        const std::optional<std::filesystem::path> path =
            tryFilesystemPathFromUtf8(action.substr(
                kTransferRevealActionPrefix.size()));
        if (path.has_value()) {
            (void)relaydesk::platform::revealPathInFileManager(path.value());
        }
        return true;
    }
    if (action.starts_with(kTransferResendMessageActionPrefix)) {
        const std::string messageId(
            action.substr(kTransferResendMessageActionPrefix.size()));
        if (!messageId.empty()) {
            relayRuntime.resendSelectedPeerMessage(messageId);
        }
        return true;
    }

    if (action.starts_with(kTransferSendActionPrefix)) {
        const std::optional<TransferPartActionTarget> target =
            parseTransferPartActionTarget(action, kTransferSendActionPrefix);
        if (target.has_value()) {
            relayRuntime.sendSelectedPeerFileTransfer(target->messageId,
                                                      target->partId);
        }
        return true;
    }
    if (action.starts_with(kTransferCancelActionPrefix)) {
        const std::optional<TransferPartActionTarget> target =
            parseTransferPartActionTarget(action, kTransferCancelActionPrefix);
        if (target.has_value()) {
            relayRuntime.cancelSelectedPeerFileTransfer(target->messageId,
                                                        target->partId);
        }
        return true;
    }
    if (action.starts_with(kTransferAcceptActionPrefix)) {
        const std::optional<TransferPartActionTarget> target =
            parseTransferPartActionTarget(action, kTransferAcceptActionPrefix);
        if (target.has_value()) {
            relayRuntime.acceptSelectedPeerFileTransfer(target->messageId,
                                                        target->partId,
                                                        false);
        }
        return true;
    }
    if (action.starts_with(kTransferOverwriteActionPrefix)) {
        const std::optional<TransferPartActionTarget> target =
            parseTransferPartActionTarget(
                action, kTransferOverwriteActionPrefix);
        if (target.has_value()) {
            relayRuntime.acceptSelectedPeerFileTransfer(target->messageId,
                                                        target->partId,
                                                        true);
        }
        return true;
    }
    if (action.starts_with(kTransferRejectActionPrefix)) {
        const std::optional<TransferPartActionTarget> target =
            parseTransferPartActionTarget(action, kTransferRejectActionPrefix);
        if (target.has_value()) {
            relayRuntime.rejectSelectedPeerFileTransfer(target->messageId,
                                                        target->partId);
        }
        return true;
    }
    if (action.starts_with(kTransferSaveAsActionPrefix)) {
        const std::optional<TransferPartActionTarget> target =
            parseTransferPartActionTarget(action, kTransferSaveAsActionPrefix);
        if (!target.has_value()) {
            return true;
        }
        const relaydesk::storage::ChatMessagePart* part =
            findSelectedPeerMessagePart(relayRuntime, target.value());
        if (part == nullptr || !part->GetFileName().has_value()) {
            return true;
        }
        const std::optional<std::filesystem::path> savePath =
            selectIncomingTransferSavePath(part->GetFileName().value());
        if (savePath.has_value()) {
            relayRuntime.acceptSelectedPeerFileTransferAs(target->messageId,
                                                          target->partId,
                                                          savePath.value());
        }
        return true;
    }
    return false;
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
    html += R"(" value=")";
    html += escapeHtmlMultilineAttribute(text);
    html += R"(")";
    html += makeUrlLinkAttributes(text);
    html += R"(></selectable>)";
    if (!outgoing && !timeText.empty()) {
        html += R"(<div class="time-label">)";
        html += escapeHtml(timeText);
        html += R"(</div>)";
    }
    html += R"(</div>)";
    return html;
}

bool isTransferWarning(
    const relaydesk::storage::ChatMessagePart& part)
{
    if (!part.GetTransferState().has_value()) {
        return false;
    }
    const relaydesk::storage::TransferState state =
        part.GetTransferState().value();
    return state == relaydesk::storage::TransferState::Failed ||
        state == relaydesk::storage::TransferState::Interrupted ||
        state == relaydesk::storage::TransferState::Cancelled ||
        state == relaydesk::storage::TransferState::Rejected;
}

std::string makeTransferMessageMarkup(
    const relaydesk::storage::ChatMessageRecord& message,
    const relaydesk::storage::ChatMessagePart& part)
{
    const bool outgoing =
        message.GetDirection() == relaydesk::storage::MessageDirection::Outgoing;
    const int progress = transferProgressPercent(part);
    const bool warning = isTransferWarning(part);
    const std::string title = partDisplayText(part);
    const std::string sizeText =
        part.GetFileSize().has_value() ? formatFileSize(part.GetFileSize().value())
                                       : "文件夹";
    const std::optional<OpenableTransferPath> openablePath =
        resolveOpenableTransferPath(part);
    const std::optional<std::filesystem::path> fileContextPath =
        openablePath.has_value() && !openablePath->folder
            ? std::optional<std::filesystem::path>(openablePath->path)
            : std::nullopt;
    const bool targetExists =
        !outgoing && part.GetTransferState().has_value() &&
        part.GetTransferState().value() ==
            relaydesk::storage::TransferState::Offered &&
        incomingTransferTargetExists(part);
    const std::string actionsMarkup = makeTransferActionsMarkup(
        message, part, openablePath, targetExists);
    const std::string stateText =
        transferStateTextWithActions(part, outgoing, actionsMarkup);

    std::string html;
    html.reserve(760);
    html += R"(<div class="message-row message-row-transfer" contenteditable="true" aria-readonly="true">)";
    html += R"(<div class="transfer-card)";
    if (warning) {
        html += " warning";
    }
    html += R"(" contenteditable="false")";
    if (fileContextPath.has_value()) {
        html += makeClipboardAttachmentAttributes(
            part.GetType() == relaydesk::storage::MessagePartType::Folder
                ? ComposerAttachmentKind::Folder
                : ComposerAttachmentKind::File,
            fileContextPath.value(),
            title);
        html += R"( data-action="file-context:)";
        html += escapeHtml(filesystemPathToGenericUtf8(
            fileContextPath.value()));
        html += '"';
    }
    html += '>';
    html += R"(<div class="file-icon doc-icon doc-icon-zip"><div class="doc-fold"></div></div>)";
    html += R"(<div class="transfer-content"><selectable class="file-name">)";
    html += escapeHtml(title);
    html += R"(</selectable><div class="transfer-summary"><div class="file-size">)";
    html += escapeHtml(sizeText);
    html += R"(</div><div class="transfer-percent)";
    if (progress == 100) {
        html += " done";
    }
    html += R"(">)";
    html += std::to_string(progress);
    html += R"(%</div></div><progress class="progress-main)";
    if (warning) {
        html += " warning";
    }
    html += R"(" value=")";
    html += std::to_string(progress);
    html += R"(" max="100"></progress><div class="transfer-footer"><div class="transfer-meta)";
    if (stateText == "已清理") {
        html += " cleaned";
    }
    html += R"(">)";
    html += escapeHtml(stateText);
    html += R"(</div><div class="transfer-tail">)";
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
    html += actionsMarkup;
    html += R"(</div></div></div>)";
    return html;
}

struct MessageImageAsset {
    std::filesystem::path sourcePath;
    relaydesk::platform::ImageSize displaySize;
};

std::optional<MessageImageAsset> resolveMessageImageAsset(
    const relaydesk::storage::ChatMessagePart& part)
{
    if (part.GetType() != relaydesk::storage::MessagePartType::Image ||
        !part.GetLocalPath().has_value() || part.GetLocalPath()->empty()) {
        return std::nullopt;
    }

    try {
        const relaydesk::storage::AppPaths appPaths =
            relaydesk::storage::createAppPaths();
        const std::filesystem::path sourcePath =
            resolveWorkRelativePath(appPaths, part.GetLocalPath().value());
        std::error_code error;
        if (!std::filesystem::is_regular_file(sourcePath, error) || error) {
            return std::nullopt;
        }

        const std::optional<relaydesk::platform::ImageSize> displaySize =
            relaydesk::platform::probeImageSize(sourcePath);
        if (!displaySize.has_value() || displaySize->width == 0u ||
            displaySize->height == 0u) {
            return std::nullopt;
        }

        return MessageImageAsset{sourcePath, displaySize.value()};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::pair<int, int> messageImageDisplaySize(
    const relaydesk::platform::ImageSize& imageSize)
{
    const float width = static_cast<float>(imageSize.width);
    const float height = static_cast<float>(imageSize.height);
    const float maximumScale =
        std::min(kMessageImageMaxWidth / width,
                 kMessageImageMaxHeight / height);
    float scale = std::min(1.0f, maximumScale);
    const float longestSide = std::max(width, height);
    if (longestSide < 120.0f) {
        scale = std::min(maximumScale, 120.0f / longestSide);
    }
    return {
        std::max(1, static_cast<int>(std::lround(width * scale))),
        std::max(1, static_cast<int>(std::lround(height * scale)))
    };
}

std::optional<std::string> makeMessageImageCardMarkup(
    const relaydesk::storage::ChatMessagePart& part)
{
    const std::optional<MessageImageAsset> asset =
        resolveMessageImageAsset(part);
    if (!asset.has_value()) {
        return std::nullopt;
    }

    const auto [imageWidth, imageHeight] =
        messageImageDisplaySize(asset->displaySize);
    const std::string sourcePath =
        filesystemPathToGenericUtf8(asset->sourcePath);

    std::string html;
    html.reserve(560);
    html += R"(<div class="message-image-card" contenteditable="false")";
    html += makeClipboardAttachmentAttributes(
        ComposerAttachmentKind::Image, asset->sourcePath, partDisplayText(part));
    html += R"( data-action="image-context:)";
    html += escapeHtml(sourcePath);
    html += R"(" style="width: )";
    html += std::to_string(imageWidth + 12);
    html += "px; height: ";
    html += std::to_string(imageHeight + 12);
    html += R"(px;"><img class="message-image" src=")";
    html += escapeHtml(sourcePath);
    html += R"(" data-action="image-context:)";
    html += escapeHtml(sourcePath);
    html += R"(" style="width: )";
    html += std::to_string(imageWidth);
    html += "px; height: ";
    html += std::to_string(imageHeight);
    html += R"(px;"></div>)";
    return html;
}

std::string makeImageMessageMarkup(
    const relaydesk::storage::ChatMessageRecord& message,
    const relaydesk::storage::ChatMessagePart& part)
{
    const std::optional<std::string> imageCardMarkup =
        makeMessageImageCardMarkup(part);
    if (!imageCardMarkup.has_value()) {
        return makeTransferMessageMarkup(message, part);
    }

    const bool outgoing =
        message.GetDirection() == relaydesk::storage::MessageDirection::Outgoing;
    const std::string timeText = shortMessageTime(message);

    std::string html;
    html.reserve(720);
    html += R"(<div class="message-row message-row-image )";
    html += outgoing ? "message-row-right" : "message-row-left";
    html += R"(">)";
    if (outgoing && !timeText.empty()) {
        html += R"(<div class="time-label">)";
        html += escapeHtml(timeText);
        html += R"(</div>)";
    }
    html += R"(<div class="message-selection-document" contenteditable="true" aria-readonly="true">)";
    html += imageCardMarkup.value();
    html += R"(</div>)";
    if (!outgoing && !timeText.empty()) {
        html += R"(<div class="time-label">)";
        html += escapeHtml(timeText);
        html += R"(</div>)";
    }
    html += R"(</div>)";
    return html;
}

std::string makeDocumentTextPartMarkup(
    const relaydesk::storage::ChatMessagePart& part)
{
    const std::string text = trimMessageWhitespace(partDisplayText(part));
    std::string html;
    html.reserve(text.size() + 180u);
    html += R"(<selectable class="message-document-text" value=")";
    html += escapeHtmlMultilineAttribute(text);
    html += '"';
    html += makeUrlLinkAttributes(text);
    html += R"(></selectable>)";
    return html;
}

std::string makeDocumentTransferPartMarkup(
    const relaydesk::storage::ChatMessageRecord& message,
    const relaydesk::storage::ChatMessagePart& part)
{
    const bool outgoing =
        message.GetDirection() == relaydesk::storage::MessageDirection::Outgoing;
    const int progress = transferProgressPercent(part);
    const bool warning = isTransferWarning(part);
    const std::string title = partDisplayText(part);
    const std::string sizeText = part.GetFileSize().has_value()
        ? formatFileSize(part.GetFileSize().value())
        : "文件夹";
    const std::optional<OpenableTransferPath> openablePath =
        resolveOpenableTransferPath(part);
    const std::optional<std::filesystem::path> fileContextPath =
        openablePath.has_value() && !openablePath->folder
            ? std::optional<std::filesystem::path>(openablePath->path)
            : std::nullopt;
    const bool targetExists =
        !outgoing && part.GetTransferState().has_value() &&
        part.GetTransferState().value() ==
            relaydesk::storage::TransferState::Offered &&
        incomingTransferTargetExists(part);
    const std::string actionsMarkup = makeTransferActionsMarkup(
        message, part, openablePath, targetExists);
    const std::string stateText =
        transferStateTextWithActions(part, outgoing, actionsMarkup);

    std::string html;
    html.reserve(title.size() + 640u);
    html += R"(<div class="message-document-file)";
    if (warning) {
        html += " warning";
    }
    html += R"(" contenteditable="false")";
    if (fileContextPath.has_value()) {
        html += makeClipboardAttachmentAttributes(
            part.GetType() == relaydesk::storage::MessagePartType::Folder
                ? ComposerAttachmentKind::Folder
                : ComposerAttachmentKind::File,
            fileContextPath.value(),
            title);
        html += R"( data-action="file-context:)";
        html += escapeHtml(filesystemPathToGenericUtf8(
            fileContextPath.value()));
        html += '"';
    }
    html += '>';
    html += R"(<div class="message-document-file-icon doc-icon doc-icon-zip"><div class="doc-fold"></div></div>)";
    html += R"(<div class="message-document-file-content"><selectable class="message-document-file-name" value=")";
    html += escapeHtml(title);
    html += R"("></selectable><div class="message-document-file-summary"><div class="message-document-file-state">)";
    html += escapeHtml(sizeText);
    html += " · ";
    html += escapeHtml(stateText);
    html += R"(</div><div class="message-document-file-percent">)";
    html += std::to_string(progress);
    html += R"(%</div></div><progress class="message-document-progress)";
    if (warning) {
        html += " warning";
    }
    html += R"(" value=")";
    html += std::to_string(progress);
    html += R"(" max="100"></progress>)";
    html += actionsMarkup;
    html += R"(</div></div>)";
    return html;
}

std::string makeCompoundMessageMarkup(
    const relaydesk::storage::ChatMessageRecord& message)
{
    const bool outgoing =
        message.GetDirection() == relaydesk::storage::MessageDirection::Outgoing;
    const std::string timeText = shortMessageTime(message);

    std::string documentMarkup;
    documentMarkup.reserve(1600);
    for (const auto& part : message.GetParts()) {
        if (part.GetType() == relaydesk::storage::MessagePartType::Image) {
            const std::optional<std::string> imageMarkup =
                makeMessageImageCardMarkup(part);
            documentMarkup += imageMarkup.has_value()
                ? imageMarkup.value()
                : makeDocumentTransferPartMarkup(message, part);
        } else if (part.GetType() == relaydesk::storage::MessagePartType::File ||
                   part.GetType() ==
                       relaydesk::storage::MessagePartType::Folder) {
            documentMarkup += makeDocumentTransferPartMarkup(message, part);
        } else {
            documentMarkup += makeDocumentTextPartMarkup(part);
        }
    }

    if (documentMarkup.empty()) {
        relaydesk::storage::ChatMessagePart emptyPart;
        emptyPart.SetText("空消息");
        documentMarkup = makeDocumentTextPartMarkup(emptyPart);
    }

    std::string html;
    html.reserve(documentMarkup.size() + 260u);
    html += R"(<div class="message-row message-row-document )";
    html += outgoing ? "message-row-right" : "message-row-left";
    html += R"(">)";
    if (outgoing && !timeText.empty()) {
        html += R"(<div class="time-label">)";
        html += escapeHtml(timeText);
        html += R"(</div>)";
    }
    html += R"(<div class="message-document )";
    html += outgoing ? "message-document-right" : "message-document-left";
    html += R"(" contenteditable="true" aria-readonly="true">)";
    html += documentMarkup;
    html += R"(</div>)";
    if (!outgoing && !timeText.empty()) {
        html += R"(<div class="time-label">)";
        html += escapeHtml(timeText);
        html += R"(</div>)";
    }
    html += R"(</div>)";
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
            if (message.GetParts().size() > 1u) {
                html += makeCompoundMessageMarkup(message);
                continue;
            }

            bool rendered = false;
            for (const auto& part : message.GetParts()) {
                if (part.GetType() == relaydesk::storage::MessagePartType::Image) {
                    html += makeImageMessageMarkup(message, part);
                } else if (part.GetType() ==
                               relaydesk::storage::MessagePartType::File ||
                           part.GetType() ==
                               relaydesk::storage::MessagePartType::Folder) {
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

void showImagePreview(skui::Runtime& runtime,
                      const std::filesystem::path& imagePath)
{
    const std::optional<relaydesk::platform::ImageSize> imageSize =
        relaydesk::platform::probeImageSize(imagePath);
    if (!imageSize.has_value() || imageSize->width == 0u ||
        imageSize->height == 0u) {
        return;
    }

    const float availableWidth = static_cast<float>(std::max(
        1,
        runtimeLogicalWidth(runtime) - kChatMessagePaneLeft - 96));
    const float availableHeight = static_cast<float>(
        std::max(1, runtimeLogicalHeight(runtime) - 96));
    const float width = static_cast<float>(imageSize->width);
    const float height = static_cast<float>(imageSize->height);
    const float scale = std::min(
        1.0f,
        std::min(availableWidth / width, availableHeight / height));
    const int displayWidth =
        std::max(1, static_cast<int>(std::lround(width * scale)));
    const int displayHeight =
        std::max(1, static_cast<int>(std::lround(height * scale)));

    runtime.setAttributeById("image-preview-image",
                             "src",
                             filesystemPathToGenericUtf8(imagePath));
    runtime.setStyleById(
        "image-preview-image",
        "width: " + std::to_string(displayWidth) +
            "px; height: " + std::to_string(displayHeight) + "px;");
    runtime.setStyleById("image-preview-overlay", "display: flex;");
    gImagePreviewVisible = true;
}

void hideImagePreview(skui::Runtime& runtime)
{
    runtime.setStyleById("image-preview-overlay", "display: none;");
    gImagePreviewVisible = false;
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
    const relaydesk::runtime::RelayDeskRuntime& relayRuntime,
    const SkiaUiRuntimeBinding& binding)
{
    std::string signature = "--devices--\n";
    signature += makeDeviceListSignature(relayRuntime);
    signature += "\n--chat--\n";
    signature += makeChatSignature(relayRuntime);
    signature += "\n--composer-document--\n";
    for (const ComposerAttachment& attachment :
         binding.composerAttachments) {
        signature += attachment.attachmentId;
        signature += '|';
        signature += attachment.displayName;
        signature += '|';
        signature += std::to_string(attachment.fileSize);
        signature += '|';
        signature += attachment.fileSizePending ? '1' : '0';
        signature += '\n';
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

bool isChatScrolledToLatest(const skui::Runtime& runtime)
{
    const std::optional<skui::ScrollState> scrollState =
        runtime.scrollStateById("chat-scroll");
    return scrollState.has_value() &&
        (scrollState->maxScrollY <= kChatScrollBottomTolerance ||
         std::abs(scrollState->maxScrollY - scrollState->scrollY) <=
             kChatScrollBottomTolerance);
}

std::string composerPlaceholder(
    const relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    const std::optional<relaydesk::runtime::PeerListItem> selectedPeer =
        relayRuntime.GetSelectedPeer();
    return selectedPeer.has_value()
        ? "给 " + peerDisplayName(selectedPeer.value()) + " 发送消息"
        : "选择设备后发送消息";
}

std::string makeEmptyComposerDocumentMarkup()
{
    return R"(<div id="composer-document" class="composer-document" contenteditable="true" contenteditable-flow="inline"><p id="composer-text-initial" class="composer-document-paragraph"><br></p></div>)";
}

bool composerDocumentHasContent(const skui::Runtime& runtime)
{
    for (const std::string& elementId :
         runtime.childElementIdsById(kComposerDocumentId)) {
        if (composerAttachmentIdFromElementId(elementId).has_value()) {
            return true;
        }
        const std::optional<std::string> text =
            runtime.textContentById(elementId);
        if (text.has_value() &&
            !trimMessageWhitespace(text.value()).empty()) {
            return true;
        }
    }
    return false;
}

void applyComposerPlaceholder(
    skui::Runtime& runtime,
    const relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    (void)runtime.setTextById(
        "composer-placeholder", composerPlaceholder(relayRuntime));
    (void)runtime.setVisibleById(
        "composer-placeholder", !composerDocumentHasContent(runtime));
}

void applyComposerDocumentPanel(
    skui::Runtime& skiaRuntime,
    relaydesk::runtime::RelayDeskRuntime& relayRuntime)
{
    constexpr int kComposerWrapHeight = 250;
    constexpr int kComposerDocumentHeight = 176;
    constexpr int kComposerChatBottom = 274;
    constexpr int kChatScrollTop = 177;
    constexpr int kChatContentVerticalPadding = 46;
    const bool narrow = runtimeLogicalWidth(skiaRuntime) <= 900;
    const int toolbarTop = narrow ? 200 : 198;
    const int sendTop = narrow ? 195 : 193;
    skiaRuntime.setAttributeById(
        "composer-wrap",
        "class",
        "composer-wrap composer-wrap-expanded");
    skiaRuntime.setStyleById(
        "composer-wrap",
        "height: " + std::to_string(kComposerWrapHeight) + "px;");
    skiaRuntime.setStyleById(
        "chat-scroll",
        "bottom: " + std::to_string(kComposerChatBottom) + "px;");
    const int chatContentMinimumHeight = std::max(
        0,
        runtimeLogicalHeight(skiaRuntime) - kChatScrollTop -
            kComposerChatBottom - kChatContentVerticalPadding);
    skiaRuntime.setStyleById(
        "chat-content",
        "min-height: " + std::to_string(chatContentMinimumHeight) + "px;");
    skiaRuntime.setStyleById(
        "composer-document",
        "left: 12px; top: 10px; right: 12px; height: " +
            std::to_string(kComposerDocumentHeight) +
            "px; overflow-y: auto;");
    skiaRuntime.setStyleById(
        "composer-placeholder", "left: 12px; top: 10px; right: 12px;");

    if (narrow) {
        skiaRuntime.setStyleById("composer-emoji", "display: none;");
        skiaRuntime.setStyleById(
            "composer-attach",
            "left: 16px; right: auto; top: " +
                std::to_string(toolbarTop) + "px;");
        skiaRuntime.setStyleById(
            "composer-folder",
            "left: 58px; right: auto; top: " +
                std::to_string(toolbarTop) + "px;");
        skiaRuntime.setStyleById(
            "composer-send",
            "top: " + std::to_string(sendTop) + "px;");
    } else {
        skiaRuntime.setStyleById(
            "composer-emoji",
            "display: block; top: " +
                std::to_string(toolbarTop) + "px;");
        skiaRuntime.setStyleById(
            "composer-attach",
            "left: auto; right: 150px; top: " +
                std::to_string(toolbarTop) + "px;");
        skiaRuntime.setStyleById(
            "composer-folder",
            "left: auto; right: 108px; top: " +
                std::to_string(toolbarTop) + "px;");
        skiaRuntime.setStyleById(
            "composer-send",
            "top: " + std::to_string(sendTop) + "px;");
    }
    applyComposerPlaceholder(skiaRuntime, relayRuntime);
}

void applyRelayDeskDevicePanel(
    skui::Runtime& skiaRuntime,
    relaydesk::runtime::RelayDeskRuntime& relayRuntime,
    SkiaUiRuntimeBinding& binding,
    ChatScrollUpdateMode scrollUpdateMode)
{
    const std::optional<skui::ScrollState> previousChatScroll =
        skiaRuntime.scrollStateById("chat-scroll");
    const bool historyPrepended =
        scrollUpdateMode == ChatScrollUpdateMode::PreserveViewportAfterPrepend;
    const bool keepChatAtLatest =
        scrollUpdateMode == ChatScrollUpdateMode::ScrollToLatest ||
        (!historyPrepended &&
         (!previousChatScroll.has_value() ||
          previousChatScroll->maxScrollY <= kChatScrollBottomTolerance ||
          std::abs(previousChatScroll->maxScrollY -
                   previousChatScroll->scrollY) <=
              kChatScrollBottomTolerance));

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
    } else {
        addTextUpdate(updates, "header-title", "未选择设备");
        addTextUpdate(updates, "header-ip", "等待发现设备");
        addStyleUpdate(updates, "header-status-dot", "background-color: #a8b3c0;");
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
    applyComposerDocumentPanel(skiaRuntime, relayRuntime);

    const std::optional<skui::ScrollState> updatedChatScroll =
        skiaRuntime.scrollStateById("chat-scroll");
    if (!updatedChatScroll.has_value()) {
        return;
    }

    float targetScrollY = updatedChatScroll->maxScrollY;
    if (historyPrepended && previousChatScroll.has_value()) {
        const float prependedHeight =
            updatedChatScroll->maxScrollY - previousChatScroll->maxScrollY;
        targetScrollY = std::clamp(previousChatScroll->scrollY + prependedHeight,
                                   0.0f,
                                   updatedChatScroll->maxScrollY);
    } else if (!keepChatAtLatest) {
        targetScrollY =
            std::min(previousChatScroll->scrollY, updatedChatScroll->maxScrollY);
    }

    const bool paginationWasSuppressed =
        binding.suppressChatHistoryPagination;
    binding.suppressChatHistoryPagination = true;
    if (keepChatAtLatest && !historyPrepended) {
        (void)skiaRuntime.scrollToBottomById("chat-scroll");
    } else {
        (void)skiaRuntime.setScrollOffsetById(
            "chat-scroll", updatedChatScroll->scrollX, targetScrollY);
    }
    binding.suppressChatHistoryPagination = paginationWasSuppressed;
}

void selectTab(skui::Runtime& runtime, std::string_view id)
{
    for (std::string_view tab : kTabs) {
        runtime.removeClassById(tab, "tab-active");
    }
    runtime.addClassById(id, "tab-active");
}

void CALLBACK refreshRelayDeskSkiaUiDeferred(
    HWND,
    UINT,
    UINT_PTR timerId,
    DWORD);

void scheduleRelayDeskPanelRefresh(SkiaUiRuntimeBinding& binding)
{
    if (binding.timerId != 0 && binding.deferredRefreshTimerId == 0) {
        binding.deferredRefreshTimerId = SetTimer(
            nullptr,
            0,
            1,
            refreshRelayDeskSkiaUiDeferred);
    }
}

bool sendComposerMessage(
    skui::Runtime& runtime,
    relaydesk::runtime::RelayDeskRuntime& relayRuntime,
    SkiaUiRuntimeBinding& binding)
{
    if (!relayRuntime.GetSelectedPeer().has_value()) {
        return false;
    }
    std::vector<relaydesk::storage::ChatMessagePart> parts =
        makeComposerMessageParts(runtime, binding);
    if (parts.empty()) {
        return false;
    }

    relayRuntime.sendMessagePartsToSelectedPeer(std::move(parts));
    for (const ComposerAttachment& attachment :
         binding.composerAttachments) {
        (void)core::async::cancel(
            composerFolderSizeTaskKey(attachment.attachmentId));
    }
    binding.composerAttachments.clear();
    binding.composerSelection = {};
    (void)runtime.replaceHtmlById(
        kComposerDocumentId, makeEmptyComposerDocumentMarkup());
    applyRelayDeskDevicePanel(runtime,
                              relayRuntime,
                              binding,
                              ChatScrollUpdateMode::ScrollToLatest);
    return true;
}

void installRelayDeskInteractions(skui::Runtime& runtime,
                                  relaydesk::runtime::RelayDeskRuntime& relayRuntime,
                                  SkiaUiRuntimeBinding& binding)
{
    runtime.setElementKeyDownCallback(
        [&runtime, &relayRuntime, &binding](
            const skui::ElementEvent& event) {
            constexpr unsigned kEnterKey = VK_RETURN;
            constexpr unsigned kPasteKey = 'V';
            if (event.type != skui::ElementEventType::KeyDown ||
                event.id != kComposerDocumentId) {
                return false;
            }
            if (event.ctrlKey && event.key == kPasteKey) {
                rememberComposerSelection(runtime, binding);
                if (binding.readClipboardAttachmentPaths) {
                    if (addComposerAttachmentPaths(
                            runtime,
                            binding,
                            binding.readClipboardAttachmentPaths())) {
                        hideMessageContextMenu(runtime);
                        applyComposerDocumentPanel(runtime, relayRuntime);
                        return true;
                    }
                }
                if (pasteComposerClipboardContent(
                        runtime,
                        binding,
                        runtime.readClipboardContent())) {
                    hideMessageContextMenu(runtime);
                    applyComposerDocumentPanel(runtime, relayRuntime);
                    return true;
                }
                if (!addComposerAttachmentPaths(
                        runtime,
                        binding,
                        relaydesk::platform::collectClipboardAttachmentPaths())) {
                    return false;
                }
                hideMessageContextMenu(runtime);
                applyComposerDocumentPanel(runtime, relayRuntime);
                return true;
            }
            if (event.key != kEnterKey || event.shiftKey) {
                return false;
            }
            hideMessageContextMenu(runtime);
            (void)sendComposerMessage(runtime, relayRuntime, binding);
            return true;
        });
    runtime.setElementEventCallback([&runtime, &relayRuntime, &binding](
                                        const skui::ElementEvent& event) {
        constexpr std::string_view imageContextPrefix = "image-context:";
        constexpr std::string_view fileContextPrefix = "file-context:";
        constexpr std::string_view removeAttachmentPrefix =
            "remove-attachment:";
        if (event.type == skui::ElementEventType::Scroll &&
            event.id == "chat-scroll" &&
            event.scrollY <= kChatLoadMoreTopThreshold &&
            relayRuntime.GetSelectedPeerHasMoreMessages() &&
            !binding.suppressChatHistoryPagination) {
            const std::size_t previousMessageCount =
                relayRuntime.GetSelectedPeerMessages().size();
            if (binding.loadMoreSelectedPeerMessages) {
                binding.loadMoreSelectedPeerMessages();
            } else {
                relayRuntime.loadMoreSelectedPeerMessages();
            }

            if (relayRuntime.GetSelectedPeerMessages().size() >
                previousMessageCount) {
                binding.chatHistoryPrependPending = true;
                scheduleRelayDeskPanelRefresh(binding);
            }
            return;
        }
        if (event.type == skui::ElementEventType::Input &&
            event.id == kComposerDocumentId) {
            rememberComposerSelection(runtime, binding);
            discardComposerAttachmentsMissingFromDocument(runtime, binding);
            applyComposerDocumentPanel(runtime, relayRuntime);
            const skui::Selection selection = runtime.selection();
            if (selection.rangeCount > 0) {
                (void)runtime.scrollIntoViewById(selection.focusNodeId);
            }
            return;
        }
        if ((event.type == skui::ElementEventType::MouseDown ||
             event.type == skui::ElementEventType::MouseUp) &&
            event.button == skui::MouseButton::Left) {
            rememberComposerSelection(runtime, binding);
        }

        if (event.type == skui::ElementEventType::MouseUp &&
            event.button == skui::MouseButton::Right &&
            event.action.starts_with(imageContextPrefix)) {
            const std::optional<std::filesystem::path> imagePath =
                tryFilesystemPathFromUtf8(
                    std::string_view(event.action).substr(
                        imageContextPrefix.size()));
            if (imagePath.has_value()) {
                std::error_code error;
                if (std::filesystem::is_regular_file(imagePath.value(), error) &&
                    !error) {
                    gImageContextPath = imagePath.value();
                    showImageMessageContextMenu(runtime, event.x, event.y);
                }
            } else {
                gImageContextPath.clear();
            }
            return;
        }

        if (event.type == skui::ElementEventType::MouseUp &&
            event.button == skui::MouseButton::Right &&
            event.action.starts_with(fileContextPrefix)) {
            gFileContextPath.clear();
            hideMessageContextMenu(runtime);
            const std::optional<std::filesystem::path> filePath =
                tryFilesystemPathFromUtf8(
                    std::string_view(event.action).substr(
                        fileContextPrefix.size()));
            if (filePath.has_value()) {
                std::error_code error;
                if (std::filesystem::is_regular_file(filePath.value(), error) &&
                    !error) {
                    gFileContextPath = filePath.value();
                    showFileMessageContextMenu(runtime, event.x, event.y);
                }
            }
            return;
        }

        if (event.type == skui::ElementEventType::MouseUp &&
            event.button == skui::MouseButton::Right &&
            event.tag == "selectable" &&
            (eventHasClass(event, "bubble") ||
             eventHasClass(event, "message-document-text"))) {
            gMessageContextText = event.value;
            showTextMessageContextMenu(runtime, event.x, event.y);
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
        if (event.type != skui::ElementEventType::Click ||
            event.button != skui::MouseButton::Left ||
            event.action.empty()) {
            return;
        }

        const std::string_view action(event.action);
        if (handleTransferCardAction(action, relayRuntime)) {
            hideMessageContextMenu(runtime);
        } else if (action.starts_with(imageContextPrefix)) {
            hideMessageContextMenu(runtime);
            const std::optional<std::filesystem::path> imagePath =
                tryFilesystemPathFromUtf8(
                    action.substr(imageContextPrefix.size()));
            if (imagePath.has_value()) {
                std::error_code error;
                if (std::filesystem::is_regular_file(imagePath.value(), error) &&
                    !error) {
                    showImagePreview(runtime, imagePath.value());
                }
            }
        } else if (action.starts_with(devicePrefix)) {
            hideMessageContextMenu(runtime);
            hideImagePreview(runtime);
            relayRuntime.selectPeer(std::string(action.substr(devicePrefix.size())));
            applyRelayDeskDevicePanel(runtime,
                                      relayRuntime,
                                      binding,
                                      ChatScrollUpdateMode::ScrollToLatest);
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
        } else if (action == "reveal-image-context") {
            (void)relaydesk::platform::revealPathInFileManager(
                gImageContextPath);
            hideMessageContextMenu(runtime);
        } else if (action == "copy-image-context") {
            (void)relaydesk::platform::copyImageFileToClipboard(
                gImageContextPath);
            hideMessageContextMenu(runtime);
        } else if (action == "save-image-context") {
            hideMessageContextMenu(runtime);
            if (saveFileAs(gImageContextPath) == SaveFileResult::Failed) {
                MessageBoxW(binding.window,
                            L"无法保存图片，请检查目标位置是否可写。",
                            L"RelayDesk",
                            MB_OK | MB_ICONERROR);
            }
        } else if (action == "reveal-file-context") {
            (void)relaydesk::platform::revealPathInFileManager(
                gFileContextPath);
            hideMessageContextMenu(runtime);
        } else if (action == "copy-file-context") {
            if (!relaydesk::platform::copyAttachmentPathToClipboard(
                    gFileContextPath)) {
                MessageBoxW(binding.window,
                            L"无法复制文件，请稍后重试。",
                            L"RelayDesk",
                            MB_OK | MB_ICONERROR);
            }
            hideMessageContextMenu(runtime);
        } else if (action == "save-file-context") {
            hideMessageContextMenu(runtime);
            if (saveFileAs(gFileContextPath) == SaveFileResult::Failed) {
                MessageBoxW(binding.window,
                            L"无法保存文件，请检查目标位置是否可写。",
                            L"RelayDesk",
                            MB_OK | MB_ICONERROR);
            }
        } else if (action == "close-image-preview") {
            hideImagePreview(runtime);
        } else if (action.starts_with(removeAttachmentPrefix)) {
            const std::string_view attachmentId =
                action.substr(removeAttachmentPrefix.size());
            if (removeComposerAttachment(runtime, binding, attachmentId)) {
                applyComposerDocumentPanel(runtime, relayRuntime);
            }
        } else if (action == "select-attachment-files") {
            hideMessageContextMenu(runtime);
            if (addComposerAttachmentPaths(
                    runtime,
                    binding,
                    relaydesk::platform::selectFilesFromDialog())) {
                applyComposerDocumentPanel(runtime, relayRuntime);
            }
        } else if (action == "select-attachment-folder") {
            hideMessageContextMenu(runtime);
            const std::optional<std::filesystem::path> folderPath =
                relaydesk::platform::selectFolderFromDialog();
            if (folderPath.has_value() &&
                addComposerAttachmentPaths(
                    runtime, binding, {folderPath.value()})) {
                applyComposerDocumentPanel(runtime, relayRuntime);
            }
        } else if (action == "send-message") {
            hideMessageContextMenu(runtime);
            (void)sendComposerMessage(runtime, relayRuntime, binding);
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
    relaydesk::platform::initializeAttachmentDropTarget();
    (void)addComposerAttachmentPaths(
        *binding.skiaRuntime,
        binding,
        relaydesk::platform::consumeDroppedAttachmentPaths());
    const std::vector<std::string> changedAttachmentIds =
        pollPendingFolderSizeResults(binding);
    refreshComposerAttachmentCards(*binding.skiaRuntime,
                                   binding,
                                   changedAttachmentIds);
    binding.relayRuntime->refreshPeersIfNeeded();
    const std::string nextSignature =
        makeRelayDeskUiSignature(*binding.relayRuntime, binding);
    const bool scrollChatToLatest = !binding.chatInitialized;
    if (!force && !scrollChatToLatest &&
        !binding.chatHistoryPrependPending &&
        nextSignature == binding.lastDeviceSignature) {
        return false;
    }

    binding.lastDeviceSignature = nextSignature;
    const ChatScrollUpdateMode scrollUpdateMode = scrollChatToLatest
        ? ChatScrollUpdateMode::ScrollToLatest
        : binding.chatHistoryPrependPending
            ? ChatScrollUpdateMode::PreserveViewportAfterPrepend
            : ChatScrollUpdateMode::PreserveOffset;
    applyRelayDeskDevicePanel(*binding.skiaRuntime,
                              *binding.relayRuntime,
                              binding,
                              scrollUpdateMode);
    binding.chatInitialized = true;
    binding.chatHistoryPrependPending = false;
    return true;
}

void CALLBACK refreshRelayDeskSkiaUiDeferred(
    HWND,
    UINT,
    UINT_PTR timerId,
    DWORD)
{
    KillTimer(nullptr, timerId);
    if (gRuntimeBinding == nullptr) {
        return;
    }

    gRuntimeBinding->deferredRefreshTimerId = 0;
    if (refreshRelayDeskDevicePanelIfChanged(*gRuntimeBinding, false)) {
        requestSkiaUiWindowRedraw(*gRuntimeBinding);
    }
}

void CALLBACK refreshRelayDeskSkiaUiTimer(HWND, UINT, UINT_PTR, DWORD)
{
    if (gRuntimeBinding == nullptr) {
        return;
    }
    if (refreshRelayDeskDevicePanelIfChanged(*gRuntimeBinding, false)) {
        requestSkiaUiWindowRedraw(*gRuntimeBinding);
    }
    if (gRuntimeBinding->backgroundController != nullptr) {
        gRuntimeBinding->backgroundController->processRuntimeState();
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
    relaydesk::storage::TransferState transferState,
    std::string localPath = "capture")
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(std::move(partId));
    part.SetType(type);
    part.SetTransferId("capture-transfer-" + part.GetPartId());
    part.SetFileName(std::move(fileName));
    part.SetFileSize(fileSize);
    part.SetTransferredSize(transferredSize);
    part.SetTransferState(transferState);
    part.SetLocalPath(std::move(localPath));
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

struct CaptureTransferActionScenario {
    relaydesk::storage::MessageDirection direction =
        relaydesk::storage::MessageDirection::Outgoing;
    relaydesk::storage::TransferState state =
        relaydesk::storage::TransferState::Pending;
    relaydesk::storage::DeliveryState deliveryState =
        relaydesk::storage::DeliveryState::Delivered;
    relaydesk::storage::MessagePartType partType =
        relaydesk::storage::MessagePartType::File;
    std::optional<OpenableTransferPath> openablePath;
    bool incomingTargetExists = false;
};

struct ExpectedTransferAction {
    std::string_view label;
    std::string_view actionPrefix;
    bool primary = false;
};

std::string makeCaptureTransferActionsMarkup(
    const CaptureTransferActionScenario& scenario)
{
    const relaydesk::storage::ChatMessagePart part = makeCaptureFilePart(
        "capture-action-part",
        scenario.partType,
        scenario.partType == relaydesk::storage::MessagePartType::Folder
            ? "capture-action-folder"
            : "capture-action.bin",
        1024,
        0,
        scenario.state);
    relaydesk::storage::ChatMessageRecord message = makeCaptureMessage(
        "capture-action-message",
        scenario.direction,
        "2026-07-09T09:35:00Z",
        {part});
    message.SetDeliveryState(scenario.deliveryState);
    return makeTransferActionsMarkup(message,
                                     part,
                                     scenario.openablePath,
                                     scenario.incomingTargetExists);
}

bool transferActionsMatch(
    const std::string& markup,
    std::initializer_list<ExpectedTransferAction> expectedActions)
{
    constexpr std::string_view kButtonMarker =
        R"(class="transfer-action-button)";
    constexpr std::string_view kActionMarker = R"(data-action=")";
    std::size_t cursor = 0;
    for (const ExpectedTransferAction& expected : expectedActions) {
        const std::size_t buttonStart = markup.find(kButtonMarker, cursor);
        if (buttonStart == std::string::npos) {
            return false;
        }
        const std::size_t classValueStart = buttonStart + kButtonMarker.size();
        const std::size_t classValueEnd = markup.find('"', classValueStart);
        if (classValueEnd == std::string::npos) {
            return false;
        }
        const bool primary =
            markup.substr(classValueStart, classValueEnd - classValueStart) ==
            " primary";
        if (primary != expected.primary) {
            return false;
        }

        const std::size_t actionMarkerStart =
            markup.find(kActionMarker, classValueEnd);
        if (actionMarkerStart == std::string::npos) {
            return false;
        }
        const std::size_t actionStart =
            actionMarkerStart + kActionMarker.size();
        if (markup.compare(actionStart,
                           expected.actionPrefix.size(),
                           expected.actionPrefix) != 0) {
            return false;
        }
        const std::size_t actionEnd = markup.find('"', actionStart);
        const std::size_t labelStart = markup.find('>', actionEnd);
        if (actionEnd == std::string::npos || labelStart == std::string::npos) {
            return false;
        }
        const std::size_t labelEnd = markup.find("</div>", labelStart + 1u);
        if (labelEnd == std::string::npos ||
            markup.substr(labelStart + 1u, labelEnd - labelStart - 1u) !=
                expected.label) {
            return false;
        }
        cursor = labelEnd + 6u;
    }
    return markup.find(kButtonMarker, cursor) == std::string::npos &&
        (expectedActions.size() != 0u || markup.empty());
}

bool captureTransferActionMatrixMatchesOriginal()
{
    constexpr std::string_view kCleanedMarkup =
        R"(<div class="transfer-cleaned-notice">已清理</div>)";
    const OpenableTransferPath filePath{
        std::filesystem::path("capture-action.bin"), false};
    const OpenableTransferPath folderPath{
        std::filesystem::path("capture-action-folder"), true};
    CaptureTransferActionScenario scenario;

    if (!transferActionsMatch(makeCaptureTransferActionsMarkup(scenario), {})) {
        return false;
    }

    scenario.state = relaydesk::storage::TransferState::Offered;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"主动发送", kTransferSendActionPrefix, true},
             {"取消", kTransferCancelActionPrefix, false}})) {
        return false;
    }

    scenario.state = relaydesk::storage::TransferState::Transferring;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"取消", kTransferCancelActionPrefix, false}})) {
        return false;
    }

    scenario.state = relaydesk::storage::TransferState::Interrupted;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"继续发送", kTransferSendActionPrefix, true}})) {
        return false;
    }

    scenario.state = relaydesk::storage::TransferState::Failed;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"重新发送", kTransferSendActionPrefix, true}})) {
        return false;
    }
    scenario.deliveryState = relaydesk::storage::DeliveryState::Failed;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"重新发送", kTransferResendMessageActionPrefix, true}})) {
        return false;
    }

    scenario.deliveryState = relaydesk::storage::DeliveryState::Delivered;
    for (const relaydesk::storage::TransferState openState : {
             relaydesk::storage::TransferState::Completed,
             relaydesk::storage::TransferState::Cancelled}) {
        scenario.state = openState;
        scenario.openablePath = filePath;
        if (!transferActionsMatch(
                makeCaptureTransferActionsMarkup(scenario),
                {{"打开", kTransferOpenActionPrefix, true},
                 {"打开文件夹", kTransferRevealActionPrefix, false}})) {
            return false;
        }
        scenario.openablePath.reset();
        if (makeCaptureTransferActionsMarkup(scenario) != kCleanedMarkup) {
            return false;
        }
    }

    scenario.state = relaydesk::storage::TransferState::Rejected;
    scenario.partType = relaydesk::storage::MessagePartType::Folder;
    scenario.openablePath = folderPath;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"打开文件夹", kTransferOpenActionPrefix, false}})) {
        return false;
    }
    scenario.openablePath.reset();
    if (makeCaptureTransferActionsMarkup(scenario) != kCleanedMarkup) {
        return false;
    }

    scenario = CaptureTransferActionScenario{};
    scenario.direction = relaydesk::storage::MessageDirection::Incoming;
    if (!transferActionsMatch(makeCaptureTransferActionsMarkup(scenario), {})) {
        return false;
    }

    scenario.state = relaydesk::storage::TransferState::Offered;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"接收", kTransferAcceptActionPrefix, true},
             {"另存为", kTransferSaveAsActionPrefix, false},
             {"拒绝", kTransferRejectActionPrefix, false}})) {
        return false;
    }
    scenario.incomingTargetExists = true;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"接收", kTransferAcceptActionPrefix, true},
             {"另存为", kTransferSaveAsActionPrefix, false},
             {"覆盖", kTransferOverwriteActionPrefix, false},
             {"拒绝", kTransferRejectActionPrefix, false}})) {
        return false;
    }

    scenario.incomingTargetExists = false;
    scenario.state = relaydesk::storage::TransferState::Transferring;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"取消", kTransferCancelActionPrefix, false}})) {
        return false;
    }

    scenario.state = relaydesk::storage::TransferState::Interrupted;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"继续接收", kTransferAcceptActionPrefix, true}})) {
        return false;
    }

    scenario.state = relaydesk::storage::TransferState::Completed;
    scenario.openablePath = filePath;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"打开", kTransferOpenActionPrefix, true},
             {"打开文件夹", kTransferRevealActionPrefix, false}})) {
        return false;
    }
    scenario.openablePath.reset();
    if (makeCaptureTransferActionsMarkup(scenario) != kCleanedMarkup) {
        return false;
    }

    scenario.state = relaydesk::storage::TransferState::Cancelled;
    scenario.openablePath = filePath;
    if (!transferActionsMatch(
            makeCaptureTransferActionsMarkup(scenario),
            {{"打开", kTransferOpenActionPrefix, true},
             {"打开文件夹", kTransferRevealActionPrefix, false}})) {
        return false;
    }
    scenario.openablePath.reset();
    if (!transferActionsMatch(makeCaptureTransferActionsMarkup(scenario), {})) {
        return false;
    }

    for (const relaydesk::storage::TransferState emptyState : {
             relaydesk::storage::TransferState::Failed,
             relaydesk::storage::TransferState::Rejected}) {
        scenario.state = emptyState;
        if (!transferActionsMatch(
                makeCaptureTransferActionsMarkup(scenario), {})) {
            return false;
        }
    }

    const relaydesk::storage::ChatMessagePart offeredPart = makeCaptureFilePart(
        "capture-standalone-action",
        relaydesk::storage::MessagePartType::File,
        "standalone-action.bin",
        1024,
        0,
        relaydesk::storage::TransferState::Offered);
    relaydesk::storage::ChatMessageRecord offeredMessage = makeCaptureMessage(
        "capture-standalone-action-message",
        relaydesk::storage::MessageDirection::Outgoing,
        "2026-07-09T09:35:00Z",
        {offeredPart});
    const std::string standaloneMarkup =
        makeTransferMessageMarkup(offeredMessage, offeredPart);
    offeredMessage.SetParts(
        {makeCaptureTextPart("capture-compound-text", "file follows"),
         offeredPart});
    const std::string compoundMarkup = makeCompoundMessageMarkup(offeredMessage);
    return standaloneMarkup.find(R"(class="transfer-actions")") !=
            std::string::npos &&
        standaloneMarkup.find(">主动发送</div>") != std::string::npos &&
        compoundMarkup.find(R"(class="message-document-file)") !=
            std::string::npos &&
        compoundMarkup.find(R"(class="transfer-actions")") !=
            std::string::npos &&
        compoundMarkup.find(">主动发送</div>") != std::string::npos &&
        transferStateText(relaydesk::storage::TransferState::Failed, true) ==
            "发送失败" &&
        transferStateText(relaydesk::storage::TransferState::Failed, false) ==
            "接收失败" &&
        transferStateText(relaydesk::storage::TransferState::Interrupted, true) ==
            "发送中断" &&
        transferStateText(relaydesk::storage::TransferState::Interrupted, false) ==
            "接收中断";
}

std::vector<relaydesk::storage::ChatMessageRecord> makeCaptureChatMessages(
    const std::string& imagePath = "capture")
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
                             relaydesk::storage::TransferState::Completed,
                             imagePath)}));
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

std::vector<relaydesk::storage::ChatMessageRecord>
makeCapturePagedChatMessages()
{
    std::vector<relaydesk::storage::ChatMessageRecord> messages;
    messages.reserve(kCaptureHistoryMessageCount);
    for (std::size_t index = 0; index < kCaptureHistoryMessageCount; ++index) {
        const std::string messageNumber = std::to_string(index + 1u);
        const auto direction = index % 2u == 0u
            ? relaydesk::storage::MessageDirection::Incoming
            : relaydesk::storage::MessageDirection::Outgoing;
        messages.push_back(makeCaptureMessage(
            "pagination-message-" + messageNumber,
            direction,
            "2026-07-09T06:48:00Z",
            {makeCaptureTextPart("pagination-part-" + messageNumber,
                                 "pagination-message-" + messageNumber)}));
    }
    return messages;
}

std::vector<relaydesk::storage::ChatMessageRecord> makeCaptureFileCardMessages(
    const std::string& existingFilePath)
{
    std::vector<relaydesk::storage::ChatMessageRecord> messages;
    messages.push_back(makeCaptureMessage(
        "capture-file-filler",
        relaydesk::storage::MessageDirection::Outgoing,
        "2026-07-09T09:35:00Z",
        {makeCaptureFilePart("filler-file",
                             relaydesk::storage::MessagePartType::File,
                             "capture-layout-baseline.dat",
                             2048,
                             2048,
                             relaydesk::storage::TransferState::Completed,
                             "capture")}));
    messages.push_back(makeCaptureMessage(
        "capture-file-short",
        relaydesk::storage::MessageDirection::Outgoing,
        "2026-07-09T09:36:00Z",
        {makeCaptureFilePart("short-file",
                             relaydesk::storage::MessagePartType::File,
                             "dynamic_dom.html",
                             11200,
                             11200,
                             relaydesk::storage::TransferState::Completed,
                             existingFilePath)}));
    messages.push_back(makeCaptureMessage(
        "capture-file-long",
        relaydesk::storage::MessageDirection::Outgoing,
        "2026-07-09T09:37:00Z",
        {makeCaptureFilePart(
            "long-file",
            relaydesk::storage::MessagePartType::File,
            "f224b2fa81c6d8465bdd43a796fb94e84d4bede67dd08e86b75f0fa5242c9c899"
            "7047aa9bf78193d7b4dd17798c1f551f.png",
            842300,
            842300,
            relaydesk::storage::TransferState::Completed,
            "capture")}));
    return messages;
}

class CaptureRelayDeskRuntime : public relaydesk::runtime::RelayDeskRuntime {
public:
    explicit CaptureRelayDeskRuntime(std::string imagePath = "capture")
        : RelayDeskRuntime(makeCaptureRuntimeOptions()),
          imagePath_(std::move(imagePath))
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
        selectedPeerMessages_ = makeCaptureChatMessages(imagePath_);
    }

    void showShortConversation()
    {
        std::vector<relaydesk::storage::ChatMessageRecord> messages =
            makeCaptureChatMessages(imagePath_);
        messages.resize(1);
        selectedPeerMessages_ = std::move(messages);
    }

    void showFullConversation()
    {
        selectedPeerMessages_ = makeCaptureChatMessages(imagePath_);
    }

    void showPagedConversation()
    {
        pagedMessages_ = makeCapturePagedChatMessages();
        const auto initialPageBegin =
            pagedMessages_.end() -
            static_cast<std::ptrdiff_t>(kCaptureHistoryPageSize);
        selectedPeerMessages_.assign(initialPageBegin, pagedMessages_.end());
        selectedPeerHasMoreMessages_ = true;
    }

    void loadMorePagedConversation()
    {
        const std::size_t firstLoadedIndex =
            pagedMessages_.size() - selectedPeerMessages_.size();
        const std::size_t loadCount =
            std::min(kCaptureHistoryPageSize, firstLoadedIndex);
        const auto olderMessagesBegin =
            pagedMessages_.begin() +
            static_cast<std::ptrdiff_t>(firstLoadedIndex - loadCount);
        const auto olderMessagesEnd =
            pagedMessages_.begin() +
            static_cast<std::ptrdiff_t>(firstLoadedIndex);
        selectedPeerMessages_.insert(selectedPeerMessages_.begin(),
                                     olderMessagesBegin,
                                     olderMessagesEnd);
        selectedPeerHasMoreMessages_ = firstLoadedIndex > loadCount;
    }

    void showImageConversation()
    {
        std::vector<relaydesk::storage::ChatMessageRecord> messages =
            makeCaptureChatMessages(imagePath_);
        relaydesk::storage::ChatMessageRecord compoundMessage =
            std::move(messages.at(2));
        std::vector<relaydesk::storage::ChatMessagePart> parts;
        parts.push_back(makeCaptureTextPart("compound-text", "text before attachments"));
        parts.push_back(compoundMessage.GetParts().front());
        parts.push_back(makeCaptureFilePart(
            "compound-file",
            relaydesk::storage::MessagePartType::File,
            "relaydesk_vscode_icon_resources.rc",
            6900,
            0,
            relaydesk::storage::TransferState::Cancelled,
            "capture"));
        compoundMessage.SetParts(std::move(parts));
        selectedPeerMessages_ = {std::move(compoundMessage)};
    }

    void showFileCardConversation(const std::string& existingFilePath)
    {
        selectedPeerMessages_ =
            makeCaptureFileCardMessages(existingFilePath);
    }

protected:
    std::string imagePath_;
    std::vector<relaydesk::storage::ChatMessageRecord> pagedMessages_;
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
        } else if (argument == L"--capture-test-peer-switch") {
            options.testPeerSwitch = true;
        } else if (argument == L"--capture-test-image-message") {
            options.testImageMessage = true;
        } else if (argument == L"--capture-test-file-card") {
            options.testFileCard = true;
        } else if (argument == L"--capture-test-composer-attachments") {
            options.testComposerAttachments = true;
        } else if (argument == L"--capture-test-composer-keyboard") {
            options.testComposerKeyboard = true;
        } else if (argument == L"--capture-test-history-pagination") {
            options.testHistoryPagination = true;
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

bool writeCaptureImageFixture(const std::filesystem::path& outputPath)
{
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    const std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight),
        0xFFFF00FFu);
    return writePngFile(outputPath,
                        pixels,
                        kWidth,
                        kHeight,
                        static_cast<std::size_t>(kWidth) *
                            sizeof(std::uint32_t));
}

bool writeCaptureFileFixture(const std::filesystem::path& outputPath,
                             std::size_t size)
{
    std::error_code error;
    std::filesystem::create_directories(outputPath.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    const std::string content(size, 'x');
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return output.good();
}

struct CapturePixelBounds {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

std::vector<CapturePixelBounds> findTransferCardPixelBounds(
    const std::vector<std::uint32_t>& pixels,
    int width,
    int height)
{
    constexpr std::uint32_t kTransferCardBackground = 0xFFE9FBFAu;
    constexpr int kMinimumBackgroundPixelsPerRow = 100;
    constexpr int kMinimumCardSpan = 200;
    constexpr int kMaximumInternalGap = 12;
    std::vector<CapturePixelBounds> regions;
    for (int y = 0; y < height; ++y) {
        int left = width;
        int right = -1;
        int count = 0;
        for (int x = kChatMessagePaneLeft; x < width; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);
            if (pixels[index] != kTransferCardBackground) {
                continue;
            }
            left = std::min(left, x);
            right = std::max(right, x);
            ++count;
        }
        if (count < kMinimumBackgroundPixelsPerRow ||
            right - left + 1 < kMinimumCardSpan) {
            continue;
        }
        if (!regions.empty() &&
            y <= regions.back().bottom + kMaximumInternalGap + 1) {
            regions.back().left = std::min(regions.back().left, left);
            regions.back().right = std::max(regions.back().right, right);
            regions.back().bottom = y;
        } else {
            regions.push_back({left, y, right, y});
        }
    }
    return regions;
}

std::optional<CapturePixelBounds> findCaptureImagePixelBounds(
    const std::vector<std::uint32_t>& pixels,
    int width,
    int height)
{
    CapturePixelBounds bounds{width, height, -1, -1};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint32_t pixel =
                pixels[static_cast<std::size_t>(y) *
                           static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)];
            const std::uint32_t red = (pixel >> 16u) & 0xFFu;
            const std::uint32_t green = (pixel >> 8u) & 0xFFu;
            const std::uint32_t blue = pixel & 0xFFu;
            if (red < 240u || green > 20u || blue < 240u) {
                continue;
            }
            bounds.left = std::min(bounds.left, x);
            bounds.top = std::min(bounds.top, y);
            bounds.right = std::max(bounds.right, x);
            bounds.bottom = std::max(bounds.bottom, y);
        }
    }
    if (bounds.right < bounds.left || bounds.bottom < bounds.top) {
        return std::nullopt;
    }
    return bounds;
}

int findRightmostTransferProgressPixel(
    const std::vector<std::uint32_t>& pixels,
    int width,
    const CapturePixelBounds& bounds)
{
    constexpr std::uint32_t kTransferAccent = 0xFF0AA39Eu;
    constexpr int kMinimumProgressSpan = 100;
    int rightmost = -1;
    for (int y = bounds.top; y <= bounds.bottom; ++y) {
        int runStart = -1;
        for (int x = bounds.left; x <= bounds.right; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);
            if (pixels[index] == kTransferAccent) {
                if (runStart < 0) {
                    runStart = x;
                }
                if (x - runStart + 1 >= kMinimumProgressSpan) {
                    rightmost = std::max(rightmost, x);
                }
            } else {
                runStart = -1;
            }
        }
    }
    return rightmost;
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

    std::filesystem::path captureImagePath;
    if (options.testImageMessage || options.testComposerAttachments) {
        captureImagePath = outputPath.parent_path() /
            "relaydesk_skiaui_capture_message.png";
        if (!writeCaptureImageFixture(captureImagePath)) {
            return 9;
        }
    }
    const auto captureImagePathText = captureImagePath.u8string();
    CaptureRelayDeskRuntime relayRuntime(
        captureImagePathText.empty()
            ? "capture"
            : std::string(captureImagePathText.begin(),
                          captureImagePathText.end()));
    if (options.testImageMessage) {
        relayRuntime.showImageConversation();
    }
    if (options.testComposerAttachments) {
        relayRuntime.showShortConversation();
    }
    if (options.testHistoryPagination) {
        relayRuntime.showPagedConversation();
    }
    if (options.testFileCard) {
        if (!captureTransferActionMatrixMatchesOriginal()) {
            return 61;
        }
        const relaydesk::storage::AppPaths appPaths =
            relaydesk::storage::createAppPaths();
        const std::string executablePath =
            filesystemPathToGenericUtf8(appPaths.GetExecutablePath());
        relayRuntime.showFileCardConversation(executablePath);
        const std::string fileCardMarkup =
            makeChatContentMarkup(relayRuntime);
        constexpr std::string_view kCleanedNoticeMarker =
            R"(class="transfer-cleaned-notice">已清理)";
        const std::size_t firstCleanedNotice = fileCardMarkup.find(
            kCleanedNoticeMarker);
        const std::size_t secondCleanedNotice = fileCardMarkup.find(
            kCleanedNoticeMarker,
            firstCleanedNotice == std::string::npos
                ? 0u
                : firstCleanedNotice + kCleanedNoticeMarker.size());
        if (firstCleanedNotice == std::string::npos ||
            secondCleanedNotice == std::string::npos ||
            fileCardMarkup.find(
                "已清理",
                secondCleanedNotice + kCleanedNoticeMarker.size()) !=
                std::string::npos) {
            return 24;
        }
        if (fileCardMarkup.find(
                R"(class="transfer-meta">已完成)") ==
            std::string::npos) {
            return 25;
        }
        if (fileCardMarkup.find(R"(data-action="file-context:)") ==
            std::string::npos) {
            return 27;
        }
        if (html.find(R"(data-action="reveal-file-context")") ==
                std::string::npos ||
            html.find(R"(data-action="copy-file-context")") ==
                std::string::npos ||
            html.find(R"(data-action="save-file-context")") ==
                std::string::npos) {
            return 28;
        }
        const relaydesk::storage::ChatMessagePart cancelledPart =
            makeCaptureFilePart(
                "cancelled-file",
                relaydesk::storage::MessagePartType::File,
                "cancelled.bin",
                1,
                0,
                relaydesk::storage::TransferState::Cancelled,
                "capture");
        if (transferStateText(cancelledPart, true) != "已取消") {
            return 26;
        }
    }
    if (options.testImageMessage) {
        const std::string compoundMessageMarkup =
            makeChatContentMarkup(relayRuntime);
        const std::size_t firstMessageRow =
            compoundMessageMarkup.find("class=\"message-row ");
        if (compoundMessageMarkup.find("class=\"message-image\"") ==
                std::string::npos ||
            compoundMessageMarkup.find("message-document") ==
                std::string::npos ||
            compoundMessageMarkup.find(
                R"(contenteditable="true" aria-readonly="true")") ==
                std::string::npos ||
            compoundMessageMarkup.find(R"(contenteditable="false")") ==
                std::string::npos ||
            firstMessageRow == std::string::npos ||
            compoundMessageMarkup.find(
                "class=\"message-row ", firstMessageRow + 1u) !=
                std::string::npos) {
            return 10;
        }
    }
    if (options.testPeerSwitch) {
        relayRuntime.showShortConversation();
    }
    skui::Runtime runtime(runtimeOptions);
    SkiaUiRuntimeBinding binding;
    binding.relayRuntime = &relayRuntime;
    binding.skiaRuntime = &runtime;
    if (options.testHistoryPagination) {
        binding.loadMoreSelectedPeerMessages = [&relayRuntime]() {
            relayRuntime.loadMorePagedConversation();
        };
    }
    if (options.testComposerAttachments) {
        if (html.find(R"(data-action="select-attachment-files")") ==
                std::string::npos ||
            html.find(R"(data-action="select-attachment-folder")") ==
                std::string::npos ||
            html.find(R"(id="composer-document" class="composer-document" contenteditable="true")") ==
                std::string::npos ||
            html.find(R"(<p id="composer-text-initial")") ==
                std::string::npos ||
            html.find("<textarea") != std::string::npos) {
            return 30;
        }

        ComposerAttachment imageAttachment;
        imageAttachment.kind = ComposerAttachmentKind::Image;
        imageAttachment.attachmentId = "capture-image-attachment";
        imageAttachment.displayName = "产品界面参考图.png";
        imageAttachment.localPath =
            filesystemPathToGenericUtf8(captureImagePath);
        imageAttachment.sourcePath = captureImagePath;
        imageAttachment.previewPath = captureImagePath;
        imageAttachment.fileSize = fileSizeOrZero(captureImagePath);
        binding.composerAttachments.push_back(std::move(imageAttachment));
        const std::string imageAttachmentMarkup =
            makeComposerAttachmentMarkup(binding.composerAttachments.front());
        const std::size_t imageNamePosition = imageAttachmentMarkup.find(
            binding.composerAttachments.front().displayName);
        if (imageAttachmentMarkup.find("composer-attachment-image") ==
                std::string::npos ||
            imageAttachmentMarkup.find(R"(contenteditable="false")") ==
                std::string::npos ||
            imageAttachmentMarkup.find(
                R"(data-clipboard-kind="image")") ==
                std::string::npos ||
            imageAttachmentMarkup.find(
                R"(data-clipboard-path=")") ==
                std::string::npos ||
            imageAttachmentMarkup.find(R"(data-action="image-context:)") ==
                std::string::npos ||
            imageAttachmentMarkup.find(
                R"(style="width: 249px; height: 140px;")") ==
                std::string::npos ||
            imageNamePosition == std::string::npos ||
            imageAttachmentMarkup.find(
                binding.composerAttachments.front().displayName,
                imageNamePosition +
                    binding.composerAttachments.front().displayName.size()) !=
                std::string::npos) {
            return 48;
        }

        const std::filesystem::path fixtureDirectory =
            outputPath.parent_path() / "composer_attachment_fixture";
        const std::filesystem::path documentPath =
            fixtureDirectory / L"项目需求说明.txt";
        const std::filesystem::path nestedFilePath =
            fixtureDirectory / "assets" / "reference.bin";
        if (!writeCaptureFileFixture(documentPath, 1536u) ||
            !writeCaptureFileFixture(nestedFilePath, 2048u)) {
            return 31;
        }

        const std::optional<ComposerAttachment> documentAttachment =
            makeComposerAttachmentFromPath(documentPath);
        const std::optional<ComposerAttachment> folderAttachment =
            makeComposerAttachmentFromPath(fixtureDirectory);
        if (!documentAttachment.has_value() ||
            !folderAttachment.has_value()) {
            return 32;
        }
        binding.composerAttachments.push_back(documentAttachment.value());
        constexpr std::string_view kLongCaptureFileName =
            "relaydesk_vscode_icon_resources.rc";
        binding.composerAttachments.back().displayName = kLongCaptureFileName;
        const std::string fileAttachmentMarkup =
            makeComposerAttachmentMarkup(binding.composerAttachments.back());
        const std::size_t fullNamePosition =
            fileAttachmentMarkup.find(kLongCaptureFileName);
        const std::size_t secondFullNamePosition =
            fullNamePosition == std::string::npos
            ? std::string::npos
            : fileAttachmentMarkup.find(
                  kLongCaptureFileName,
                  fullNamePosition + kLongCaptureFileName.size());
        if (fullNamePosition == std::string::npos ||
            fileAttachmentMarkup.find(
                R"(data-clipboard-kind="file")") ==
                std::string::npos ||
            secondFullNamePosition == std::string::npos ||
            fileAttachmentMarkup.find(
                kLongCaptureFileName,
                secondFullNamePosition + kLongCaptureFileName.size()) !=
                std::string::npos) {
            return 49;
        }
        binding.composerAttachments.push_back(folderAttachment.value());
        startPendingFolderSizeProbe(binding.composerAttachments.back());
        const std::string pendingMarkup =
            makeComposerAttachmentMarkup(binding.composerAttachments);
        if (pendingMarkup.find("正在计算容量...") == std::string::npos) {
            return 33;
        }

        bool folderSizeCompleted = false;
        constexpr int kMaximumFolderSizePollAttempts = 100;
        for (int attempt = 0;
             attempt < kMaximumFolderSizePollAttempts && !folderSizeCompleted;
             ++attempt) {
            (void)core::async::dispatchReady();
            (void)pollPendingFolderSizeResults(binding);
            folderSizeCompleted = std::any_of(
                binding.composerAttachments.begin(),
                binding.composerAttachments.end(),
                [](const ComposerAttachment& attachment) {
                    return attachment.kind == ComposerAttachmentKind::Folder &&
                        !attachment.fileSizePending &&
                        attachment.fileSize == 3584u;
                });
            if (!folderSizeCompleted) {
                Sleep(10);
            }
        }
        if (!folderSizeCompleted) {
            return 34;
        }
        const auto completedFolder = std::find_if(
            binding.composerAttachments.begin(),
            binding.composerAttachments.end(),
            [](const ComposerAttachment& attachment) {
                return attachment.kind == ComposerAttachmentKind::Folder &&
                    !attachment.fileSizePending;
            });
        if (completedFolder != binding.composerAttachments.end()) {
            completedFolder->displayName = "产品设计素材";
        }

        ComposerAttachment pendingFolder;
        pendingFolder.kind = ComposerAttachmentKind::Folder;
        pendingFolder.attachmentId = "capture-pending-folder";
        pendingFolder.displayName = "正在统计的设计素材";
        pendingFolder.localPath =
            filesystemPathToGenericUtf8(fixtureDirectory);
        pendingFolder.sourcePath = fixtureDirectory;
        pendingFolder.previewPath = fixtureDirectory;
        pendingFolder.fileSizePending = true;
        binding.composerAttachments.push_back(std::move(pendingFolder));

        const std::string attachmentMarkup =
            makeComposerAttachmentMarkup(binding.composerAttachments);
        if (attachmentMarkup.find("3.5 KB") == std::string::npos ||
            attachmentMarkup.find("正在计算容量...") == std::string::npos ||
            attachmentMarkup.find("data-action=\"remove-attachment:") ==
                std::string::npos) {
            return 35;
        }
    }
    installRelayDeskInteractions(runtime, relayRuntime, binding);
    const int initialWidth =
        options.initialWidth > 0 ? options.initialWidth : options.width;
    const int initialHeight =
        options.initialHeight > 0 ? options.initialHeight : options.height;
    if (options.testHistoryPagination) {
        runtime.beginUpdate();
    }
    runtime.resize(initialWidth, initialHeight, options.dpiScale);
    if (!runtime.loadDocumentFromString(html)) {
        if (options.testHistoryPagination) {
            runtime.endUpdate();
        }
        return 4;
    }
    if (options.testComposerAttachments) {
        constexpr std::string_view kCaptureComposerText =
            "请查收这些附件";
        if (!runtime.setTextById(kComposerInitialParagraphId,
                                 kCaptureComposerText) ||
            !runtime.collapseSelection(kComposerInitialParagraphId,
                                       kCaptureComposerText.size()) ||
            !runtime.insertHtmlAtSelection(
                kComposerDocumentId,
                makeComposerAttachmentMarkup(
                    binding.composerAttachments))) {
            return 36;
        }
        const std::filesystem::path pastedDocumentPath =
            outputPath.parent_path() / "composer_attachment_fixture" /
            L"项目需求说明.txt";
        const std::filesystem::path pastedFolderPath =
            outputPath.parent_path() / "composer_attachment_fixture";
        if (!verifyOrderedComposerClipboardPaste(
                captureImagePath, pastedDocumentPath)) {
            return 53;
        }
        binding.readClipboardAttachmentPaths = [captureImagePath,
                                                pastedDocumentPath,
                                                pastedFolderPath] {
            return std::vector<std::filesystem::path>{captureImagePath,
                                                      pastedDocumentPath,
                                                      pastedFolderPath};
        };
        const std::size_t attachmentCountBeforePaste =
            composerAttachmentCount(binding);
        skui::Event paste;
        paste.type = skui::EventType::KeyDown;
        paste.key = 'V';
        paste.ctrlKey = true;
        (void)runtime.handleEvent(paste);
        if (composerAttachmentCount(binding) !=
            attachmentCountBeforePaste + 3u) {
            return 44;
        }
        const std::vector<relaydesk::storage::ChatMessagePart> parts =
            makeComposerMessageParts(runtime, binding);
        if (parts.size() != 8u ||
            parts[0].GetType() != relaydesk::storage::MessagePartType::Text ||
            parts[1].GetType() != relaydesk::storage::MessagePartType::Image ||
            parts[2].GetType() != relaydesk::storage::MessagePartType::File ||
            parts[3].GetType() != relaydesk::storage::MessagePartType::Folder ||
            parts[3].GetFileSize().value_or(0u) != 3584u ||
            parts[5].GetType() != relaydesk::storage::MessagePartType::Image ||
            parts[6].GetType() != relaydesk::storage::MessagePartType::File ||
            parts[7].GetType() != relaydesk::storage::MessagePartType::Folder) {
            return 37;
        }
    }
    applyRelayDeskDevicePanel(runtime,
                              relayRuntime,
                              binding,
                              ChatScrollUpdateMode::ScrollToLatest);
    if (options.testHistoryPagination) {
        runtime.endUpdate();
        if (!isChatScrolledToLatest(runtime)) {
            return 62;
        }
        binding.documentLoaded = true;
        binding.chatInitialized = true;
        binding.lastDeviceSignature =
            makeRelayDeskUiSignature(relayRuntime, binding);
        const std::optional<skui::ScrollState> initialChatScroll =
            runtime.scrollStateById("chat-scroll");
        if (relayRuntime.GetSelectedPeerMessages().size() !=
                kCaptureHistoryPageSize ||
            !relayRuntime.GetSelectedPeerHasMoreMessages() ||
            !initialChatScroll.has_value() ||
            initialChatScroll->maxScrollY <= 48.0f) {
            return 54;
        }

        const auto loadNextHistoryPage =
            [&runtime, &options, &binding]() {
                if (!runtime.setScrollOffsetById(
                        "chat-scroll", 0.0f, 48.0f)) {
                    return false;
                }
                skui::Event mouseWheel;
                mouseWheel.type = skui::EventType::MouseWheel;
                mouseWheel.x = 800.0f * options.dpiScale;
                mouseWheel.y = 400.0f * options.dpiScale;
                mouseWheel.wheelDelta = 120.0f;
                if (!runtime.handleEvent(mouseWheel)) {
                    return false;
                }
                return refreshRelayDeskDevicePanelIfChanged(binding, false);
            };

        if (!loadNextHistoryPage()) {
            return 55;
        }
        const std::optional<skui::ScrollState> firstPageChatScroll =
            runtime.scrollStateById("chat-scroll");
        const std::optional<std::string> firstPageText =
            runtime.textContentById("chat-content");
        if (relayRuntime.GetSelectedPeerMessages().size() != 60u ||
            !relayRuntime.GetSelectedPeerHasMoreMessages() ||
            !firstPageChatScroll.has_value() ||
            !firstPageText.has_value() ||
            firstPageText->find("pagination-message-31") ==
                std::string::npos) {
            return 56;
        }
        const float expectedFirstPageScrollY =
            firstPageChatScroll->maxScrollY - initialChatScroll->maxScrollY;
        if (std::abs(firstPageChatScroll->scrollY -
                     expectedFirstPageScrollY) >
            kChatScrollBottomTolerance) {
            return 57;
        }

        if (!loadNextHistoryPage()) {
            return 58;
        }
        const std::optional<skui::ScrollState> secondPageChatScroll =
            runtime.scrollStateById("chat-scroll");
        const std::optional<std::string> secondPageText =
            runtime.textContentById("chat-content");
        if (relayRuntime.GetSelectedPeerMessages().size() !=
                kCaptureHistoryMessageCount ||
            relayRuntime.GetSelectedPeerHasMoreMessages() ||
            !secondPageChatScroll.has_value() ||
            !secondPageText.has_value() ||
            secondPageText->find("pagination-message-1") ==
                std::string::npos) {
            return 59;
        }
        const float expectedSecondPageScrollY =
            secondPageChatScroll->maxScrollY -
            firstPageChatScroll->maxScrollY;
        if (std::abs(secondPageChatScroll->scrollY -
                     expectedSecondPageScrollY) >
            kChatScrollBottomTolerance) {
            return 60;
        }
        (void)runtime.setScrollOffsetById(
            "chat-scroll", 0.0f, secondPageChatScroll->maxScrollY);
    }
    if (options.testComposerAttachments) {
        skui::Event imageMouseDown;
        imageMouseDown.type = skui::EventType::MouseDown;
        imageMouseDown.x = 600.0f * options.dpiScale;
        imageMouseDown.y = 670.0f * options.dpiScale;
        imageMouseDown.button = skui::MouseButton::Left;
        (void)runtime.handleEvent(imageMouseDown);
        skui::Event imageMouseUp = imageMouseDown;
        imageMouseUp.type = skui::EventType::MouseUp;
        (void)runtime.handleEvent(imageMouseUp);
        if (!gImagePreviewVisible) {
            return 50;
        }
        hideImagePreview(runtime);

        const std::size_t attachmentCountBeforeRemove =
            composerAttachmentCount(binding);
        const std::string removedAttachmentId =
            binding.composerAttachments.at(1).attachmentId;
        skui::Event removeMouseDown;
        removeMouseDown.type = skui::EventType::MouseDown;
        removeMouseDown.x = 1230.0f * options.dpiScale;
        removeMouseDown.y = 655.0f * options.dpiScale;
        removeMouseDown.button = skui::MouseButton::Left;
        (void)runtime.handleEvent(removeMouseDown);
        skui::Event removeMouseUp = removeMouseDown;
        removeMouseUp.type = skui::EventType::MouseUp;
        (void)runtime.handleEvent(removeMouseUp);
        if (composerAttachmentCount(binding) + 1u !=
                attachmentCountBeforeRemove ||
            findComposerAttachment(binding, removedAttachmentId) != nullptr) {
            return 51;
        }
    }
    if (options.testComposerKeyboard) {
        const std::size_t messageCountBefore =
            relayRuntime.GetSelectedPeerMessages().size();
        constexpr std::string_view kFirstLine = "line one";
        if (!runtime.setTextById(kComposerInitialParagraphId, kFirstLine) ||
            !runtime.collapseSelection(kComposerInitialParagraphId,
                                       kFirstLine.size())) {
            return 38;
        }

        skui::Event shiftEnter;
        shiftEnter.type = skui::EventType::KeyDown;
        shiftEnter.key = VK_RETURN;
        shiftEnter.shiftKey = true;
        (void)runtime.handleEvent(shiftEnter);
        const std::vector<std::string> lineIds =
            runtime.childElementIdsById(kComposerDocumentId);
        if (lineIds.size() != 2u) {
            return 39;
        }

        skui::Event textInput;
        textInput.type = skui::EventType::TextInput;
        textInput.text = "line two";
        (void)runtime.handleEvent(textInput);
        if (!runtime.setSelectionBaseAndExtent(
                lineIds.front(), 0, lineIds.back(), 8)) {
            return 52;
        }
        const skui::Selection multilineSelection = runtime.selection();
        if (multilineSelection.anchorNodeId != lineIds.front() ||
            multilineSelection.anchorOffset != 0 ||
            multilineSelection.focusNodeId != lineIds.back() ||
            multilineSelection.focusOffset != 8 ||
            !runtime.collapseSelection(lineIds.back(), 8)) {
            return 52;
        }
        const std::optional<skui::ScrollState> twoLineComposerScroll =
            runtime.scrollStateById(kComposerDocumentId);
        if (!twoLineComposerScroll.has_value() ||
            twoLineComposerScroll->viewportHeight < 175.5f ||
            twoLineComposerScroll->viewportHeight > 176.5f ||
            twoLineComposerScroll->maxScrollY > 0.0f) {
            return 46;
        }

        constexpr int kAdditionalComposerLines = 14;
        for (int line = 0; line < kAdditionalComposerLines; ++line) {
            (void)runtime.handleEvent(shiftEnter);
            textInput.text = "scroll line";
            (void)runtime.handleEvent(textInput);
        }
        const std::optional<skui::ScrollState> composerScroll =
            runtime.scrollStateById(kComposerDocumentId);
        if (!composerScroll.has_value() ||
            composerScroll->maxScrollY <= 0.0f) {
            return 45;
        }

        skui::Event enter = shiftEnter;
        enter.shiftKey = false;
        (void)runtime.handleEvent(enter);

        const auto& messages = relayRuntime.GetSelectedPeerMessages();
        if (messages.size() != messageCountBefore + 1u) {
            return 40;
        }
        const auto& parts = messages.back().GetParts();
        const std::string sentText = parts.empty()
            ? std::string{}
            : parts[0].GetText().value_or("");
        if (parts.size() != 1u ||
            !sentText.starts_with("line one\nline two\nscroll line") ||
            composerDocumentHasContent(runtime)) {
            return 41;
        }
        const std::optional<skui::ScrollState> emptyComposerScroll =
            runtime.scrollStateById(kComposerDocumentId);
        if (!emptyComposerScroll.has_value() ||
            emptyComposerScroll->viewportHeight < 175.5f ||
            emptyComposerScroll->viewportHeight > 176.5f) {
            return 47;
        }
        if (makeChatContentMarkup(relayRuntime).find(
                R"(value="line one&#10;line two&#10;scroll line)") ==
            std::string::npos) {
            return 42;
        }
        (void)runtime.collapseSelection(kComposerInitialParagraphId, 0u);
        (void)runtime.handleEvent(enter);
        if (relayRuntime.GetSelectedPeerMessages().size() !=
                messageCountBefore + 1u ||
            runtime.childElementIdsById(kComposerDocumentId).size() != 1u) {
            return 43;
        }
        const std::optional<skui::ScrollState> chatScroll =
            runtime.scrollStateById("chat-scroll");
        if (chatScroll.has_value()) {
            (void)runtime.setScrollOffsetById(
                "chat-scroll", chatScroll->scrollX, chatScroll->maxScrollY);
        }
    }
    if (initialWidth != options.width || initialHeight != options.height) {
        runtime.resize(options.width, options.height, options.dpiScale);
        applyRelayDeskDevicePanel(runtime,
                                  relayRuntime,
                                  binding,
                                  ChatScrollUpdateMode::PreserveOffset);
    }
    if (options.testFileCard) {
        const std::size_t interactionRowBytes =
            static_cast<std::size_t>(options.width) * sizeof(std::uint32_t);
        std::vector<std::uint32_t> interactionPixels(
            static_cast<std::size_t>(options.width) *
            static_cast<std::size_t>(options.height));
        if (!runtime.renderToBgraPixels(interactionPixels.data(),
                                        options.width,
                                        options.height,
                                        interactionRowBytes,
                                        options.dpiScale)) {
            return 29;
        }
        const std::vector<CapturePixelBounds> cardBounds =
            findTransferCardPixelBounds(
                interactionPixels, options.width, options.height);
        if (cardBounds.size() < 2u) {
            return 29;
        }
        const CapturePixelBounds& targetCard =
            cardBounds[cardBounds.size() - 2u];
        skui::Event mouseDown;
        mouseDown.type = skui::EventType::MouseDown;
        mouseDown.x = static_cast<float>(
            (targetCard.left + targetCard.right) / 2);
        mouseDown.y = static_cast<float>(
            (targetCard.top + targetCard.bottom) / 2);
        mouseDown.button = skui::MouseButton::Right;
        (void)runtime.handleEvent(mouseDown);
        skui::Event mouseUp = mouseDown;
        mouseUp.type = skui::EventType::MouseUp;
        (void)runtime.handleEvent(mouseUp);
        if (!gMessageContextMenuVisible || gFileContextPath.empty() ||
            !gFileContextPath.is_absolute() ||
            gFileContextPath.filename() != L"relaydesk_skiaui.exe") {
            return 29;
        }
        hideMessageContextMenu(runtime);
    }
    if (options.testImageMessage) {
        if (html.find(R"(data-action="save-image-context")") ==
            std::string::npos) {
            return 15;
        }
        const std::size_t interactionRowBytes =
            static_cast<std::size_t>(options.width) * sizeof(std::uint32_t);
        std::vector<std::uint32_t> interactionPixels(
            static_cast<std::size_t>(options.width) *
            static_cast<std::size_t>(options.height));
        std::optional<CapturePixelBounds> imageBounds;
        constexpr int kMaximumImageInteractionRenderAttempts = 50;
        for (int attempt = 0;
             attempt < kMaximumImageInteractionRenderAttempts &&
                 !imageBounds.has_value();
             ++attempt) {
            if (!runtime.renderToBgraPixels(interactionPixels.data(),
                                            options.width,
                                            options.height,
                                            interactionRowBytes,
                                            options.dpiScale)) {
                return 12;
            }
            imageBounds = findCaptureImagePixelBounds(
                interactionPixels, options.width, options.height);
            if (!imageBounds.has_value()) {
                Sleep(10);
            }
        }
        if (!imageBounds.has_value()) {
            return 12;
        }
        const float imageX = static_cast<float>(
            (imageBounds->left + imageBounds->right) / 2);
        const float imageY = static_cast<float>(
            (imageBounds->top + imageBounds->bottom) / 2);
        skui::Event mouseDown;
        mouseDown.type = skui::EventType::MouseDown;
        mouseDown.x = imageX;
        mouseDown.y = imageY;
        mouseDown.button = skui::MouseButton::Right;
        (void)runtime.handleEvent(mouseDown);
        skui::Event mouseUp = mouseDown;
        mouseUp.type = skui::EventType::MouseUp;
        (void)runtime.handleEvent(mouseUp);
        if (!gMessageContextMenuVisible || gImageContextPath.empty()) {
            return 12;
        }
        if (gImageContextPath.native().find(L'/') != std::wstring::npos) {
            return 14;
        }

        const std::filesystem::path savedImagePath =
            outputPath.parent_path() / "relaydesk_skiaui_saved_image.png";
        std::error_code error;
        std::filesystem::remove(savedImagePath, error);
        const bool copied = copyFileToPath(
            gImageContextPath, savedImagePath);
        error.clear();
        const std::uintmax_t sourceSize =
            std::filesystem::file_size(gImageContextPath, error);
        const bool sourceSizeValid = !error;
        error.clear();
        const std::uintmax_t savedSize =
            std::filesystem::file_size(savedImagePath, error);
        const bool savedSizeValid = !error;
        error.clear();
        std::filesystem::remove(savedImagePath, error);
        if (!copied || !sourceSizeValid || !savedSizeValid ||
            sourceSize != savedSize) {
            return 16;
        }
        if (!copyFileToPath(gImageContextPath, gImageContextPath)) {
            return 17;
        }
        if (copyFileToPath(gImageContextPath,
                           gImageContextPath / "invalid-target.png")) {
            return 18;
        }
        hideMessageContextMenu(runtime);

        mouseDown.button = skui::MouseButton::Left;
        (void)runtime.handleEvent(mouseDown);
        mouseUp = mouseDown;
        mouseUp.type = skui::EventType::MouseUp;
        (void)runtime.handleEvent(mouseUp);
        if (!gImagePreviewVisible) {
            return 11;
        }
        hideImagePreview(runtime);

        const std::size_t selectionPixelIndex =
            static_cast<std::size_t>(imageY) *
                static_cast<std::size_t>(options.width) +
            static_cast<std::size_t>(imageX);
        const std::uint32_t imagePixelBeforeSelection =
            interactionPixels[selectionPixelIndex];
        skui::Event selectionMouseDown;
        selectionMouseDown.type = skui::EventType::MouseDown;
        selectionMouseDown.x = static_cast<float>(imageBounds->left + 10);
        selectionMouseDown.y = static_cast<float>(imageBounds->top - 25);
        selectionMouseDown.button = skui::MouseButton::Left;
        (void)runtime.handleEvent(selectionMouseDown);
        skui::Event selectionMouseMove = selectionMouseDown;
        selectionMouseMove.type = skui::EventType::MouseMove;
        selectionMouseMove.x = static_cast<float>(imageBounds->right + 50);
        selectionMouseMove.y = static_cast<float>(imageBounds->bottom + 45);
        (void)runtime.handleEvent(selectionMouseMove);
        skui::Event selectionMouseUp = selectionMouseMove;
        selectionMouseUp.type = skui::EventType::MouseUp;
        (void)runtime.handleEvent(selectionMouseUp);
        if (!runtime.renderToBgraPixels(interactionPixels.data(),
                                        options.width,
                                        options.height,
                                        interactionRowBytes,
                                        options.dpiScale)) {
            return 16;
        }
        if (interactionPixels[selectionPixelIndex] ==
            imagePixelBeforeSelection) {
            return 16;
        }
        skui::Event clearSelectionMouseDown;
        clearSelectionMouseDown.type = skui::EventType::MouseDown;
        clearSelectionMouseDown.x = 800.0f;
        clearSelectionMouseDown.y = 400.0f;
        clearSelectionMouseDown.button = skui::MouseButton::Left;
        (void)runtime.handleEvent(clearSelectionMouseDown);
        skui::Event clearSelectionMouseUp = clearSelectionMouseDown;
        clearSelectionMouseUp.type = skui::EventType::MouseUp;
        (void)runtime.handleEvent(clearSelectionMouseUp);
    }
    if (options.testPeerSwitch) {
        binding.documentLoaded = true;
        binding.chatInitialized = true;
        binding.lastDeviceSignature =
            makeRelayDeskUiSignature(relayRuntime, binding);

        relayRuntime.showFullConversation();
        skui::Event mouseDown;
        mouseDown.type = skui::EventType::MouseDown;
        mouseDown.x = 100.0f * options.dpiScale;
        mouseDown.y = 310.0f * options.dpiScale;
        mouseDown.button = skui::MouseButton::Left;
        (void)runtime.handleEvent(mouseDown);
        skui::Event mouseUp = mouseDown;
        mouseUp.type = skui::EventType::MouseUp;
        (void)runtime.handleEvent(mouseUp);

        const std::optional<skui::ScrollState> switchedScroll =
            runtime.scrollStateById("chat-scroll");
        if (!switchedScroll.has_value() || switchedScroll->maxScrollY <= 0.0f) {
            return 8;
        }
        if (std::abs(switchedScroll->maxScrollY - switchedScroll->scrollY) >
            kChatScrollBottomTolerance) {
            return 63;
        }
        (void)refreshRelayDeskDevicePanelIfChanged(binding, false);
    }
    if (!isChatScrolledToLatest(runtime)) {
        return 7;
    }

    const std::size_t rowBytes =
        static_cast<std::size_t>(options.width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(options.width) *
        static_cast<std::size_t>(options.height));
    const bool requiresImageRender =
        options.testImageMessage || options.testComposerAttachments;
    bool imageRendered = !requiresImageRender;
    constexpr int kMaximumImageRenderAttempts = 50;
    for (int attempt = 0;
         attempt < kMaximumImageRenderAttempts && !imageRendered;
         ++attempt) {
        if (!runtime.renderToBgraPixels(pixels.data(),
                                        options.width,
                                        options.height,
                                        rowBytes,
                                        options.dpiScale)) {
            return 5;
        }
        imageRendered = std::any_of(
            pixels.begin(),
            pixels.end(),
            [](std::uint32_t pixel) {
                const std::uint32_t red = (pixel >> 16u) & 0xFFu;
                const std::uint32_t green = (pixel >> 8u) & 0xFFu;
                const std::uint32_t blue = pixel & 0xFFu;
                return red >= 240u && green <= 20u && blue >= 240u;
            });
        if (!imageRendered) {
            Sleep(10);
        }
    }
    if (!requiresImageRender) {
        if (!runtime.renderToBgraPixels(pixels.data(),
                                        options.width,
                                        options.height,
                                        rowBytes,
                                        options.dpiScale)) {
            return 5;
        }
    } else if (!imageRendered) {
        return 13;
    }

    if (options.testFileCard &&
        !writePngFile(outputPath, pixels, options.width, options.height, rowBytes)) {
        return 6;
    }

    if (options.testFileCard) {
        const std::vector<CapturePixelBounds> cardBounds =
            findTransferCardPixelBounds(
                pixels, options.width, options.height);
        if (cardBounds.size() < 2) {
            return 19;
        }
        const CapturePixelBounds& shortCard = cardBounds[cardBounds.size() - 2];
        const CapturePixelBounds& longCard = cardBounds.back();
        const int shortWidth = shortCard.right - shortCard.left + 1;
        const int longWidth = longCard.right - longCard.left + 1;
        const int shortHeight = shortCard.bottom - shortCard.top + 1;
        const int longHeight = longCard.bottom - longCard.top + 1;
        const bool hasCardExpansionRoom = options.width >= 1400;
        if (hasCardExpansionRoom && shortWidth + 40 >= longWidth) {
            return 20;
        }
        if (shortHeight + 12 >= longHeight) {
            return 21;
        }
        if (shortCard.right >= options.width - 24 ||
            longCard.right >= options.width - 24) {
            return 22;
        }
        constexpr int kMaximumTransferContentRightGap = 40;
        const int shortProgressRight = findRightmostTransferProgressPixel(
            pixels, options.width, shortCard);
        if (shortProgressRight < 0 ||
            shortCard.right - shortProgressRight >
                kMaximumTransferContentRightGap) {
            return 23;
        }
    }

    if (options.testFileCard) {
        return 0;
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

    relaydesk::skiaui::SingleInstanceGuard singleInstance;
    if (singleInstance.alreadyRunning()) {
        relaydesk::skiaui::activateExistingInstance();
        return 0;
    }
    relaydesk::skiaui::syncLaunchAtStartupOnAppStart();

    const auto html =
        std::make_shared<const std::string>(makeEmbeddedDocument());
    if (html->empty()) {
        return 1;
    }
    const auto documentLoaded = std::make_shared<bool>(false);
    auto& relayRuntime = relaydesk::runtime::getRelayDeskRuntime();
    relaydesk::skiaui::BackgroundController backgroundController(instance);
    backgroundController.attachRuntime(relayRuntime);
    auto binding = std::make_shared<SkiaUiRuntimeBinding>();
    binding->relayRuntime = &relayRuntime;
    binding->backgroundController = &backgroundController;
    gRuntimeBinding = binding.get();

    skui::win32::WindowOptions options;
    options.title = L"RelayDesk";
    options.logicalWidth = 1600;
    options.logicalHeight = 900;
    options.useSystemDpiScale = true;
    options.clearColor = colorRefFromSkColor(kDemoClearColor);
    options.runtime.clearColor = kDemoClearColor;
    options.onWindowMessage =
        [&backgroundController](HWND window,
                                UINT message,
                                WPARAM wParam,
                                LPARAM lParam,
                                skui::Runtime&) {
            return backgroundController.handleMainWindowMessage(
                window,
                message,
                wParam,
                lParam);
        };
    options.onRuntimeReady = [binding](skui::Runtime& runtime) {
        binding->skiaRuntime = &runtime;
        installRelayDeskInteractions(runtime, *binding->relayRuntime, *binding);
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
    if (binding->deferredRefreshTimerId != 0) {
        KillTimer(nullptr, binding->deferredRefreshTimerId);
    }
    if (gRuntimeBinding == binding.get()) {
        gRuntimeBinding = nullptr;
    }
    backgroundController.detachRuntime();
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
