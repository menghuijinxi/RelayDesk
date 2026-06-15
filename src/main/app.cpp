#include "eui_neo.h"

#include "core/render/text.h"
#include "core/platform/platform.h"
#include "core/uuid.h"
#include "main/app_runtime.h"
#include "platform/attachment_input.h"
#include "platform/text_encoding.h"
#include "storage/app_paths.h"
#include "storage/sticker_store.h"
#include "storage/ui_preferences.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using eui::Color;

constexpr Color kWindowBackground{0.972f, 0.976f, 0.980f, 1.0f};
constexpr Color kPanelBackground{0.996f, 0.997f, 0.998f, 1.0f};
constexpr Color kBorder{0.830f, 0.850f, 0.870f, 1.0f};
constexpr Color kText{0.080f, 0.095f, 0.115f, 1.0f};
constexpr Color kMutedText{0.360f, 0.390f, 0.430f, 1.0f};
constexpr Color kSubtleText{0.560f, 0.590f, 0.620f, 1.0f};
constexpr Color kTeal{0.000f, 0.590f, 0.590f, 1.0f};
constexpr Color kTealSoft{0.860f, 0.965f, 0.960f, 1.0f};
constexpr Color kAmber{0.890f, 0.560f, 0.000f, 1.0f};
constexpr Color kAmberSoft{1.000f, 0.970f, 0.900f, 1.0f};
constexpr Color kDanger{0.820f, 0.190f, 0.120f, 1.0f};
constexpr Color kDangerSoft{1.000f, 0.925f, 0.900f, 1.0f};
constexpr Color kGreen{0.250f, 0.660f, 0.160f, 1.0f};
constexpr Color kOffline{0.630f, 0.650f, 0.670f, 1.0f};
constexpr Color kAvatarGreen{0.080f, 0.600f, 0.440f, 1.0f};
constexpr unsigned int kRefreshIconCodePoint = 0xE72C;
constexpr float kContentTop = 0.0f;
constexpr float kChatHeaderHeight = 118.0f;
constexpr float kChatTimelineContentHeight = 650.0f;
constexpr float kComposerHeight = 196.0f;
constexpr float kMessageBubblePadding = 12.0f;
constexpr float kFailedDeliveryStateHeight = 20.0f;
constexpr float kFailedDeliveryStateGap = 8.0f;
constexpr std::size_t kMaxRecentEmojiCount = 10;
constexpr std::size_t kMaxPendingAttachmentCount = 8;

struct EmojiEntry {
    const char* glyph;
    const char* name;
};

constexpr std::array<EmojiEntry, 96> kEmojiEntries{{
    {"😀", "grinning"},
    {"😃", "smiley"},
    {"😄", "smile"},
    {"😁", "grin"},
    {"😆", "laughing"},
    {"😅", "sweat_smile"},
    {"😂", "joy"},
    {"🤣", "rofl"},
    {"😊", "blush"},
    {"🙂", "slightly_smiling"},
    {"😉", "wink"},
    {"😌", "relieved"},
    {"😍", "heart_eyes"},
    {"🥰", "smiling_hearts"},
    {"😘", "kissing_heart"},
    {"😋", "yum"},
    {"😛", "stuck_out_tongue"},
    {"😜", "wink_tongue"},
    {"🤪", "zany"},
    {"🤨", "raised_eyebrow"},
    {"🧐", "monocle"},
    {"🤓", "nerd"},
    {"😎", "sunglasses"},
    {"🥳", "party"},
    {"😏", "smirk"},
    {"😒", "unamused"},
    {"😞", "disappointed"},
    {"😔", "pensive"},
    {"😟", "worried"},
    {"😕", "confused"},
    {"🙁", "slightly_frowning"},
    {"☹️", "frowning"},
    {"😣", "persevere"},
    {"😖", "confounded"},
    {"😫", "tired"},
    {"😩", "weary"},
    {"🥺", "pleading"},
    {"😢", "cry"},
    {"😭", "sob"},
    {"😤", "triumph"},
    {"😠", "angry"},
    {"😡", "rage"},
    {"🤬", "symbols_mouth"},
    {"🤯", "mind_blown"},
    {"😳", "flushed"},
    {"🥵", "hot"},
    {"🥶", "cold"},
    {"😱", "scream"},
    {"😨", "fearful"},
    {"😰", "cold_sweat"},
    {"😥", "sad_relieved"},
    {"😓", "sweat"},
    {"🤗", "hug"},
    {"🤔", "thinking"},
    {"🤭", "hand_over_mouth"},
    {"🤫", "shushing"},
    {"🤥", "lying"},
    {"😶", "no_mouth"},
    {"🙄", "rolling_eyes"},
    {"😬", "grimacing"},
    {"😴", "sleeping"},
    {"🤤", "drooling"},
    {"😪", "sleepy"},
    {"😵", "dizzy"},
    {"🤐", "zipper_mouth"},
    {"🤢", "nauseated"},
    {"🤮", "vomiting"},
    {"🤧", "sneezing"},
    {"😷", "mask"},
    {"🤒", "thermometer"},
    {"🤕", "head_bandage"},
    {"👍", "thumbs_up"},
    {"🤦", "facepalm"},
    {"🤷", "shrug"},
    {"🙈", "see_no_evil"},
    {"🙉", "hear_no_evil"},
    {"🙊", "speak_no_evil"},
    {"🤡", "clown"},
    {"🥴", "woozy"},
    {"🫠", "melting"},
    {"😮‍💨", "face_exhaling"},
    {"🫥", "dotted_line_face"},
    {"🥲", "smiling_tear"},
    {"🙌", "raised_hands"},
    {"👏", "clap"},
    {"🤝", "handshake"},
    {"✌️", "victory"},
    {"🤞", "crossed_fingers"},
    {"👊", "fist"},
    {"💪", "muscle"},
    {"🫶", "heart_hands"},
    {"🫰", "finger_heart"},
    {"💯", "hundred"},
    {"🔥", "fire"},
    {"💥", "boom"},
    {"🍉", "melon"},
}};

struct PeerPreview {
    std::string deviceId;
    std::string name;
    std::string address;
    bool online;
    bool selected;
};

struct TransferPreview {
    const char* fileName;
    const char* direction;
    const char* detail;
    float progress;
    Color accent;
};

struct StickerPickerItem {
    std::string packId;
    std::string itemId;
    std::string displayName;
    std::string relativePath;
    std::string absolutePath;
};

enum class PendingAttachmentKind {
    Image,
    File,
};

struct PendingAttachmentItem {
    PendingAttachmentKind kind = PendingAttachmentKind::File;
    std::string displayName;
    std::string localPath;
    std::string previewPath;
    std::filesystem::path sourcePath;
    std::uintmax_t fileSize = 0;
    unsigned int imagePixelWidth = 0;
    unsigned int imagePixelHeight = 0;
    bool stageOnSend = false;
};

enum class ComposerDraftItemType {
    Text,
    Attachment,
};

struct ComposerDraftItem {
    ComposerDraftItemType type = ComposerDraftItemType::Text;
    std::string text;
    PendingAttachmentItem attachment;
};

void drawFileDocumentCard(eui::Ui& ui,
                          const std::string& id,
                          float x,
                          float y,
                          float width,
                          const std::string& title,
                          const std::string& detail,
                          bool folder,
                          bool compact);
void drawCompactImageDocumentCard(eui::Ui& ui,
                                  const std::string& id,
                                  float x,
                                  float y,
                                  float width,
                                  const PendingAttachmentItem& attachment);

struct AppLayout {
    float width;
    float height;
    float contentHeight;
    float peerX;
    float peerWidth;
    float chatX;
    float chatWidth;
    float detailX;
    float detailWidth;
    bool showPeers;
    bool showDetails;
};

AppLayout makeLayout(const eui::Screen& screen)
{
    AppLayout layout{};
    layout.width = std::max(screen.width, 640.0f);
    layout.height = std::max(screen.height, 520.0f);
    layout.contentHeight = std::max(1.0f, layout.height - kContentTop);
    layout.showPeers = layout.width >= 720.0f;
    layout.showDetails = layout.width >= 980.0f;
    layout.peerX = 0.0f;
    layout.peerWidth = layout.showPeers
        ? std::clamp(layout.width * 0.28f, 310.0f, 420.0f)
        : 0.0f;
    layout.detailWidth = layout.showDetails
        ? std::clamp(layout.width * 0.24f, 270.0f, 360.0f)
        : 0.0f;
    layout.chatX = layout.peerWidth + 1.0f;
    layout.detailX = layout.width - layout.detailWidth;
    layout.chatWidth = layout.detailX - layout.chatX;

    if (layout.showDetails && layout.chatWidth < 400.0f) {
        layout.showDetails = false;
        layout.detailWidth = 0.0f;
        layout.detailX = layout.width;
        layout.chatWidth = layout.detailX - layout.chatX;
    }

    if (layout.showPeers && layout.chatWidth < 360.0f) {
        layout.showPeers = false;
        layout.peerWidth = 0.0f;
        layout.chatX = 0.0f;
        layout.chatWidth = layout.detailX - layout.chatX;
    }

    return layout;
}

void ensureAppStorage()
{
    static bool initialized = false;
    if (initialized) {
        return;
    }

    try {
        const auto paths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(paths);
    } catch (const std::exception&) {
        // UI 仍可启动，后续存储层接入日志后再把启动失败原因展示给用户。
    }
    initialized = true;
}

void rect(eui::Ui& ui,
          const std::string& id,
          float x,
          float y,
          float width,
          float height,
          Color color,
          float radius = 0.0f,
          Color border = {0.0f, 0.0f, 0.0f, 0.0f})
{
    auto builder = ui.rect(id)
        .position(x, y)
        .size(width, height)
        .color(color)
        .radius(radius);

    if (border.a > 0.0f) {
        builder.border(1.0f, border);
    }

    builder.build();
}

void text(eui::Ui& ui,
          const std::string& id,
          float x,
          float y,
          float width,
          float height,
          const std::string& value,
          float fontSize,
          Color color = kText,
          eui::HorizontalAlign align = eui::HorizontalAlign::Left)
{
    ui.text(id)
        .position(x, y)
        .size(width, height)
        .text(value)
        .fontSize(fontSize)
        .lineHeight(height)
        .color(color)
        .horizontalAlign(align)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

void paragraphText(eui::Ui& ui,
                   const std::string& id,
                   float x,
                   float y,
                   float width,
                   float height,
                   const std::string& value,
                   float fontSize,
                   float lineHeight,
                   Color color = kText,
                   eui::HorizontalAlign align = eui::HorizontalAlign::Left)
{
    ui.text(id)
        .position(x, y)
        .size(width, height)
        .text(value)
        .fontSize(fontSize)
        .lineHeight(lineHeight)
        .color(color)
        .horizontalAlign(align)
        .verticalAlign(eui::VerticalAlign::Top)
        .build();
}

void icon(eui::Ui& ui,
          const std::string& id,
          float x,
          float y,
          float size,
          unsigned int codepoint,
          Color color = kText)
{
    ui.text(id)
        .position(x, y)
        .size(size, size)
        .icon(codepoint)
        .fontSize(size * 0.62f)
        .lineHeight(size)
        .color(color)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

std::size_t firstUtf8CodepointLength(unsigned char leadByte)
{
    if ((leadByte & 0x80u) == 0u) {
        return 1u;
    }
    if ((leadByte & 0xE0u) == 0xC0u) {
        return 2u;
    }
    if ((leadByte & 0xF0u) == 0xE0u) {
        return 3u;
    }
    if ((leadByte & 0xF8u) == 0xF0u) {
        return 4u;
    }
    return 1u;
}

unsigned int utf8CodepointValue(const std::string& value)
{
    if (value.empty()) {
        return 0u;
    }

    std::size_t index = 0;
    const unsigned char first = static_cast<unsigned char>(value[index++]);
    if ((first & 0x80u) == 0u) {
        return first;
    }
    if ((first & 0xE0u) == 0xC0u && index < value.size()) {
        return ((first & 0x1Fu) << 6u)
            | (static_cast<unsigned char>(value[index]) & 0x3Fu);
    }
    if ((first & 0xF0u) == 0xE0u && index + 1u < value.size()) {
        unsigned int codepoint = (first & 0x0Fu) << 12u;
        codepoint |= (static_cast<unsigned char>(value[index++]) & 0x3Fu) << 6u;
        codepoint |= static_cast<unsigned char>(value[index]) & 0x3Fu;
        return codepoint;
    }
    if ((first & 0xF8u) == 0xF0u && index + 2u < value.size()) {
        unsigned int codepoint = (first & 0x07u) << 18u;
        codepoint |= (static_cast<unsigned char>(value[index++]) & 0x3Fu) << 12u;
        codepoint |= (static_cast<unsigned char>(value[index++]) & 0x3Fu) << 6u;
        codepoint |= static_cast<unsigned char>(value[index]) & 0x3Fu;
        return codepoint;
    }

    return 0u;
}

bool isEmojiCodepointText(const std::string& value)
{
    const unsigned int codepoint = utf8CodepointValue(value);
    return (codepoint >= 0x1F000u && codepoint <= 0x1FAFFu)
        || (codepoint >= 0x2600u && codepoint <= 0x27BFu)
        || (codepoint >= 0xFE00u && codepoint <= 0xFE0Fu)
        || codepoint == 0x200Du
        || codepoint == 0x20E3u;
}

std::string makeAvatarText(const std::string& displayName)
{
    const std::size_t first = displayName.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "?";
    }

    const auto leadByte = static_cast<unsigned char>(displayName[first]);
    const std::size_t length = firstUtf8CodepointLength(leadByte);
    if (first + length > displayName.size()) {
        return displayName.substr(first, 1u);
    }

    return displayName.substr(first, length);
}

void avatar(eui::Ui& ui,
            const std::string& id,
            float x,
            float y,
            float size,
            const std::string& displayName)
{
    rect(ui, id + ".bg", x, y, size, size, kAvatarGreen, size * 0.5f);
    text(ui, id + ".text", x, y, size, size, makeAvatarText(displayName),
         std::clamp(size * 0.44f, 14.0f, 32.0f),
         {1.0f, 1.0f, 1.0f, 1.0f}, eui::HorizontalAlign::Center);
}

void statusDot(eui::Ui& ui,
               const std::string& id,
               float x,
               float y,
               Color color,
               float size = 9.0f)
{
    rect(ui, id, x, y, size, size, color, size * 0.5f);
}

components::ProgressStyle progressStyle(Color track, Color fill)
{
    components::ProgressStyle style;
    style.track = track;
    style.fill = fill;
    return style;
}

components::ScrollStyle scrollStyle()
{
    components::ScrollStyle style;
    style.track = {0.920f, 0.930f, 0.940f, 1.0f};
    style.thumb = {0.650f, 0.690f, 0.720f, 1.0f};
    style.thumbHover = {0.520f, 0.570f, 0.610f, 1.0f};
    style.thumbPressed = kTeal;
    return style;
}

components::InputStyle composerInputStyle()
{
    components::InputStyle style;
    style.background = kPanelBackground;
    style.hover = kPanelBackground;
    style.focused = kPanelBackground;
    style.pressed = kPanelBackground;
    style.border = {0.0f, 0.0f, 0.0f, 0.0f};
    style.focusBorder = {0.0f, 0.0f, 0.0f, 0.0f};
    style.text = kText;
    style.placeholder = kSubtleText;
    style.cursor = kTeal;
    style.shadow = {};
    style.radius = 0.0f;
    return style;
}

components::ContextMenuStyle stickerContextMenuStyle()
{
    components::ContextMenuStyle style;
    style.background = kPanelBackground;
    style.hover = kTealSoft;
    style.pressed = {0.790f, 0.940f, 0.930f, 1.0f};
    style.text = kText;
    style.mutedText = kMutedText;
    style.border = kBorder;
    style.shadow = {true, {0.0f, 5.0f}, 12.0f, 0.0f,
                    {0.0f, 0.0f, 0.0f, 0.14f}, false};
    style.radius = 8.0f;
    return style;
}

bool hasComposerText(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n") != std::string::npos;
}

std::string emojiPreviewText(const std::string& emoji)
{
    if (emoji == "thumbs_up") {
        return "👍";
    }
    if (emoji == "smile") {
        return "🙂";
    }
    return emoji;
}

void rememberRecentEmoji(std::vector<std::string>& recentEmojis,
                         const std::string& emoji)
{
    recentEmojis.erase(std::remove(recentEmojis.begin(), recentEmojis.end(), emoji),
                       recentEmojis.end());
    recentEmojis.insert(recentEmojis.begin(), emoji);
    if (recentEmojis.size() > kMaxRecentEmojiCount) {
        recentEmojis.resize(kMaxRecentEmojiCount);
    }
}

std::vector<std::string> normalizeRecentEmojis(
    const std::vector<std::string>& recentEmojis)
{
    std::vector<std::string> result;
    result.reserve(std::min(recentEmojis.size(), kMaxRecentEmojiCount));
    for (const auto& emoji : recentEmojis) {
        if (emoji.empty()
            || std::find(result.begin(), result.end(), emoji) != result.end()) {
            continue;
        }

        result.push_back(emoji);
        if (result.size() >= kMaxRecentEmojiCount) {
            break;
        }
    }
    return result;
}

std::vector<std::string> loadStoredRecentEmojis()
{
    try {
        const auto paths = relaydesk::storage::createAppPaths();
        return normalizeRecentEmojis(relaydesk::storage::loadRecentEmojis(paths));
    } catch (const std::exception&) {
        return {};
    }
}

void saveStoredRecentEmojis(const std::vector<std::string>& recentEmojis)
{
    try {
        const auto paths = relaydesk::storage::createAppPaths();
        relaydesk::storage::saveRecentEmojis(paths,
                                             normalizeRecentEmojis(recentEmojis));
    } catch (const std::exception&) {
    }
}

std::string filesystemPathToUtf8String(const std::filesystem::path& filePath)
{
    const auto value = filePath.u8string();
    return std::string(value.begin(), value.end());
}

std::string filesystemPathToGenericUtf8String(const std::filesystem::path& filePath)
{
    const auto value = filePath.generic_u8string();
    return std::string(value.begin(), value.end());
}

std::filesystem::path filesystemPathFromUtf8String(const std::string& pathText)
{
#if defined(_WIN32)
    return std::filesystem::path(relaydesk::platform::utf8ToWide(pathText));
#else
    return std::filesystem::path(pathText);
#endif
}

std::filesystem::path resolveWorkRelativePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& relativePath)
{
    std::filesystem::path filePath(relativePath);
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
    std::filesystem::path relativePath =
        std::filesystem::relative(filePath, appPaths.GetWorkDirectory(), error);
    if (!error && !relativePath.empty() && !isParentTraversalPath(relativePath)) {
        return filesystemPathToGenericUtf8String(relativePath);
    }

    return filesystemPathToUtf8String(filePath);
}

std::string lowerAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

bool isImageAttachmentPath(const std::filesystem::path& filePath)
{
    const std::string extension = lowerAscii(filePath.extension().string());
    return extension == ".png"
        || extension == ".jpg"
        || extension == ".jpeg"
        || extension == ".gif"
        || extension == ".webp"
        || extension == ".bmp";
}

std::string upperAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
    return value;
}

std::string formatFileSize(std::uintmax_t fileSize)
{
    constexpr std::array<const char*, 5> units{"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(fileSize);
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

std::string fileTypeTag(const std::string& fileName)
{
    const std::filesystem::path filePath(fileName);
    std::string extension = filePath.extension().string();
    if (extension.empty()) {
        return "FILE";
    }

    if (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }
    extension = upperAscii(extension);
    if (extension == "JPEG") {
        return "JPG";
    }
    if (extension.size() > 5u) {
        extension.resize(5u);
    }
    return extension.empty() ? "FILE" : extension;
}

std::size_t utf8CodepointCount(const std::string& value)
{
    std::size_t count = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto leadByte = static_cast<unsigned char>(value[index]);
        const std::size_t length = firstUtf8CodepointLength(leadByte);
        index += std::max<std::size_t>(1u, length);
        ++count;
    }
    return count;
}

std::size_t estimateWrappedLineCount(const std::string& value,
                                     float width,
                                     float fontSize)
{
    const float characterWidth = std::max(1.0f, fontSize * 0.92f);
    const auto charactersPerLine =
        static_cast<std::size_t>(std::max(1.0f, width / characterWidth));
    std::size_t result = 0;
    std::size_t lineStart = 0;
    while (lineStart <= value.size()) {
        const std::size_t lineEnd = value.find('\n', lineStart);
        const std::size_t countEnd =
            lineEnd == std::string::npos ? value.size() : lineEnd;
        const std::string line = value.substr(lineStart, countEnd - lineStart);
        const std::size_t codepointCount = utf8CodepointCount(line);
        result += std::max<std::size_t>(
            1u,
            (codepointCount + charactersPerLine - 1u) / charactersPerLine);
        if (lineEnd == std::string::npos) {
            break;
        }
        lineStart = lineEnd + 1u;
    }
    return std::max<std::size_t>(1u, result);
}

float estimateParagraphHeight(const std::string& value,
                              float width,
                              float fontSize,
                              float lineHeight)
{
    const std::size_t lineCount =
        estimateWrappedLineCount(value, width, fontSize);
    return std::max(lineHeight, static_cast<float>(lineCount) * lineHeight);
}

std::filesystem::path makeAbsolutePath(const std::filesystem::path& filePath)
{
    std::error_code error;
    std::filesystem::path absolutePath = std::filesystem::absolute(filePath, error);
    if (error) {
        return filePath.lexically_normal();
    }
    return absolutePath.lexically_normal();
}

std::uintmax_t fileSizeOrZero(const std::filesystem::path& filePath)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(filePath, error);
    return error ? 0u : size;
}

void applyImageSizeMetadata(PendingAttachmentItem& attachment,
                            const std::filesystem::path& imagePath)
{
    const std::optional<relaydesk::platform::ImageSize> imageSize =
        relaydesk::platform::probeImageSize(imagePath);
    if (!imageSize.has_value()) {
        return;
    }

    attachment.imagePixelWidth = imageSize->width;
    attachment.imagePixelHeight = imageSize->height;
}

std::filesystem::path stageAttachmentForSend(
    const relaydesk::storage::AppPaths& appPaths,
    const PendingAttachmentItem& attachment)
{
    const std::filesystem::path fileName = attachment.sourcePath.filename();
    const std::filesystem::path targetDirectory =
        appPaths.GetOutboxDirectory() / relaydesk::core::createUuidV4();
    std::filesystem::create_directories(targetDirectory);
    const std::filesystem::path targetPath = targetDirectory / fileName;
    std::filesystem::copy_file(
        attachment.sourcePath,
        targetPath,
        std::filesystem::copy_options::overwrite_existing);
    return targetPath;
}

struct ComposerImageStage {
    std::filesystem::path sourcePath;
    std::filesystem::path previewPath;
};

ComposerImageStage stageComposerImageFiles(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& sourcePath)
{
    std::string extension = lowerAscii(sourcePath.extension().string());
    if (extension.empty()) {
        extension = ".img";
    }

    const std::filesystem::path targetDirectory =
        appPaths.GetOutboxDirectory() / relaydesk::core::createUuidV4();
    std::filesystem::create_directories(targetDirectory);
    const std::filesystem::path targetPath = targetDirectory / ("source" + extension);
    std::filesystem::copy_file(
        sourcePath,
        targetPath,
        std::filesystem::copy_options::overwrite_existing);
    ComposerImageStage stage;
    stage.sourcePath = targetPath.lexically_normal();
    stage.previewPath = stage.sourcePath;

    const std::filesystem::path thumbnailPath = targetDirectory / "thumbnail.png";
    const std::optional<std::filesystem::path> generatedThumbnail =
        relaydesk::platform::createImageThumbnail(stage.sourcePath,
                                                  thumbnailPath,
                                                  512u);
    if (generatedThumbnail.has_value()) {
        stage.previewPath = generatedThumbnail.value();
    }

    return stage;
}

std::optional<PendingAttachmentItem> makePendingAttachmentFromPath(
    const std::filesystem::path& filePath)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(filePath, error) || error) {
        return std::nullopt;
    }

    const auto appPaths = relaydesk::storage::createAppPaths();
    relaydesk::storage::ensureAppDirectories(appPaths);
    const std::filesystem::path absolutePath = makeAbsolutePath(filePath);
    const bool imageAttachment = isImageAttachmentPath(absolutePath);
    std::filesystem::path sourcePath = absolutePath;
    std::filesystem::path previewPath = absolutePath;
    std::string localPath = makeAttachmentLocalPath(appPaths, absolutePath);
    bool stageOnSend = localPath == filesystemPathToUtf8String(absolutePath);
    if (imageAttachment) {
        try {
            const ComposerImageStage imageStage =
                stageComposerImageFiles(appPaths, absolutePath);
            sourcePath = imageStage.sourcePath;
            previewPath = imageStage.previewPath;
            localPath = makeAttachmentLocalPath(appPaths, imageStage.sourcePath);
            stageOnSend = false;
        } catch (const std::exception&) {
            previewPath = absolutePath;
        }
    }

    PendingAttachmentItem attachment;
    attachment.kind =
        imageAttachment ? PendingAttachmentKind::Image : PendingAttachmentKind::File;
    attachment.displayName = filesystemPathToUtf8String(absolutePath.filename());
    attachment.localPath = localPath;
    attachment.previewPath = filesystemPathToGenericUtf8String(previewPath);
    attachment.sourcePath = sourcePath;
    attachment.fileSize = fileSizeOrZero(absolutePath);
    if (imageAttachment) {
        applyImageSizeMetadata(attachment, sourcePath);
    }
    attachment.stageOnSend = stageOnSend;
    return attachment;
}

PendingAttachmentItem makePendingAttachmentFromSticker(
    const StickerPickerItem& sticker)
{
    const std::filesystem::path imagePath(sticker.absolutePath);
    PendingAttachmentItem attachment;
    attachment.kind = PendingAttachmentKind::Image;
    attachment.displayName =
        sticker.displayName.empty() ? sticker.itemId : sticker.displayName;
    attachment.localPath = sticker.relativePath;
    attachment.previewPath =
        filesystemPathToGenericUtf8String(std::filesystem::path(sticker.absolutePath));
    attachment.sourcePath = imagePath;
    attachment.fileSize = fileSizeOrZero(imagePath);
    applyImageSizeMetadata(attachment, imagePath);
    attachment.stageOnSend = false;
    return attachment;
}

std::size_t composerDraftAttachmentCount(
    const std::vector<ComposerDraftItem>& draftItems)
{
    return static_cast<std::size_t>(
        std::count_if(draftItems.begin(),
                      draftItems.end(),
                      [](const ComposerDraftItem& item) {
            return item.type == ComposerDraftItemType::Attachment;
        }));
}

struct ComposerCaretState {
    std::size_t position = 0;
    float preferredX = 0.0f;
    bool hasPreferredX = false;
};

bool hasComposerDraftContent(const std::vector<ComposerDraftItem>& draftItems)
{
    return std::any_of(draftItems.begin(),
                       draftItems.end(),
                       [](const ComposerDraftItem& item) {
        if (item.type == ComposerDraftItemType::Attachment) {
            return true;
        }
        return hasComposerText(item.text);
    });
}

std::size_t composerDraftItemAtomLength(const ComposerDraftItem& item)
{
    if (item.type == ComposerDraftItemType::Attachment) {
        return 1u;
    }
    return utf8CodepointCount(item.text);
}

std::size_t composerDraftDocumentLength(
    const std::vector<ComposerDraftItem>& draftItems)
{
    std::size_t length = 0;
    for (const ComposerDraftItem& item : draftItems) {
        length += composerDraftItemAtomLength(item);
    }
    return length;
}

std::size_t utf8ByteOffsetForCodepointIndex(const std::string& value,
                                            std::size_t codepointIndex)
{
    std::size_t byteOffset = 0;
    std::size_t codepointOffset = 0;
    while (byteOffset < value.size() && codepointOffset < codepointIndex) {
        const auto leadByte = static_cast<unsigned char>(value[byteOffset]);
        const std::size_t length = std::min(
            firstUtf8CodepointLength(leadByte),
            value.size() - byteOffset);
        byteOffset += std::max<std::size_t>(1u, length);
        ++codepointOffset;
    }
    return byteOffset;
}

std::vector<std::string> splitUtf8Codepoints(const std::string& value)
{
    std::vector<std::string> codepoints;
    for (std::size_t byteOffset = 0; byteOffset < value.size();) {
        const auto leadByte = static_cast<unsigned char>(value[byteOffset]);
        const std::size_t length = std::min(
            firstUtf8CodepointLength(leadByte),
            value.size() - byteOffset);
        const std::size_t safeLength = std::max<std::size_t>(1u, length);
        codepoints.push_back(value.substr(byteOffset, safeLength));
        byteOffset += safeLength;
    }
    return codepoints;
}

ComposerDraftItem makeComposerDraftTextItem(std::string text)
{
    ComposerDraftItem item;
    item.type = ComposerDraftItemType::Text;
    item.text = std::move(text);
    return item;
}

ComposerDraftItem makeComposerDraftAttachmentItem(PendingAttachmentItem attachment)
{
    ComposerDraftItem item;
    item.type = ComposerDraftItemType::Attachment;
    item.attachment = std::move(attachment);
    return item;
}

void normalizeComposerDraftItems(std::vector<ComposerDraftItem>& draftItems)
{
    for (std::size_t index = 0; index < draftItems.size();) {
        if (draftItems[index].type == ComposerDraftItemType::Text
            && draftItems[index].text.empty()) {
            draftItems.erase(draftItems.begin()
                             + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }

    for (std::size_t index = 1; index < draftItems.size();) {
        ComposerDraftItem& previousItem = draftItems[index - 1u];
        ComposerDraftItem& currentItem = draftItems[index];
        if (previousItem.type == ComposerDraftItemType::Text
            && currentItem.type == ComposerDraftItemType::Text) {
            previousItem.text += currentItem.text;
            draftItems.erase(draftItems.begin()
                             + static_cast<std::ptrdiff_t>(index));
            continue;
        }

        ++index;
    }
}

void clampComposerCaret(const std::vector<ComposerDraftItem>& draftItems,
                        ComposerCaretState& caret)
{
    caret.position =
        std::min(caret.position, composerDraftDocumentLength(draftItems));
}

void insertComposerDraftTextItemAt(std::vector<ComposerDraftItem>& draftItems,
                                   std::size_t itemIndex,
                                   std::string text)
{
    if (text.empty()) {
        return;
    }

    itemIndex = std::min(itemIndex, draftItems.size());
    draftItems.insert(
        draftItems.begin() + static_cast<std::ptrdiff_t>(itemIndex),
        makeComposerDraftTextItem(std::move(text)));
}

void insertComposerDraftTextAtCaret(std::vector<ComposerDraftItem>& draftItems,
                                    ComposerCaretState& caret,
                                    std::string text)
{
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (text.empty()) {
        return;
    }

    clampComposerCaret(draftItems, caret);
    const std::size_t insertedLength = utf8CodepointCount(text);
    std::size_t consumedLength = 0;
    for (std::size_t index = 0; index < draftItems.size(); ++index) {
        ComposerDraftItem& item = draftItems[index];
        if (item.type == ComposerDraftItemType::Text) {
            const std::size_t itemLength = utf8CodepointCount(item.text);
            if (caret.position <= consumedLength + itemLength) {
                const std::size_t offset = caret.position - consumedLength;
                const std::size_t byteOffset =
                    utf8ByteOffsetForCodepointIndex(item.text, offset);
                item.text.insert(byteOffset, text);
                caret.position += insertedLength;
                caret.hasPreferredX = false;
                normalizeComposerDraftItems(draftItems);
                return;
            }
            consumedLength += itemLength;
            continue;
        }

        if (caret.position <= consumedLength) {
            insertComposerDraftTextItemAt(draftItems, index, std::move(text));
            caret.position += insertedLength;
            caret.hasPreferredX = false;
            normalizeComposerDraftItems(draftItems);
            return;
        }
        ++consumedLength;
        if (caret.position <= consumedLength) {
            insertComposerDraftTextItemAt(draftItems, index + 1u, std::move(text));
            caret.position += insertedLength;
            caret.hasPreferredX = false;
            normalizeComposerDraftItems(draftItems);
            return;
        }
    }

    draftItems.push_back(makeComposerDraftTextItem(std::move(text)));
    caret.position += insertedLength;
    caret.hasPreferredX = false;
    normalizeComposerDraftItems(draftItems);
}

void insertComposerDraftAttachmentAtCaret(
    std::vector<ComposerDraftItem>& draftItems,
    ComposerCaretState& caret,
    PendingAttachmentItem attachment)
{
    if (composerDraftAttachmentCount(draftItems) >= kMaxPendingAttachmentCount) {
        return;
    }

    clampComposerCaret(draftItems, caret);
    std::size_t consumedLength = 0;
    for (std::size_t index = 0; index < draftItems.size(); ++index) {
        ComposerDraftItem& item = draftItems[index];
        if (item.type == ComposerDraftItemType::Text) {
            const std::size_t itemLength = utf8CodepointCount(item.text);
            if (caret.position <= consumedLength + itemLength) {
                const std::size_t offset = caret.position - consumedLength;
                const std::size_t byteOffset =
                    utf8ByteOffsetForCodepointIndex(item.text, offset);
                std::vector<ComposerDraftItem> replacement;
                if (byteOffset > 0u) {
                    replacement.push_back(
                        makeComposerDraftTextItem(item.text.substr(0u, byteOffset)));
                }
                replacement.push_back(
                    makeComposerDraftAttachmentItem(std::move(attachment)));
                if (byteOffset < item.text.size()) {
                    replacement.push_back(
                        makeComposerDraftTextItem(item.text.substr(byteOffset)));
                }
                draftItems.erase(draftItems.begin()
                                 + static_cast<std::ptrdiff_t>(index));
                draftItems.insert(
                    draftItems.begin() + static_cast<std::ptrdiff_t>(index),
                    replacement.begin(),
                    replacement.end());
                ++caret.position;
                caret.hasPreferredX = false;
                normalizeComposerDraftItems(draftItems);
                return;
            }
            consumedLength += itemLength;
            continue;
        }

        if (caret.position <= consumedLength) {
            draftItems.insert(
                draftItems.begin() + static_cast<std::ptrdiff_t>(index),
                makeComposerDraftAttachmentItem(std::move(attachment)));
            ++caret.position;
            caret.hasPreferredX = false;
            return;
        }
        ++consumedLength;
        if (caret.position <= consumedLength) {
            draftItems.insert(
                draftItems.begin() + static_cast<std::ptrdiff_t>(index + 1u),
                makeComposerDraftAttachmentItem(std::move(attachment)));
            ++caret.position;
            caret.hasPreferredX = false;
            return;
        }
    }

    draftItems.push_back(makeComposerDraftAttachmentItem(std::move(attachment)));
    ++caret.position;
    caret.hasPreferredX = false;
}

void insertComposerDraftAttachmentPathAtCaret(
    std::vector<ComposerDraftItem>& draftItems,
    ComposerCaretState& caret,
    const std::filesystem::path& filePath)
{
    if (composerDraftAttachmentCount(draftItems) >= kMaxPendingAttachmentCount) {
        return;
    }

    try {
        std::optional<PendingAttachmentItem> attachment =
            makePendingAttachmentFromPath(filePath);
        if (attachment.has_value()) {
            insertComposerDraftAttachmentAtCaret(draftItems,
                                                 caret,
                                                 std::move(attachment.value()));
        }
    } catch (const std::exception&) {
    }
}

void insertComposerDraftAttachmentPathsAtCaret(
    std::vector<ComposerDraftItem>& draftItems,
    ComposerCaretState& caret,
    const std::vector<std::filesystem::path>& filePaths)
{
    for (const auto& filePath : filePaths) {
        insertComposerDraftAttachmentPathAtCaret(draftItems, caret, filePath);
    }
}

bool removeComposerDraftAtomAt(std::vector<ComposerDraftItem>& draftItems,
                               std::size_t atomIndex)
{
    std::size_t consumedLength = 0;
    for (std::size_t index = 0; index < draftItems.size(); ++index) {
        ComposerDraftItem& item = draftItems[index];
        if (item.type == ComposerDraftItemType::Text) {
            const std::size_t itemLength = utf8CodepointCount(item.text);
            if (atomIndex < consumedLength + itemLength) {
                const std::size_t offset = atomIndex - consumedLength;
                const std::size_t byteBegin =
                    utf8ByteOffsetForCodepointIndex(item.text, offset);
                const std::size_t byteEnd =
                    utf8ByteOffsetForCodepointIndex(item.text, offset + 1u);
                item.text.erase(byteBegin, byteEnd - byteBegin);
                normalizeComposerDraftItems(draftItems);
                return true;
            }
            consumedLength += itemLength;
            continue;
        }

        if (atomIndex == consumedLength) {
            draftItems.erase(draftItems.begin()
                             + static_cast<std::ptrdiff_t>(index));
            normalizeComposerDraftItems(draftItems);
            return true;
        }
        ++consumedLength;
    }

    return false;
}

void removeComposerDraftAtomBeforeCaret(std::vector<ComposerDraftItem>& draftItems,
                                        ComposerCaretState& caret)
{
    clampComposerCaret(draftItems, caret);
    if (caret.position == 0u) {
        return;
    }

    if (removeComposerDraftAtomAt(draftItems, caret.position - 1u)) {
        --caret.position;
        caret.hasPreferredX = false;
    }
}

void removeComposerDraftAtomAfterCaret(std::vector<ComposerDraftItem>& draftItems,
                                       ComposerCaretState& caret)
{
    clampComposerCaret(draftItems, caret);
    if (removeComposerDraftAtomAt(draftItems, caret.position)) {
        caret.hasPreferredX = false;
    }
}

void removeComposerDraftItemAt(std::vector<ComposerDraftItem>& draftItems,
                               ComposerCaretState& caret,
                               std::size_t index)
{
    if (index >= draftItems.size()) {
        return;
    }

    std::size_t itemStart = 0;
    for (std::size_t itemIndex = 0; itemIndex < index; ++itemIndex) {
        itemStart += composerDraftItemAtomLength(draftItems[itemIndex]);
    }
    const std::size_t itemLength = composerDraftItemAtomLength(draftItems[index]);
    draftItems.erase(draftItems.begin() + static_cast<std::ptrdiff_t>(index));

    if (caret.position > itemStart) {
        caret.position -= std::min(caret.position - itemStart, itemLength);
    }
    caret.hasPreferredX = false;
    normalizeComposerDraftItems(draftItems);
    clampComposerCaret(draftItems, caret);
}

std::vector<std::filesystem::path> selectAttachmentFilesFromDialog()
{
    core::platform::FileDialogOptions options;
    options.prompt = "选择文件";
    options.allowMultiple = true;

    std::vector<std::filesystem::path> filePaths;
    const core::platform::FileDialogResult result =
        core::platform::openFileDialog(options);
    if (result.status != core::platform::FileDialogStatus::Selected) {
        return filePaths;
    }

    filePaths.reserve(result.paths.size());
    for (const auto& pathText : result.paths) {
        filePaths.push_back(filesystemPathFromUtf8String(pathText));
    }
    return filePaths;
}

StickerPickerItem makeStickerPickerItem(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::storage::StickerPack& pack,
    const relaydesk::storage::StickerItem& item)
{
    const std::filesystem::path absolutePath =
        resolveWorkRelativePath(appPaths, item.GetRelativePath());
    return StickerPickerItem{
        pack.GetPackId(),
        item.GetItemId(),
        item.GetDisplayName(),
        item.GetRelativePath(),
        filesystemPathToUtf8String(absolutePath),
    };
}

void appendStickerPickerItems(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::storage::StickerPack& pack,
    std::vector<StickerPickerItem>& items)
{
    for (const auto& item : pack.GetItems()) {
        const std::filesystem::path absolutePath =
            resolveWorkRelativePath(appPaths, item.GetRelativePath());
        if (!std::filesystem::is_regular_file(absolutePath)) {
            continue;
        }

        StickerPickerItem pickerItem = makeStickerPickerItem(appPaths, pack, item);
        items.push_back(std::move(pickerItem));
    }
}

std::vector<StickerPickerItem> loadStoredStickerPickerItems()
{
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        std::vector<StickerPickerItem> items;
        appendStickerPickerItems(
            appPaths,
            relaydesk::storage::loadFavoriteStickerPack(appPaths),
            items);
        for (const auto& pack : relaydesk::storage::loadStickerPacks(appPaths)) {
            appendStickerPickerItems(appPaths, pack, items);
        }
        return items;
    } catch (const std::exception&) {
        return {};
    }
}

void loadStoredStickerPickerItemsOnce(eui::Ui& ui,
                                      std::vector<StickerPickerItem>& items)
{
    bool& loaded = ui.state<bool>("composer.stickers.loaded");
    if (loaded) {
        return;
    }

    items = loadStoredStickerPickerItems();
    loaded = true;
}

std::optional<std::string> importStickerPackFromFileDialog()
{
    core::platform::FileDialogOptions options;
    options.prompt = "导入表情包";
    options.filterName = "表情包";
    options.allowedExtensions = {"json", "png", "jpg", "jpeg", "gif", "webp"};

    const core::platform::FileDialogResult result =
        core::platform::openFileDialog(options);
    if (result.status == core::platform::FileDialogStatus::Cancelled) {
        return std::nullopt;
    }
    if (result.status == core::platform::FileDialogStatus::Failed) {
        return result.error.empty() ? std::string("导入失败") : result.error;
    }
    if (result.paths.empty()) {
        return std::nullopt;
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(appPaths);
        const std::filesystem::path selectedPath(result.paths.front());
        const std::filesystem::path sourceDirectory =
            std::filesystem::is_directory(selectedPath)
                ? selectedPath
                : selectedPath.parent_path();
        if (sourceDirectory.empty()) {
            return std::string("导入来源无效");
        }

        const relaydesk::storage::StickerPack pack =
            relaydesk::storage::importStickerPack(
                appPaths,
                sourceDirectory,
                filesystemPathToUtf8String(sourceDirectory.filename()));
        return "已导入 " + pack.GetDisplayName();
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
}

relaydesk::storage::ChatMessagePart makeComposerTextPart(std::string textValue)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetType(relaydesk::storage::MessagePartType::Text);
    part.SetText(std::move(textValue));
    return part;
}

std::optional<relaydesk::storage::ChatMessagePart> makeComposerAttachmentPart(
    const PendingAttachmentItem& attachment)
{
    std::filesystem::path localPath = attachment.sourcePath;
    std::string localPathText = attachment.localPath;
    std::uintmax_t fileSize = attachment.fileSize;
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(appPaths);
        if (attachment.stageOnSend) {
            localPath = stageAttachmentForSend(appPaths, attachment);
            localPathText = makeAttachmentLocalPath(appPaths, localPath);
        } else if (localPathText.empty()) {
            localPathText = makeAttachmentLocalPath(appPaths, localPath);
        }
        fileSize = fileSizeOrZero(localPath);
    } catch (const std::exception&) {
        return std::nullopt;
    }

    relaydesk::storage::ChatMessagePart part;
    part.SetType(attachment.kind == PendingAttachmentKind::Image
                     ? relaydesk::storage::MessagePartType::Image
                     : relaydesk::storage::MessagePartType::File);
    part.SetTransferId(relaydesk::core::createUuidV4());
    part.SetTransferState(relaydesk::storage::TransferState::Pending);
    part.SetFileName(attachment.displayName);
    part.SetFileSize(fileSize);
    part.SetLocalPath(localPathText);
    return part;
}

std::vector<relaydesk::storage::ChatMessagePart> makeComposerMessageParts(
    const std::vector<ComposerDraftItem>& draftItems)
{
    std::vector<relaydesk::storage::ChatMessagePart> parts;
    for (const auto& item : draftItems) {
        if (item.type == ComposerDraftItemType::Text) {
            if (hasComposerText(item.text)) {
                parts.push_back(makeComposerTextPart(item.text));
            }
            continue;
        }

        std::optional<relaydesk::storage::ChatMessagePart> attachmentPart =
            makeComposerAttachmentPart(item.attachment);
        if (attachmentPart.has_value()) {
            parts.push_back(std::move(attachmentPart.value()));
        }
    }
    return parts;
}

std::optional<std::filesystem::path> resolveRenderableImagePath(
    const relaydesk::storage::ChatMessagePart& part)
{
    if (part.GetType() != relaydesk::storage::MessagePartType::Image
        || !part.GetLocalPath().has_value()
        || part.GetLocalPath().value().empty()) {
        return std::nullopt;
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        const std::filesystem::path imagePath =
            resolveWorkRelativePath(appPaths, part.GetLocalPath().value());
        if (!std::filesystem::is_regular_file(imagePath)) {
            return std::nullopt;
        }

        return imagePath;
    } catch (const std::exception&) {
        return std::nullopt;
    }
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
    case relaydesk::storage::TransferState::Completed:
        return "已完成";
    case relaydesk::storage::TransferState::Failed:
        return "传输失败";
    case relaydesk::storage::TransferState::Cancelled:
        return "已取消";
    }

    return "";
}

std::string messagePartTitle(const relaydesk::storage::ChatMessagePart& part)
{
    if (part.GetFileName().has_value() && !part.GetFileName().value().empty()) {
        return part.GetFileName().value();
    }
    switch (part.GetType()) {
    case relaydesk::storage::MessagePartType::Image:
        return "图片";
    case relaydesk::storage::MessagePartType::File:
        return "文件";
    case relaydesk::storage::MessagePartType::Folder:
        return "文件夹";
    case relaydesk::storage::MessagePartType::Text:
    case relaydesk::storage::MessagePartType::Emoji:
        return "";
    }

    return "";
}

std::string messagePartDetail(const relaydesk::storage::ChatMessagePart& part)
{
    std::string detail;
    if (part.GetType() == relaydesk::storage::MessagePartType::Folder) {
        detail = "文件夹";
    } else if (part.GetFileSize().has_value()) {
        detail = formatFileSize(part.GetFileSize().value());
    }

    if (part.GetTransferState().has_value()) {
        const std::string stateText =
            transferStateText(part.GetTransferState().value());
        if (!stateText.empty()) {
            if (!detail.empty()) {
                detail += " · ";
            }
            detail += stateText;
        }
    }
    return detail;
}

std::optional<std::string> favoriteStickerImage(
    const std::string& localPath,
    const std::string& displayName)
{
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(appPaths);
        std::filesystem::path imagePath(localPath);
        if (imagePath.is_relative()) {
            imagePath = appPaths.GetWorkDirectory() / imagePath;
        }

        relaydesk::storage::addFavoriteStickerFromImage(
            appPaths,
            imagePath,
            displayName);
        return std::nullopt;
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
}

void loadRecentEmojisOnce(eui::Ui& ui, std::vector<std::string>& recentEmojis)
{
    bool& loaded = ui.state<bool>("composer.emoji.recent.loaded");
    if (loaded) {
        return;
    }

    recentEmojis = loadStoredRecentEmojis();
    loaded = true;
}

void drawEmojiCell(eui::Ui& ui,
                   const std::string& id,
                   float x,
                   float y,
                   float size,
                   const std::string& emoji,
                   const std::function<void(const std::string&)>& onSelect)
{
    text(ui,
         id + ".glyph",
         x,
         y + 1.0f,
         size,
         size - 2.0f,
         emoji,
         23.0f,
         kText,
         eui::HorizontalAlign::Center);
    ui.rect(id + ".hit")
        .position(x, y)
        .size(size, size)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([onSelect, emoji] {
            onSelect(emoji);
        })
        .build();
}

void drawEmojiPicker(eui::Ui& ui,
                     float x,
                     float y,
                     float width,
                     float height,
                     float anchorCenterX,
                     const std::vector<std::string>& recentEmojis,
                     const std::function<void(const std::string&)>& onSelect)
{
    constexpr float padding = 18.0f;
    constexpr float cellSize = 34.0f;
    constexpr float gap = 6.0f;
    const int columnCount = std::max(
        6,
        static_cast<int>((width - padding * 2.0f) / (cellSize + gap)));
    rect(ui, "emoji.picker.bg", x, y, width, height, kPanelBackground, 8.0f, kBorder);
    const float pointerX = std::clamp(anchorCenterX - 7.0f,
                                      x + 18.0f,
                                      x + width - 32.0f);
    rect(ui,
         "emoji.picker.anchor",
         pointerX,
         y + height - 1.0f,
         14.0f,
         10.0f,
         kPanelBackground,
         2.0f,
         kBorder);
    ui.rect("emoji.picker.panel.hit")
        .position(x, y)
        .size(width, height + 10.0f)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([] {})
        .build();
    text(ui, "emoji.picker.recent.title", x + padding, y + 16.0f, width - padding * 2.0f,
         22.0f, "最近使用", 13.0f, kMutedText);

    float rowY = y + 45.0f;
    if (recentEmojis.empty()) {
        text(ui,
             "emoji.picker.recent.empty",
             x + padding,
             rowY + 6.0f,
             width - padding * 2.0f,
             22.0f,
             "暂无最近使用",
             12.0f,
             kSubtleText);
    } else {
        const std::size_t recentCount = std::min(
            recentEmojis.size(),
            static_cast<std::size_t>(columnCount));
        for (std::size_t index = 0; index < recentCount; ++index) {
            const float cellX = x + padding + static_cast<float>(index) * (cellSize + gap);
            drawEmojiCell(ui,
                          "emoji.picker.recent." + std::to_string(index),
                          cellX,
                          rowY,
                          cellSize,
                          emojiPreviewText(recentEmojis[index]),
                          onSelect);
        }
    }

    const float allTitleY = rowY + cellSize + 18.0f;
    text(ui, "emoji.picker.all.title", x + padding, allTitleY, width - padding * 2.0f,
         22.0f, "所有表情", 13.0f, kMutedText);

    rowY = allTitleY + 31.0f;
    const float availableHeight = std::max(0.0f, height - (rowY - y) - 14.0f);
    const int maxRows = std::max(1, static_cast<int>(availableHeight / (cellSize + gap)));
    const int maxCells = maxRows * columnCount;
    const std::size_t visibleCount = std::min(
        kEmojiEntries.size(),
        static_cast<std::size_t>(maxCells));
    for (std::size_t index = 0; index < visibleCount; ++index) {
        const int column = static_cast<int>(index) % columnCount;
        const int row = static_cast<int>(index) / columnCount;
        const float cellX = x + padding + static_cast<float>(column) * (cellSize + gap);
        const float cellY = rowY + static_cast<float>(row) * (cellSize + gap);
        drawEmojiCell(ui,
                      "emoji.picker.all." + std::to_string(index),
                      cellX,
                      cellY,
                      cellSize,
                      kEmojiEntries[index].glyph,
                      onSelect);
    }
}

void drawPickerTab(eui::Ui& ui,
                   const std::string& id,
                   float x,
                   float y,
                   float width,
                   const std::string& label,
                   bool active,
                   const std::function<void()>& onClick)
{
    rect(ui,
         id + ".bg",
         x,
         y,
         width,
         30.0f,
         active ? kTealSoft : Color{0.0f, 0.0f, 0.0f, 0.0f},
         6.0f,
         active ? Color{0.640f, 0.880f, 0.870f, 1.0f}
                : Color{0.0f, 0.0f, 0.0f, 0.0f});
    text(ui,
         id + ".label",
         x,
         y + 4.0f,
         width,
         20.0f,
         label,
         13.0f,
         active ? kTeal : kMutedText,
         eui::HorizontalAlign::Center);
    ui.rect(id + ".hit")
        .position(x, y)
        .size(width, 30.0f)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick(onClick)
        .build();
}

void drawStickerCell(eui::Ui& ui,
                     const std::string& id,
                     float x,
                     float y,
                     float size,
                     const StickerPickerItem& sticker,
                     const std::function<void(const StickerPickerItem&)>& onSelect)
{
    rect(ui,
         id + ".bg",
         x,
         y,
         size,
         size,
         {1.0f, 1.0f, 1.0f, 1.0f},
         7.0f,
         kBorder);
    ui.image(id + ".image")
        .position(x + 4.0f, y + 4.0f)
        .size(size - 8.0f, size - 8.0f)
        .path(sticker.absolutePath)
        .contain()
        .radius(5.0f)
        .build();
    ui.rect(id + ".hit")
        .position(x, y)
        .size(size, size)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([onSelect, sticker] {
            onSelect(sticker);
        })
        .build();
}

void drawStickerImportButton(eui::Ui& ui,
                             float x,
                             float y,
                             const std::function<void()>& onImport)
{
    rect(ui, "emoji.picker.stickers.import.bg", x, y, 76.0f, 30.0f,
         kPanelBackground, 6.0f, kBorder);
    icon(ui, "emoji.picker.stickers.import.icon", x + 7.0f, y + 2.0f, 26.0f,
         0xE8B7, kMutedText);
    text(ui, "emoji.picker.stickers.import.text", x + 31.0f, y + 5.0f, 39.0f,
         18.0f, "导入", 12.0f, kMutedText, eui::HorizontalAlign::Center);
    ui.rect("emoji.picker.stickers.import.hit")
        .position(x, y)
        .size(76.0f, 30.0f)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick(onImport)
        .build();
}

void drawUnicodeEmojiPickerContent(
    eui::Ui& ui,
    float x,
    float y,
    float width,
    float height,
    const std::vector<std::string>& recentEmojis,
    const std::function<void(const std::string&)>& onSelect)
{
    constexpr float padding = 18.0f;
    constexpr float cellSize = 34.0f;
    constexpr float gap = 6.0f;
    const int columnCount = std::max(
        6,
        static_cast<int>((width - padding * 2.0f) / (cellSize + gap)));

    text(ui, "emoji.rich.recent.title", x + padding, y, width - padding * 2.0f,
         22.0f, "最近使用", 13.0f, kMutedText);

    float rowY = y + 29.0f;
    if (recentEmojis.empty()) {
        text(ui,
             "emoji.rich.recent.empty",
             x + padding,
             rowY + 6.0f,
             width - padding * 2.0f,
             22.0f,
             "暂无最近使用",
             12.0f,
             kSubtleText);
    } else {
        const std::size_t recentCount = std::min(
            recentEmojis.size(),
            static_cast<std::size_t>(columnCount));
        for (std::size_t index = 0; index < recentCount; ++index) {
            const float cellX = x + padding + static_cast<float>(index)
                * (cellSize + gap);
            drawEmojiCell(ui,
                          "emoji.rich.recent." + std::to_string(index),
                          cellX,
                          rowY,
                          cellSize,
                          emojiPreviewText(recentEmojis[index]),
                          onSelect);
        }
    }

    const float allTitleY = rowY + cellSize + 18.0f;
    text(ui, "emoji.rich.all.title", x + padding, allTitleY,
         width - padding * 2.0f, 22.0f, "所有表情", 13.0f, kMutedText);

    rowY = allTitleY + 31.0f;
    const float availableHeight = std::max(0.0f, height - (rowY - y) - 14.0f);
    const int maxRows = std::max(
        1,
        static_cast<int>(availableHeight / (cellSize + gap)));
    const int maxCells = maxRows * columnCount;
    const std::size_t visibleCount = std::min(
        kEmojiEntries.size(),
        static_cast<std::size_t>(maxCells));
    for (std::size_t index = 0; index < visibleCount; ++index) {
        const int column = static_cast<int>(index) % columnCount;
        const int row = static_cast<int>(index) / columnCount;
        const float cellX = x + padding + static_cast<float>(column)
            * (cellSize + gap);
        const float cellY = rowY + static_cast<float>(row) * (cellSize + gap);
        drawEmojiCell(ui,
                      "emoji.rich.all." + std::to_string(index),
                      cellX,
                      cellY,
                      cellSize,
                      kEmojiEntries[index].glyph,
                      onSelect);
    }
}

void drawStickerPickerContent(
    eui::Ui& ui,
    float x,
    float y,
    float width,
    float height,
    const std::vector<StickerPickerItem>& stickers,
    const std::string& importStatus,
    const std::function<void(const StickerPickerItem&)>& onSelect,
    const std::function<void()>& onImport)
{
    constexpr float padding = 18.0f;
    constexpr float cellSize = 48.0f;
    constexpr float gap = 10.0f;
    const int columnCount = std::max(
        4,
        static_cast<int>((width - padding * 2.0f) / (cellSize + gap)));

    text(ui, "emoji.picker.stickers.title", x + padding, y + 4.0f,
         width - padding * 2.0f - 90.0f, 22.0f, "收藏表情", 13.0f, kMutedText);
    drawStickerImportButton(ui, x + width - padding - 76.0f, y, onImport);

    const float gridY = y + 43.0f;
    if (stickers.empty()) {
        text(ui,
             "emoji.picker.stickers.empty",
             x + padding,
             gridY + 12.0f,
             width - padding * 2.0f,
             22.0f,
             "暂无自定义表情",
             12.0f,
             kSubtleText);
    } else {
        const float availableHeight = std::max(0.0f, height - (gridY - y) - 36.0f);
        const int maxRows = std::max(
            1,
            static_cast<int>(availableHeight / (cellSize + gap)));
        const int maxCells = maxRows * columnCount;
        const std::size_t visibleCount =
            std::min(stickers.size(), static_cast<std::size_t>(maxCells));
        for (std::size_t index = 0; index < visibleCount; ++index) {
            const int column = static_cast<int>(index) % columnCount;
            const int row = static_cast<int>(index) / columnCount;
            const float cellX = x + padding + static_cast<float>(column)
                * (cellSize + gap);
            const float cellY = gridY + static_cast<float>(row) * (cellSize + gap);
            drawStickerCell(ui,
                            "emoji.picker.sticker." + std::to_string(index),
                            cellX,
                            cellY,
                            cellSize,
                            stickers[index],
                            onSelect);
        }
    }

    if (!importStatus.empty()) {
        text(ui, "emoji.picker.stickers.status", x + padding, y + height - 26.0f,
             width - padding * 2.0f, 20.0f, importStatus, 12.0f, kSubtleText);
    }
}

void drawRichEmojiPicker(eui::Ui& ui,
                         float x,
                         float y,
                         float width,
                         float height,
                         float anchorCenterX,
                         int selectedTab,
                         const std::vector<std::string>& recentEmojis,
                         const std::vector<StickerPickerItem>& stickers,
                         const std::string& importStatus,
                         const std::function<void(const std::string&)>& onSelectEmoji,
                         const std::function<void(const StickerPickerItem&)>& onSelectSticker,
                         const std::function<void()>& onImport,
                         const std::function<void(int)>& onSelectTab)
{
    rect(ui, "emoji.rich.bg", x, y, width, height, kPanelBackground, 8.0f, kBorder);
    const float pointerX = std::clamp(anchorCenterX - 7.0f,
                                      x + 18.0f,
                                      x + width - 32.0f);
    rect(ui,
         "emoji.rich.anchor",
         pointerX,
         y + height - 1.0f,
         14.0f,
         10.0f,
         kPanelBackground,
         2.0f,
         kBorder);
    ui.rect("emoji.rich.panel.hit")
        .position(x, y)
        .size(width, height + 10.0f)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([] {})
        .build();

    const float tabY = y + 12.0f;
    drawPickerTab(ui, "emoji.rich.tab.emoji", x + 18.0f, tabY, 68.0f,
                  "表情", selectedTab == 0, [onSelectTab] {
                      onSelectTab(0);
                  });
    drawPickerTab(ui, "emoji.rich.tab.stickers", x + 92.0f, tabY, 68.0f,
                  "收藏", selectedTab == 1, [onSelectTab] {
                      onSelectTab(1);
                  });

    const float contentY = y + 56.0f;
    const float contentHeight = std::max(1.0f, height - 66.0f);
    if (selectedTab == 1) {
        drawStickerPickerContent(ui,
                                 x,
                                 contentY,
                                 width,
                                 contentHeight,
                                 stickers,
                                 importStatus,
                                 onSelectSticker,
                                 onImport);
    } else {
        drawUnicodeEmojiPickerContent(ui,
                                      x,
                                      contentY,
                                      width,
                                      contentHeight,
                                      recentEmojis,
                                      onSelectEmoji);
    }
}

struct ComposerFlowCursor {
    float x;
    float y;
    float lineHeight;
};

float composerDraftFileNodeWidth(float flowWidth)
{
    if (flowWidth < 420.0f) {
        return flowWidth;
    }
    return std::clamp(flowWidth * 0.42f, 220.0f, 340.0f);
}

struct ComposerAttachmentNodeSize {
    float width = 0.0f;
    float height = 0.0f;
};

struct ComposerEditorCaretLocation {
    std::size_t position = 0;
    float x = 0.0f;
    float y = 0.0f;
    float height = 20.0f;
};

struct ComposerEditorTextAtom {
    std::size_t position = 0;
    std::string text;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
};

struct ComposerEditorAttachmentAtom {
    std::size_t position = 0;
    std::size_t itemIndex = 0;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct ComposerEditorLayout {
    std::vector<ComposerEditorTextAtom> textAtoms;
    std::vector<ComposerEditorAttachmentAtom> attachmentAtoms;
    std::vector<ComposerEditorCaretLocation> caretLocations;
    float contentHeight = 0.0f;
};

constexpr float kComposerEditorFontSize = 14.0f;
constexpr float kComposerEditorLineHeight = 22.0f;
constexpr float kComposerEditorScrollbarWidth = 7.0f;
constexpr float kComposerEditorScrollbarGap = 6.0f;
constexpr float kComposerEditorImageMaxWidth = 260.0f;
constexpr float kComposerEditorImageMaxHeight = 104.0f;
constexpr float kComposerEditorImageFallbackWidth = 156.0f;
constexpr float kComposerEditorImageFallbackHeight = 88.0f;

float composerEditorTextAtomWidth(const std::string& value)
{
    if (value.empty() || value == "\n") {
        return 0.0f;
    }

    const float measuredWidth =
        core::TextPrimitive::measureTextWidth(value, "", kComposerEditorFontSize);
    if (std::isfinite(measuredWidth) && measuredWidth > 0.0f) {
        return measuredWidth;
    }
    return std::max(4.0f,
                    static_cast<float>(utf8CodepointCount(value)) * 8.5f);
}

float composerAttachmentAspectRatio(const PendingAttachmentItem& attachment)
{
    if (attachment.imagePixelWidth == 0u || attachment.imagePixelHeight == 0u) {
        return 0.0f;
    }

    return static_cast<float>(attachment.imagePixelWidth)
        / static_cast<float>(attachment.imagePixelHeight);
}

ComposerAttachmentNodeSize composerDraftImageNodeSize(
    const PendingAttachmentItem& attachment,
    float flowWidth)
{
    if (flowWidth <= 0.0f) {
        return {};
    }

    const float minimumSide = std::min(44.0f, flowWidth);
    const float aspectRatio = composerAttachmentAspectRatio(attachment);
    if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0f) {
        return {
            std::min(flowWidth, kComposerEditorImageFallbackWidth),
            kComposerEditorImageFallbackHeight
        };
    }

    const float maxWidth = std::min(flowWidth, kComposerEditorImageMaxWidth);
    float nodeWidth = maxWidth;
    float nodeHeight = nodeWidth / aspectRatio;
    if (nodeHeight > kComposerEditorImageMaxHeight) {
        nodeHeight = kComposerEditorImageMaxHeight;
        nodeWidth = nodeHeight * aspectRatio;
    }

    if (nodeWidth > flowWidth) {
        nodeWidth = flowWidth;
        nodeHeight = nodeWidth / aspectRatio;
    }

    nodeWidth = std::clamp(nodeWidth, minimumSide, flowWidth);
    nodeHeight = std::clamp(nodeHeight, minimumSide, kComposerEditorImageMaxHeight);
    return {nodeWidth, nodeHeight};
}

void setComposerEditorCaretLocation(ComposerEditorLayout& layout,
                                    std::size_t position,
                                    float x,
                                    float y,
                                    float height)
{
    if (position >= layout.caretLocations.size()) {
        return;
    }

    layout.caretLocations[position] = {position, x, y, height};
}

void advanceComposerEditorLine(ComposerFlowCursor& cursor,
                               float lineStartX,
                               float rowGap)
{
    const float lineHeight = std::max(cursor.lineHeight, kComposerEditorLineHeight);
    cursor.x = lineStartX;
    cursor.y += lineHeight + rowGap;
    cursor.lineHeight = kComposerEditorLineHeight;
}

ComposerEditorLayout makeComposerEditorLayout(
    const std::vector<ComposerDraftItem>& draftItems,
    float width)
{
    constexpr float rowGap = 8.0f;
    constexpr float attachmentGap = 8.0f;
    ComposerEditorLayout layout;
    const std::size_t documentLength = composerDraftDocumentLength(draftItems);
    layout.caretLocations.resize(documentLength + 1u);

    ComposerFlowCursor cursor{0.0f, 0.0f, kComposerEditorLineHeight};
    setComposerEditorCaretLocation(layout,
                                   0u,
                                   cursor.x,
                                   cursor.y,
                                   kComposerEditorLineHeight);

    std::size_t documentPosition = 0;
    for (std::size_t itemIndex = 0; itemIndex < draftItems.size(); ++itemIndex) {
        const ComposerDraftItem& item = draftItems[itemIndex];
        if (item.type == ComposerDraftItemType::Text) {
            for (const std::string& codepoint : splitUtf8Codepoints(item.text)) {
                if (codepoint == "\n") {
                    setComposerEditorCaretLocation(layout,
                                                   documentPosition,
                                                   cursor.x,
                                                   cursor.y,
                                                   kComposerEditorLineHeight);
                    ++documentPosition;
                    advanceComposerEditorLine(cursor, 0.0f, rowGap);
                    setComposerEditorCaretLocation(layout,
                                                   documentPosition,
                                                   cursor.x,
                                                   cursor.y,
                                                   kComposerEditorLineHeight);
                    continue;
                }

                const float atomWidth = composerEditorTextAtomWidth(codepoint);
                if (cursor.x > 0.0f && cursor.x + atomWidth > width) {
                    advanceComposerEditorLine(cursor, 0.0f, rowGap);
                }

                setComposerEditorCaretLocation(layout,
                                               documentPosition,
                                               cursor.x,
                                               cursor.y,
                                               kComposerEditorLineHeight);
                layout.textAtoms.push_back(
                    {documentPosition, codepoint, cursor.x, cursor.y, atomWidth});
                cursor.x += atomWidth;
                cursor.lineHeight =
                    std::max(cursor.lineHeight, kComposerEditorLineHeight);
                ++documentPosition;
                setComposerEditorCaretLocation(layout,
                                               documentPosition,
                                               cursor.x,
                                               cursor.y,
                                               kComposerEditorLineHeight);
            }
            continue;
        }

        const PendingAttachmentItem& attachment = item.attachment;
        const ComposerAttachmentNodeSize nodeSize =
            attachment.kind == PendingAttachmentKind::Image
                ? composerDraftImageNodeSize(attachment, width)
                : ComposerAttachmentNodeSize{composerDraftFileNodeWidth(width),
                                             58.0f};
        const float nodeWidth = nodeSize.width;
        const float nodeHeight = nodeSize.height;
        const float leadingGap = cursor.x > 0.0f ? attachmentGap : 0.0f;
        if (cursor.x > 0.0f && cursor.x + leadingGap + nodeWidth > width) {
            advanceComposerEditorLine(cursor, 0.0f, rowGap);
        } else {
            cursor.x += leadingGap;
        }

        setComposerEditorCaretLocation(layout,
                                       documentPosition,
                                       cursor.x,
                                       cursor.y,
                                       kComposerEditorLineHeight);
        layout.attachmentAtoms.push_back(
            {documentPosition, itemIndex, cursor.x, cursor.y, nodeWidth, nodeHeight});
        cursor.x += nodeWidth + attachmentGap;
        cursor.lineHeight = std::max(cursor.lineHeight, nodeHeight);
        ++documentPosition;
        setComposerEditorCaretLocation(layout,
                                       documentPosition,
                                       cursor.x,
                                       cursor.y,
                                       kComposerEditorLineHeight);
    }

    layout.contentHeight =
        cursor.y + std::max(cursor.lineHeight, kComposerEditorLineHeight);
    return layout;
}

std::size_t composerCaretFromPoint(const ComposerEditorLayout& layout,
                                   float x,
                                   float y)
{
    if (layout.caretLocations.empty()) {
        return 0u;
    }

    std::size_t bestPosition = layout.caretLocations.front().position;
    float bestScore = 0.0f;
    bool hasBestScore = false;
    for (const ComposerEditorCaretLocation& location : layout.caretLocations) {
        const float top = location.y;
        const float bottom = location.y + location.height;
        const float verticalDistance = y < top
            ? top - y
            : (y > bottom ? y - bottom : 0.0f);
        const float score = verticalDistance * 100.0f + std::fabs(x - location.x);
        if (!hasBestScore || score < bestScore) {
            bestScore = score;
            bestPosition = location.position;
            hasBestScore = true;
        }
    }
    return bestPosition;
}

std::size_t composerCaretLineEdge(const ComposerEditorLayout& layout,
                                  std::size_t position,
                                  bool endOfLine)
{
    if (layout.caretLocations.empty()) {
        return 0u;
    }

    position = std::min(position, layout.caretLocations.size() - 1u);
    const float y = layout.caretLocations[position].y;
    std::size_t bestPosition = position;
    float bestX = layout.caretLocations[position].x;
    for (const ComposerEditorCaretLocation& location : layout.caretLocations) {
        if (std::fabs(location.y - y) > 0.5f) {
            continue;
        }
        if ((endOfLine && location.x >= bestX)
            || (!endOfLine && location.x <= bestX)) {
            bestX = location.x;
            bestPosition = location.position;
        }
    }
    return bestPosition;
}

void moveComposerCaretVertically(ComposerCaretState& caret,
                                 const ComposerEditorLayout& layout,
                                 int direction)
{
    if (layout.caretLocations.empty()) {
        caret.position = 0u;
        return;
    }

    caret.position = std::min(caret.position, layout.caretLocations.size() - 1u);
    const ComposerEditorCaretLocation& current =
        layout.caretLocations[caret.position];
    if (!caret.hasPreferredX) {
        caret.preferredX = current.x;
        caret.hasPreferredX = true;
    }

    std::optional<float> targetY;
    for (const ComposerEditorCaretLocation& location : layout.caretLocations) {
        if (direction < 0 && location.y < current.y - 0.5f) {
            if (!targetY.has_value() || location.y > targetY.value()) {
                targetY = location.y;
            }
        } else if (direction > 0 && location.y > current.y + 0.5f) {
            if (!targetY.has_value() || location.y < targetY.value()) {
                targetY = location.y;
            }
        }
    }
    if (!targetY.has_value()) {
        return;
    }

    std::size_t bestPosition = caret.position;
    float bestDistance = 0.0f;
    bool hasBestDistance = false;
    for (const ComposerEditorCaretLocation& location : layout.caretLocations) {
        if (std::fabs(location.y - targetY.value()) > 0.5f) {
            continue;
        }
        const float distance = std::fabs(location.x - caret.preferredX);
        if (!hasBestDistance || distance < bestDistance) {
            bestDistance = distance;
            bestPosition = location.position;
            hasBestDistance = true;
        }
    }
    caret.position = bestPosition;
}

void handleComposerEditorKeyboardEvent(
    std::vector<ComposerDraftItem>& draftItems,
    ComposerCaretState& caret,
    const ComposerEditorLayout& layout,
    const core::KeyboardEvent& event)
{
    clampComposerCaret(draftItems, caret);
    const std::size_t documentLength = composerDraftDocumentLength(draftItems);
    if (event.left && caret.position > 0u) {
        --caret.position;
        caret.hasPreferredX = false;
    }
    if (event.right && caret.position < documentLength) {
        ++caret.position;
        caret.hasPreferredX = false;
    }
    if (event.up) {
        moveComposerCaretVertically(caret, layout, -1);
    }
    if (event.down) {
        moveComposerCaretVertically(caret, layout, 1);
    }
    if (event.home) {
        caret.position = composerCaretLineEdge(layout, caret.position, false);
        caret.hasPreferredX = false;
    }
    if (event.end) {
        caret.position = composerCaretLineEdge(layout, caret.position, true);
        caret.hasPreferredX = false;
    }
    if (event.backspace) {
        removeComposerDraftAtomBeforeCaret(draftItems, caret);
    }
    if (event.del) {
        removeComposerDraftAtomAfterCaret(draftItems, caret);
    }
    if (!event.pasteText.empty()) {
        insertComposerDraftTextAtCaret(draftItems, caret, event.pasteText);
    }
    if (!event.text.empty()) {
        insertComposerDraftTextAtCaret(draftItems, caret, event.text);
    }
    if (event.enter) {
        insertComposerDraftTextAtCaret(draftItems, caret, "\n");
    }
}

float composerEditorContentWidth(const std::vector<ComposerDraftItem>& draftItems,
                                 float width,
                                 float height)
{
    const ComposerEditorLayout fullWidthLayout =
        makeComposerEditorLayout(draftItems, width);
    if (fullWidthLayout.contentHeight <= height) {
        return width;
    }

    return std::max(
        0.0f,
        width - kComposerEditorScrollbarWidth - kComposerEditorScrollbarGap);
}

float composerEditorMaxScrollOffset(const ComposerEditorLayout& layout,
                                    float viewportHeight)
{
    return std::max(0.0f, layout.contentHeight - viewportHeight);
}

float composerEditorScrollOffsetForCaret(const ComposerEditorLayout& layout,
                                         std::size_t caretPosition,
                                         float viewportHeight,
                                         float currentOffset)
{
    if (layout.caretLocations.empty()) {
        return 0.0f;
    }

    constexpr float visibleMargin = 10.0f;
    const float maxOffset = composerEditorMaxScrollOffset(layout, viewportHeight);
    const std::size_t caretIndex =
        std::min(caretPosition, layout.caretLocations.size() - 1u);
    const ComposerEditorCaretLocation& location =
        layout.caretLocations[caretIndex];
    float nextOffset = std::clamp(currentOffset, 0.0f, maxOffset);
    if (location.y < nextOffset + visibleMargin) {
        nextOffset = location.y - visibleMargin;
    } else if (location.y + location.height
               > nextOffset + viewportHeight - visibleMargin) {
        nextOffset =
            location.y + location.height + visibleMargin - viewportHeight;
    }

    return std::clamp(nextOffset, 0.0f, maxOffset);
}

void drawComposerEditorCloseButton(eui::Ui& ui,
                                   const std::string& id,
                                   float x,
                                   float y,
                                   std::vector<ComposerDraftItem>& draftItems,
                                   ComposerCaretState& caret,
                                   std::size_t index)
{
    rect(ui, id + ".close.bg", x, y, 18.0f, 18.0f,
         kPanelBackground, 9.0f, kBorder);
    icon(ui, id + ".close.icon", x + 1.5f, y + 1.5f, 15.0f, 0xE711, kMutedText);
    ui.rect(id + ".close.hit")
        .position(x - 3.0f, y - 3.0f)
        .size(24.0f, 24.0f)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&draftItems, &caret, index] {
            removeComposerDraftItemAt(draftItems, caret, index);
        })
        .build();
}

void drawComposerEditorImageNode(eui::Ui& ui,
                                 const std::string& id,
                                 float x,
                                 float y,
                                 float width,
                                 float height,
                                 const PendingAttachmentItem& attachment,
                                 std::vector<ComposerDraftItem>& draftItems,
                                 ComposerCaretState& caret,
                                 std::size_t index)
{
    bool& previewOpen = ui.state<bool>("chat.image.preview.open");
    std::string& previewPath = ui.state<std::string>("chat.image.preview.path");
    std::string& previewName = ui.state<std::string>("chat.image.preview.name");
    const std::string originalPath =
        filesystemPathToGenericUtf8String(attachment.sourcePath);

    rect(ui, id + ".frame", x, y, width, height,
         {1.0f, 1.0f, 1.0f, 0.82f}, 8.0f, kBorder);
    ui.image(id + ".image")
        .position(x + 5.0f, y + 5.0f)
        .size(width - 10.0f, height - 10.0f)
        .path(attachment.previewPath)
        .contain()
        .radius(6.0f)
        .build();
    ui.rect(id + ".preview.hit")
        .position(x, y)
        .size(width, height)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&previewOpen,
                  &previewPath,
                  &previewName,
                  path = originalPath.empty() ? attachment.previewPath : originalPath,
                  name = attachment.displayName] {
            previewPath = path;
            previewName = name;
            previewOpen = true;
        })
        .build();
    drawComposerEditorCloseButton(ui,
                                  id,
                                  x + width - 14.0f,
                                  y + 4.0f,
                                  draftItems,
                                  caret,
                                  index);
}

void drawComposerEditorTextAtoms(eui::Ui& ui,
                                 const std::string& id,
                                 const ComposerEditorLayout& layout)
{
    std::string fragmentText;
    float fragmentX = 0.0f;
    float fragmentY = 0.0f;
    float fragmentWidth = 0.0f;
    std::size_t fragmentIndex = 0;
    auto flushFragment = [&] {
        if (fragmentText.empty()) {
            return;
        }
        paragraphText(ui,
                      id + ".text." + std::to_string(fragmentIndex),
                      fragmentX,
                      fragmentY,
                      fragmentWidth + 6.0f,
                      kComposerEditorLineHeight,
                      fragmentText,
                      kComposerEditorFontSize,
                      kComposerEditorLineHeight);
        fragmentText.clear();
        fragmentWidth = 0.0f;
        ++fragmentIndex;
    };

    for (const ComposerEditorTextAtom& atom : layout.textAtoms) {
        const bool sameLine = !fragmentText.empty()
            && std::fabs(atom.y - fragmentY) < 0.5f
            && std::fabs(atom.x - (fragmentX + fragmentWidth)) < 1.5f;
        if (!sameLine) {
            flushFragment();
            fragmentX = atom.x;
            fragmentY = atom.y;
        }
        fragmentText += atom.text;
        fragmentWidth = (atom.x + atom.width) - fragmentX;
    }
    flushFragment();
}

void drawComposerEditorAttachmentNode(eui::Ui& ui,
                                      const std::string& id,
                                      const ComposerEditorAttachmentAtom& node,
                                      std::vector<ComposerDraftItem>& draftItems,
                                      ComposerCaretState& caret)
{
    if (node.itemIndex >= draftItems.size()) {
        return;
    }

    const PendingAttachmentItem& attachment = draftItems[node.itemIndex].attachment;
    if (attachment.kind == PendingAttachmentKind::Image) {
        drawComposerEditorImageNode(ui,
                                    id,
                                    node.x,
                                    node.y,
                                    node.width,
                                    node.height,
                                    attachment,
                                    draftItems,
                                    caret,
                                    node.itemIndex);
        return;
    }

    drawFileDocumentCard(ui,
                         id,
                         node.x,
                         node.y,
                         node.width,
                         attachment.displayName,
                         formatFileSize(attachment.fileSize),
                         false,
                         false);
    drawComposerEditorCloseButton(ui,
                                  id,
                                  node.x + node.width - 14.0f,
                                  node.y + 4.0f,
                                  draftItems,
                                  caret,
                                  node.itemIndex);
}

void drawComposerEditor(eui::Ui& ui,
                        float x,
                        float y,
                        float width,
                        float height,
                        std::vector<ComposerDraftItem>& draftItems,
                        ComposerCaretState& caret,
                        const std::string& placeholder)
{
    normalizeComposerDraftItems(draftItems);
    clampComposerCaret(draftItems, caret);
    float& scrollOffset = ui.state<float>("composer.editor.scroll.offset");
    std::size_t& lastVisibleCaretPosition =
        ui.state<std::size_t>("composer.editor.scroll.caret");
    float& lastVisibleContentHeight =
        ui.state<float>("composer.editor.scroll.content.height");
    const float contentWidth =
        composerEditorContentWidth(draftItems, width, height);
    const ComposerEditorLayout layout =
        makeComposerEditorLayout(draftItems, contentWidth);
    const std::size_t caretIndex =
        std::min(caret.position, layout.caretLocations.size() - 1u);
    const ComposerEditorCaretLocation caretLocation =
        layout.caretLocations.empty()
            ? ComposerEditorCaretLocation{}
            : layout.caretLocations[caretIndex];
    const bool contentHeightChanged =
        std::fabs(layout.contentHeight - lastVisibleContentHeight) > 0.5f;
    if (caret.position != lastVisibleCaretPosition || contentHeightChanged) {
        scrollOffset = composerEditorScrollOffsetForCaret(
            layout,
            caret.position,
            height,
            scrollOffset);
        lastVisibleCaretPosition = caret.position;
        lastVisibleContentHeight = layout.contentHeight;
    }
    scrollOffset = std::clamp(
        scrollOffset,
        0.0f,
        composerEditorMaxScrollOffset(layout, height));

    ui.stack("composer.editor.pos")
        .position(x, y)
        .size(width, height)
        .content([&] {
            components::scrollView(ui, "composer.editor.scroll")
                .size(width, height)
                .offset(scrollOffset)
                .gap(0.0f)
                .step(42.0f)
                .scrollbarWidth(kComposerEditorScrollbarWidth)
                .scrollbarGap(kComposerEditorScrollbarGap)
                .style(scrollStyle())
                .onChange([&scrollOffset](float value) {
                    scrollOffset = value;
                })
                .content([&](eui::Ui& contentUi, float, float) {
                    const float documentHeight =
                        std::max(height, layout.contentHeight);
                    const bool focused =
                        contentUi.isFocused("composer.editor.hit");
                    contentUi.stack("composer.editor.content")
                        .size(contentWidth, documentHeight)
                        .content([&] {
                            contentUi.rect("composer.editor.hit")
                                .size(contentWidth, documentHeight)
                                .color({0.0f, 0.0f, 0.0f, 0.0f})
                                .focusable()
                                .imeRect(caretLocation.x,
                                         caretLocation.y - scrollOffset,
                                         1.5f,
                                         caretLocation.height)
                                .onPress([&caret,
                                          &scrollOffset,
                                          layout,
                                          contentWidth](
                                              const eui::PointerEvent& event,
                                              const eui::Rect& bounds) {
                                    const float scale = contentWidth > 0.0f
                                        ? bounds.width / contentWidth
                                        : 1.0f;
                                    const float localX = static_cast<float>(
                                        (event.x - bounds.x)
                                        / std::max(0.001f, scale));
                                    const float localY = static_cast<float>(
                                        (event.y - bounds.y)
                                        / std::max(0.001f, scale))
                                        + scrollOffset;
                                    caret.position =
                                        composerCaretFromPoint(layout,
                                                               localX,
                                                               localY);
                                    caret.hasPreferredX = false;
                                })
                                .onTextInput([&draftItems,
                                              &caret,
                                              layout](
                                                  const core::KeyboardEvent& event) {
                                    handleComposerEditorKeyboardEvent(draftItems,
                                                                      caret,
                                                                      layout,
                                                                      event);
                                })
                                .build();

                            if (draftItems.empty()) {
                                paragraphText(
                                    contentUi,
                                    "composer.editor.placeholder",
                                    6.0f,
                                    3.0f,
                                    std::max(0.0f, contentWidth - 12.0f),
                                    kComposerEditorLineHeight,
                                    placeholder,
                                    kComposerEditorFontSize,
                                    kComposerEditorLineHeight,
                                    kSubtleText);
                            } else {
                                drawComposerEditorTextAtoms(contentUi,
                                                            "composer.editor",
                                                            layout);
                                for (const ComposerEditorAttachmentAtom& node
                                     : layout.attachmentAtoms) {
                                    drawComposerEditorAttachmentNode(
                                        contentUi,
                                        "composer.editor.attachment."
                                            + std::to_string(node.position),
                                        node,
                                        draftItems,
                                        caret);
                                }
                            }

                            if (focused) {
                                rect(contentUi,
                                     "composer.editor.cursor",
                                     caretLocation.x,
                                     caretLocation.y + 2.0f,
                                     1.5f,
                                     std::max(16.0f,
                                              caretLocation.height - 4.0f),
                                     kTeal,
                                     1.0f);
                            }
                        })
                        .build();
                })
                .build();
        })
        .build();
}

void drawLocalUserHeader(eui::Ui& ui, float x, float y, float width)
{
    rect(ui, "local.avatar.bg", x + 22.0f, y + 16.0f, 44.0f, 44.0f, kAvatarGreen,
         22.0f);
    text(ui, "local.avatar.text", x + 22.0f, y + 24.0f, 44.0f, 26.0f, "林", 20.0f,
         {1.0f, 1.0f, 1.0f, 1.0f}, eui::HorizontalAlign::Center);
    text(ui, "local.name", x + 78.0f, y + 15.0f, width - 132.0f, 26.0f, "林一凡",
         17.0f);
    text(ui, "local.address", x + 78.0f, y + 41.0f, width - 132.0f, 22.0f,
         "本机 · 192.168.1.8", 13.0f, kMutedText);
    icon(ui, "local.settings", x + width - 50.0f, y + 21.0f, 32.0f, 0xE713, kText);
    rect(ui, "local.bottom.line", x, y + 76.0f, width, 1.0f, kBorder);
}

void peerRow(eui::Ui& ui,
             const PeerPreview& peer,
             int index,
             float x,
             float width,
             float y)
{
    const std::string id = "peer." + std::to_string(index);
    if (peer.selected) {
        rect(ui, id + ".selected", x + 6.0f, y - 8.0f, width - 12.0f, 64.0f,
             kTealSoft, 6.0f, {0.640f, 0.880f, 0.870f, 1.0f});
    }

    statusDot(ui, id + ".state", x + 20.0f, y + 18.0f, peer.online ? kGreen : kOffline);
    avatar(ui, id + ".avatar", x + 42.0f, y + 3.0f, 35.0f, peer.name);
    text(ui, id + ".name", x + 88.0f, y - 2.0f, width - 110.0f, 24.0f, peer.name,
         15.0f);
    text(ui, id + ".ip", x + 88.0f, y + 23.0f, width - 110.0f, 22.0f, peer.address,
         13.0f, kMutedText);
}

void messageBubble(eui::Ui& ui,
                   const std::string& id,
                   float x,
                   float y,
                   float width,
                   const char* value,
                   bool outgoing)
{
    const Color fill = outgoing ? kTealSoft : Color{0.990f, 0.990f, 0.992f, 1.0f};
    rect(ui, id + ".bg", x, y, width, 42.0f, fill, 7.0f, kBorder);
    text(ui, id + ".text", x + 12.0f, y + 9.0f, width - 24.0f, 22.0f, value, 14.0f);
}

void drawFileDocumentCard(eui::Ui& ui,
                          const std::string& id,
                          float x,
                          float y,
                          float width,
                          const std::string& title,
                          const std::string& detail,
                          bool folder,
                          bool compact)
{
    const float height = compact ? 48.0f : 58.0f;
    const float iconSize = compact ? 34.0f : 40.0f;
    const float iconX = x + 9.0f;
    const float iconY = y + (height - iconSize) * 0.5f;
    const Color iconFill = folder
        ? Color{0.890f, 0.950f, 0.990f, 1.0f}
        : Color{0.925f, 0.935f, 0.950f, 1.0f};
    const Color iconColor = folder ? kTeal : kMutedText;
    const unsigned int iconCodepoint = folder ? 0xE8B7 : 0xE7C3;

    rect(ui, id + ".bg", x, y, width, height,
         {0.972f, 0.976f, 0.982f, 1.0f}, 8.0f, kBorder);
    rect(ui, id + ".icon.bg", iconX, iconY, iconSize, iconSize, iconFill, 7.0f);
    icon(ui, id + ".icon", iconX + 2.0f, iconY + 2.0f, iconSize - 4.0f,
         iconCodepoint, iconColor);

    const float textX = iconX + iconSize + 10.0f;
    const float tagWidth = compact ? 42.0f : 50.0f;
    const float titleWidth = std::max(40.0f, width - (textX - x) - tagWidth - 20.0f);
    text(ui, id + ".title", textX, y + (compact ? 5.0f : 7.0f), titleWidth,
         22.0f, title, compact ? 13.0f : 14.0f);
    if (!detail.empty()) {
        text(ui,
             id + ".detail",
             textX,
             y + (compact ? 26.0f : 31.0f),
             titleWidth,
             18.0f,
             detail,
             compact ? 11.0f : 12.0f,
             kMutedText);
    }

    const std::string tag = folder ? "DIR" : fileTypeTag(title);
    rect(ui, id + ".tag.bg", x + width - tagWidth - 10.0f,
         y + (height - 22.0f) * 0.5f, tagWidth, 22.0f,
         {1.0f, 1.0f, 1.0f, 0.82f}, 5.0f, kBorder);
    text(ui,
         id + ".tag",
         x + width - tagWidth - 10.0f,
         y + (height - 20.0f) * 0.5f,
         tagWidth,
         18.0f,
         tag,
         compact ? 10.0f : 11.0f,
         kSubtleText,
         eui::HorizontalAlign::Center);
}

void drawCompactImageDocumentCard(eui::Ui& ui,
                                  const std::string& id,
                                  float x,
                                  float y,
                                  float width,
                                  const PendingAttachmentItem& attachment)
{
    constexpr float height = 48.0f;
    constexpr float thumbSize = 38.0f;
    bool& previewOpen = ui.state<bool>("chat.image.preview.open");
    std::string& previewPath = ui.state<std::string>("chat.image.preview.path");
    std::string& previewName = ui.state<std::string>("chat.image.preview.name");
    rect(ui, id + ".bg", x, y, width, height,
         {0.972f, 0.976f, 0.982f, 1.0f}, 8.0f, kBorder);
    ui.image(id + ".image")
        .position(x + 7.0f, y + 5.0f)
        .size(thumbSize, thumbSize)
        .path(attachment.previewPath)
        .contain()
        .radius(6.0f)
        .build();

    const float textX = x + 54.0f;
    text(ui, id + ".title", textX, y + 5.0f, width - 92.0f, 22.0f,
         attachment.displayName, 13.0f);
    text(ui, id + ".detail", textX, y + 26.0f, width - 92.0f, 18.0f,
         formatFileSize(attachment.fileSize), 11.0f, kMutedText);
    ui.rect(id + ".preview.hit")
        .position(x + 7.0f, y + 5.0f)
        .size(thumbSize, thumbSize)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&previewOpen,
                  &previewPath,
                  &previewName,
                  path = attachment.previewPath,
                  name = attachment.displayName] {
            previewPath = path;
            previewName = name;
            previewOpen = true;
        })
        .build();
}

struct MessageFlowTextAtom {
    std::size_t partIndex = 0;
    std::string text;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float fontSize = kComposerEditorFontSize;
    float lineHeight = kComposerEditorLineHeight;
    bool emoji = false;
};

struct MessageFlowPartNode {
    std::size_t partIndex = 0;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct MessageFlowLayout {
    std::vector<MessageFlowTextAtom> textAtoms;
    std::vector<MessageFlowPartNode> partNodes;
    float contentHeight = kComposerEditorLineHeight;
};

constexpr float kMessageFlowAttachmentGap = 8.0f;
constexpr float kMessageFlowRowGap = 8.0f;
constexpr float kMessageImageMaxHeight = 360.0f;
constexpr float kMessageImageFallbackWidth = 156.0f;
constexpr float kMessageImageFallbackHeight = 88.0f;

float messageTextAtomWidth(const std::string& value, float fontSize)
{
    if (std::fabs(fontSize - kComposerEditorFontSize) < 0.5f) {
        return composerEditorTextAtomWidth(value);
    }

    const float measuredWidth =
        core::TextPrimitive::measureTextWidth(value, "", fontSize);
    if (std::isfinite(measuredWidth) && measuredWidth > 0.0f) {
        return measuredWidth;
    }
    return std::max(4.0f,
                    static_cast<float>(utf8CodepointCount(value)) * fontSize * 0.62f);
}

std::string messageTextPartValue(
    const relaydesk::storage::ChatMessagePart& part)
{
    return part.GetType() == relaydesk::storage::MessagePartType::Emoji
        ? emojiPreviewText(part.GetEmoji().value_or(""))
        : part.GetText().value_or("");
}

float messageTextPartFontSize(
    const relaydesk::storage::ChatMessagePart& part)
{
    return part.GetType() == relaydesk::storage::MessagePartType::Emoji
        ? 24.0f
        : kComposerEditorFontSize;
}

float messageTextPartLineHeight(
    const relaydesk::storage::ChatMessagePart& part)
{
    return part.GetType() == relaydesk::storage::MessagePartType::Emoji
        ? 32.0f
        : kComposerEditorLineHeight;
}

float messageImageAspectRatio(const std::filesystem::path& imagePath)
{
    const std::optional<relaydesk::platform::ImageSize> imageSize =
        relaydesk::platform::probeImageSize(imagePath);
    if (!imageSize.has_value()
        || imageSize->width == 0u
        || imageSize->height == 0u) {
        return 0.0f;
    }

    return static_cast<float>(imageSize->width)
        / static_cast<float>(imageSize->height);
}

float messageImageMaxWidth(float flowWidth)
{
    if (flowWidth < 520.0f) {
        return std::min(flowWidth, 320.0f);
    }

    return std::min(flowWidth,
                    std::max(320.0f,
                             (flowWidth - kMessageFlowAttachmentGap) * 0.5f));
}

ComposerAttachmentNodeSize messageImageNodeSize(
    const relaydesk::storage::ChatMessagePart& part,
    float flowWidth)
{
    if (flowWidth <= 0.0f) {
        return {};
    }

    const float minimumSide = std::min(44.0f, flowWidth);
    const std::optional<std::filesystem::path> imagePath =
        resolveRenderableImagePath(part);
    if (!imagePath.has_value()) {
        return {composerDraftFileNodeWidth(flowWidth), 58.0f};
    }

    const float aspectRatio = messageImageAspectRatio(imagePath.value());
    if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0f) {
        return {
            std::min(flowWidth, kMessageImageFallbackWidth),
            kMessageImageFallbackHeight
        };
    }

    const float maxWidth = messageImageMaxWidth(flowWidth);
    float nodeWidth = maxWidth;
    float nodeHeight = nodeWidth / aspectRatio;
    if (nodeHeight > kMessageImageMaxHeight) {
        nodeHeight = kMessageImageMaxHeight;
        nodeWidth = nodeHeight * aspectRatio;
    }

    if (nodeWidth > flowWidth) {
        nodeWidth = flowWidth;
        nodeHeight = nodeWidth / aspectRatio;
    }

    nodeWidth = std::clamp(nodeWidth, minimumSide, flowWidth);
    nodeHeight = std::clamp(nodeHeight, minimumSide, kMessageImageMaxHeight);
    return {nodeWidth, nodeHeight};
}

ComposerAttachmentNodeSize messagePartNodeSize(
    const relaydesk::storage::ChatMessagePart& part,
    float flowWidth)
{
    switch (part.GetType()) {
    case relaydesk::storage::MessagePartType::Image:
        return messageImageNodeSize(part, flowWidth);
    case relaydesk::storage::MessagePartType::File:
    case relaydesk::storage::MessagePartType::Folder:
        return {composerDraftFileNodeWidth(flowWidth), 58.0f};
    case relaydesk::storage::MessagePartType::Text:
    case relaydesk::storage::MessagePartType::Emoji:
        return {};
    }

    return {};
}

void addMessageTextPartToLayout(
    MessageFlowLayout& layout,
    ComposerFlowCursor& cursor,
    std::size_t partIndex,
    const relaydesk::storage::ChatMessagePart& part,
    float width,
    bool& hasVisiblePart)
{
    const std::string value = messageTextPartValue(part);
    const float fontSize = messageTextPartFontSize(part);
    const float lineHeight = messageTextPartLineHeight(part);
    for (const std::string& codepoint : splitUtf8Codepoints(value)) {
        if (codepoint == "\n") {
            advanceComposerEditorLine(cursor, 0.0f, kMessageFlowRowGap);
            hasVisiblePart = true;
            continue;
        }

        const float atomWidth = messageTextAtomWidth(codepoint, fontSize);
        if (cursor.x > 0.0f && cursor.x + atomWidth > width) {
            advanceComposerEditorLine(cursor, 0.0f, kMessageFlowRowGap);
        }

        const bool emoji = part.GetType() == relaydesk::storage::MessagePartType::Emoji
            || isEmojiCodepointText(codepoint);
        layout.textAtoms.push_back(
            {partIndex,
             codepoint,
             cursor.x,
             cursor.y,
             atomWidth,
             fontSize,
             lineHeight,
             emoji});
        cursor.x += atomWidth;
        cursor.lineHeight = std::max(cursor.lineHeight, lineHeight);
        hasVisiblePart = true;
    }
}

void addMessageNodePartToLayout(
    MessageFlowLayout& layout,
    ComposerFlowCursor& cursor,
    std::size_t partIndex,
    const relaydesk::storage::ChatMessagePart& part,
    float width,
    bool& hasVisiblePart)
{
    const ComposerAttachmentNodeSize nodeSize = messagePartNodeSize(part, width);
    if (nodeSize.width <= 0.0f || nodeSize.height <= 0.0f) {
        return;
    }

    const float leadingGap = cursor.x > 0.0f ? kMessageFlowAttachmentGap : 0.0f;
    if (cursor.x > 0.0f && cursor.x + leadingGap + nodeSize.width > width) {
        advanceComposerEditorLine(cursor, 0.0f, kMessageFlowRowGap);
    } else {
        cursor.x += leadingGap;
    }

    layout.partNodes.push_back(
        {partIndex, cursor.x, cursor.y, nodeSize.width, nodeSize.height});
    cursor.x += nodeSize.width + kMessageFlowAttachmentGap;
    cursor.lineHeight = std::max(cursor.lineHeight, nodeSize.height);
    hasVisiblePart = true;
}

MessageFlowLayout makeMessageFlowLayout(
    const relaydesk::storage::ChatMessageRecord& message,
    float width)
{
    MessageFlowLayout layout;
    ComposerFlowCursor cursor{0.0f, 0.0f, kComposerEditorLineHeight};
    bool hasVisiblePart = false;

    for (std::size_t partIndex = 0; partIndex < message.GetParts().size();
         ++partIndex) {
        const auto& part = message.GetParts()[partIndex];
        switch (part.GetType()) {
        case relaydesk::storage::MessagePartType::Text:
        case relaydesk::storage::MessagePartType::Emoji:
            addMessageTextPartToLayout(layout,
                                       cursor,
                                       partIndex,
                                       part,
                                       width,
                                       hasVisiblePart);
            break;
        case relaydesk::storage::MessagePartType::Image:
        case relaydesk::storage::MessagePartType::File:
        case relaydesk::storage::MessagePartType::Folder:
            addMessageNodePartToLayout(layout,
                                       cursor,
                                       partIndex,
                                       part,
                                       width,
                                       hasVisiblePart);
            break;
        }
    }

    layout.contentHeight = hasVisiblePart
        ? cursor.y + std::max(cursor.lineHeight, kComposerEditorLineHeight)
        : kComposerEditorLineHeight;
    return layout;
}

float messageDocumentContentHeight(
    const relaydesk::storage::ChatMessageRecord& message,
    float width)
{
    return makeMessageFlowLayout(message, width).contentHeight;
}

bool shouldDrawFailedDeliveryStateInsideBubble(
    const relaydesk::storage::ChatMessageRecord& message,
    bool outgoing)
{
    return outgoing
        && message.GetDeliveryState() == relaydesk::storage::DeliveryState::Failed;
}

float messageDeliveryStateFooterHeight(
    const relaydesk::storage::ChatMessageRecord& message,
    bool outgoing)
{
    return shouldDrawFailedDeliveryStateInsideBubble(message, outgoing)
        ? kFailedDeliveryStateGap + kFailedDeliveryStateHeight
        : 0.0f;
}

float messageDocumentBubbleHeight(const relaydesk::storage::ChatMessageRecord& message,
                                  float innerWidth,
                                  bool outgoing)
{
    return messageDocumentContentHeight(message, innerWidth)
        + kMessageBubblePadding * 2.0f
        + messageDeliveryStateFooterHeight(message, outgoing);
}

void drawMessageImagePart(eui::Ui& ui,
                          const std::string& id,
                          float x,
                          float y,
                          float width,
                          float height,
                          const relaydesk::storage::ChatMessagePart& part,
                          bool& stickerMenuOpen,
                          float& stickerMenuX,
                          float& stickerMenuY,
                          std::string& stickerMenuPath,
                          std::string& stickerMenuName)
{
    bool& previewOpen = ui.state<bool>("chat.image.preview.open");
    std::string& previewPath = ui.state<std::string>("chat.image.preview.path");
    std::string& previewName = ui.state<std::string>("chat.image.preview.name");
    const std::optional<std::filesystem::path> imagePath =
        resolveRenderableImagePath(part);
    if (!imagePath.has_value()) {
        drawFileDocumentCard(ui,
                             id,
                             x,
                             y,
                             width,
                             messagePartTitle(part),
                             messagePartDetail(part),
                             false,
                             false);
        return;
    }

    const std::string imagePathText =
        filesystemPathToGenericUtf8String(imagePath.value());
    const std::string imageName = messagePartTitle(part);
    const float imageWidth = width;
    rect(ui, id + ".frame", x, y, imageWidth, height,
         {1.0f, 1.0f, 1.0f, 0.72f}, 8.0f, kBorder);
    ui.image(id + ".image")
        .position(x + 6.0f, y + 6.0f)
        .size(imageWidth - 12.0f, height - 12.0f)
        .path(imagePathText)
        .contain()
        .radius(6.0f)
        .build();
    ui.rect(id + ".hit")
        .position(x, y)
        .size(imageWidth, height)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&previewOpen, &previewPath, &previewName, imagePathText, imageName] {
            previewPath = imagePathText;
            previewName = imageName;
            previewOpen = true;
        })
        .onContextMenu([&stickerMenuOpen,
                        &stickerMenuX,
                        &stickerMenuY,
                        &stickerMenuPath,
                        &stickerMenuName,
                        imagePathText,
                        imageName](const eui::PointerEvent& event,
                                   const eui::Rect&) {
            stickerMenuOpen = true;
            stickerMenuX = static_cast<float>(event.x);
            stickerMenuY = static_cast<float>(event.y);
            stickerMenuPath = imagePathText;
            stickerMenuName = imageName;
        })
        .build();
}

void drawMessageFlowTextAtoms(eui::Ui& ui,
                              const std::string& id,
                              const MessageFlowLayout& layout,
                              float x,
                              float y)
{
    std::string fragmentText;
    float fragmentX = 0.0f;
    float fragmentY = 0.0f;
    float fragmentWidth = 0.0f;
    float fragmentFontSize = kComposerEditorFontSize;
    float fragmentLineHeight = kComposerEditorLineHeight;
    bool fragmentEmoji = false;
    std::size_t fragmentIndex = 0;
    auto flushFragment = [&] {
        if (fragmentText.empty()) {
            return;
        }
        paragraphText(ui,
                      id + ".text." + std::to_string(fragmentIndex),
                      x + fragmentX,
                      y + fragmentY,
                      fragmentWidth + 6.0f,
                      fragmentLineHeight,
                      fragmentText,
                      fragmentFontSize,
                      fragmentLineHeight);
        fragmentText.clear();
        fragmentWidth = 0.0f;
        ++fragmentIndex;
    };

    for (const MessageFlowTextAtom& atom : layout.textAtoms) {
        const bool sameLine = !fragmentText.empty()
            && std::fabs(atom.y - fragmentY) < 0.5f
            && std::fabs(atom.x - (fragmentX + fragmentWidth)) < 1.5f
            && std::fabs(atom.fontSize - fragmentFontSize) < 0.5f
            && std::fabs(atom.lineHeight - fragmentLineHeight) < 0.5f
            && atom.emoji == fragmentEmoji;
        if (!sameLine) {
            flushFragment();
            fragmentX = atom.x;
            fragmentY = atom.y;
            fragmentFontSize = atom.fontSize;
            fragmentLineHeight = atom.lineHeight;
            fragmentEmoji = atom.emoji;
        }
        fragmentText += atom.text;
        fragmentWidth = (atom.x + atom.width) - fragmentX;
    }
    flushFragment();
}

void drawMessagePartNode(eui::Ui& ui,
                         const std::string& id,
                         float x,
                         float y,
                         const relaydesk::storage::ChatMessagePart& part,
                         const MessageFlowPartNode& node,
                         bool& stickerMenuOpen,
                         float& stickerMenuX,
                         float& stickerMenuY,
                         std::string& stickerMenuPath,
                         std::string& stickerMenuName)
{
    switch (part.GetType()) {
    case relaydesk::storage::MessagePartType::Image:
        drawMessageImagePart(ui,
                             id,
                             x,
                             y,
                             node.width,
                             node.height,
                             part,
                             stickerMenuOpen,
                             stickerMenuX,
                             stickerMenuY,
                             stickerMenuPath,
                             stickerMenuName);
        return;
    case relaydesk::storage::MessagePartType::File:
        drawFileDocumentCard(ui,
                             id,
                             x,
                             y,
                             node.width,
                             messagePartTitle(part),
                             messagePartDetail(part),
                             false,
                             false);
        return;
    case relaydesk::storage::MessagePartType::Folder:
        drawFileDocumentCard(ui,
                             id,
                             x,
                             y,
                             node.width,
                             messagePartTitle(part),
                             messagePartDetail(part),
                             true,
                             false);
        return;
    case relaydesk::storage::MessagePartType::Text:
    case relaydesk::storage::MessagePartType::Emoji:
        return;
    }
}

float drawMessageDocumentBubble(
    eui::Ui& ui,
    const std::string& id,
    float x,
    float y,
    float width,
    const relaydesk::storage::ChatMessageRecord& message,
    bool outgoing,
    bool& stickerMenuOpen,
    float& stickerMenuX,
    float& stickerMenuY,
    std::string& stickerMenuPath,
    std::string& stickerMenuName)
{
    const float innerWidth =
        std::max(80.0f, width - kMessageBubblePadding * 2.0f);
    const MessageFlowLayout layout = makeMessageFlowLayout(message, innerWidth);
    const float bubbleHeight = layout.contentHeight
        + kMessageBubblePadding * 2.0f
        + messageDeliveryStateFooterHeight(message, outgoing);
    const Color fill = outgoing ? kTealSoft : Color{0.990f, 0.990f, 0.992f, 1.0f};

    rect(ui, id + ".bg", x, y, width, bubbleHeight, fill, 9.0f, kBorder);

    const float contentX = x + kMessageBubblePadding;
    const float contentY = y + kMessageBubblePadding;
    for (std::size_t nodeIndex = 0; nodeIndex < layout.partNodes.size();
         ++nodeIndex) {
        const MessageFlowPartNode& node = layout.partNodes[nodeIndex];
        if (node.partIndex >= message.GetParts().size()) {
            continue;
        }
        drawMessagePartNode(ui,
                            id + ".node." + std::to_string(nodeIndex),
                            contentX + node.x,
                            contentY + node.y,
                            message.GetParts()[node.partIndex],
                            node,
                            stickerMenuOpen,
                            stickerMenuX,
                            stickerMenuY,
                            stickerMenuPath,
                            stickerMenuName);
    }
    drawMessageFlowTextAtoms(ui, id, layout, contentX, contentY);

    return bubbleHeight;
}

void transferCard(eui::Ui& ui,
                  const std::string& id,
                  float x,
                  float y,
                  float width,
                  const TransferPreview& transfer)
{
    const Color fill = transfer.accent.r > 0.5f ? kAmberSoft : kTealSoft;
    const Color border = transfer.accent.r > 0.5f
        ? Color{0.950f, 0.760f, 0.420f, 1.0f}
        : Color{0.640f, 0.880f, 0.870f, 1.0f};
    rect(ui, id + ".bg", x, y, width, 112.0f, fill, 8.0f, border);
    icon(ui, id + ".file", x + 18.0f, y + 22.0f, 42.0f, 0xE7C3, transfer.accent);
    text(ui, id + ".name", x + 72.0f, y + 20.0f, width - 160.0f, 24.0f,
         transfer.fileName, 15.0f);
    text(ui, id + ".size", x + 72.0f, y + 46.0f, 170.0f, 20.0f, transfer.direction,
         13.0f, kMutedText);
    text(ui, id + ".percent", x + width - 80.0f, y + 46.0f, 46.0f, 20.0f,
         std::to_string(static_cast<int>(transfer.progress * 100.0f)) + "%", 13.0f,
         kText, eui::HorizontalAlign::Right);
    ui.stack(id + ".progress.pos")
        .position(x + 72.0f, y + 72.0f)
        .size(width - 118.0f, 7.0f)
        .content([&] {
            components::progress(ui, id + ".progress")
                .size(width - 118.0f, 7.0f)
                .value(transfer.progress)
                .style(progressStyle({0.840f, 0.850f, 0.850f, 1.0f}, transfer.accent))
                .build();
        })
        .build();
    text(ui, id + ".detail", x + 72.0f, y + 84.0f, width - 118.0f, 20.0f,
         transfer.detail, 12.0f, kMutedText);
}

std::string makeLocalStatusText(const relaydesk::runtime::RelayDeskRuntime& runtime)
{
    std::string value = "本机";
    const auto& localUser = runtime.GetLocalUser();
    if (!localUser.GetHostName().empty()) {
        value += " - ";
        value += localUser.GetHostName();
    }

    if (runtime.GetDiscoveryStarted()) {
        value += " - 发现端口 ";
        value += std::to_string(runtime.GetDiscoveryUdpPort());
    }
    return value;
}

void drawRuntimeLocalUserHeader(eui::Ui& ui,
                                const relaydesk::runtime::RelayDeskRuntime& runtime,
                                float x,
                                float y,
                                float width)
{
    const auto& localUser = runtime.GetLocalUser();
    const std::string displayName = localUser.GetDisplayName().empty()
        ? "RelayDesk"
        : localUser.GetDisplayName();

    avatar(ui, "local.avatar", x + 22.0f, y + 16.0f, 44.0f, displayName);
    text(ui, "local.name", x + 78.0f, y + 15.0f, width - 132.0f, 26.0f,
         displayName, 17.0f);
    text(ui, "local.address", x + 78.0f, y + 41.0f, width - 132.0f, 22.0f,
         makeLocalStatusText(runtime), 13.0f, kMutedText);
    icon(ui, "local.settings", x + width - 50.0f, y + 21.0f, 32.0f, 0xE713, kText);
    rect(ui, "local.bottom.line", x, y + 76.0f, width, 1.0f, kBorder);
}

std::vector<PeerPreview> makePeerPreviews(
    const relaydesk::runtime::RelayDeskRuntime& runtime)
{
    const auto& peers = runtime.GetPeers();
    const std::string& selectedDeviceId = runtime.GetSelectedPeerDeviceId();
    std::vector<PeerPreview> result;
    result.reserve(peers.size());
    for (std::size_t index = 0; index < peers.size(); ++index) {
        const auto& peer = peers[index];
        result.push_back(PeerPreview{
            peer.GetDeviceId(),
            peer.GetDisplayName().empty() ? peer.GetHostName() : peer.GetDisplayName(),
            peer.GetAddress(),
            peer.GetOnline(),
            peer.GetDeviceId() == selectedDeviceId,
        });
    }
    return result;
}

void peerRowHitTarget(eui::Ui& ui,
                      const PeerPreview& peer,
                      int index,
                      relaydesk::runtime::RelayDeskRuntime& runtime,
                      float x,
                      float width,
                      float y)
{
    const std::string id = "peer.hit." + std::to_string(index);
    const std::string deviceId = peer.deviceId;
    ui.rect(id)
        .position(x + 6.0f, y - 8.0f)
        .size(width - 12.0f, 64.0f)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .radius(6.0f)
        .onClick([&runtime, deviceId] {
            runtime.selectPeer(deviceId);
        })
        .build();
}

void drawDiscoveredPeerList(eui::Ui& ui,
                            relaydesk::runtime::RelayDeskRuntime& runtime,
                            float x,
                            float width,
                            float height)
{
    rect(ui, "peers.bg", x, kContentTop, width, height, kPanelBackground);
    rect(ui, "peers.line", x + width, kContentTop, 1.0f, height, kBorder);
    drawRuntimeLocalUserHeader(ui, runtime, x, kContentTop, width);
    rect(ui, "peers.search.bg", x + 20.0f, kContentTop + 96.0f, width - 40.0f,
         42.0f, {1.0f, 1.0f, 1.0f, 1.0f}, 7.0f, kBorder);
    icon(ui, "peers.search.icon", x + 32.0f, kContentTop + 102.0f, 30.0f, 0xE721,
         kText);
    text(ui, "peers.search.placeholder", x + 74.0f, kContentTop + 105.0f,
         width - 112.0f, 24.0f,
         "搜索设备", 14.0f, kSubtleText);

    const std::vector<PeerPreview> peers = makePeerPreviews(runtime);
    text(ui, "peers.online.title", x + 42.0f, kContentTop + 166.0f, 180.0f, 24.0f,
         std::string("已发现 (") + std::to_string(peers.size()) + ")", 15.0f);

    if (peers.empty()) {
        text(ui, "peers.empty.title", x + 42.0f, kContentTop + 214.0f,
             width - 84.0f, 24.0f, "暂无已发现设备", 14.0f, kMutedText);
        const std::string detail = runtime.GetStartupErrorMessage().empty()
            ? "正在监听 RelayDesk 设备"
            : runtime.GetStartupErrorMessage();
        text(ui, "peers.empty.detail", x + 42.0f, kContentTop + 242.0f,
             width - 84.0f, 22.0f, detail, 12.0f, kSubtleText);
        return;
    }

    const float rowGap = height < 740.0f ? 59.0f : 72.0f;
    const float rowStart = kContentTop + 210.0f;
    for (int index = 0; index < static_cast<int>(peers.size()); ++index) {
        const float y = rowStart + static_cast<float>(index) * rowGap;
        peerRow(ui, peers[static_cast<std::size_t>(index)], index, x, width, y);
        peerRowHitTarget(ui,
                         peers[static_cast<std::size_t>(index)],
                         index,
                         runtime,
                         x,
                         width,
                         y);
    }
}

void drawPeerList(eui::Ui& ui, float x, float width, float height)
{
    rect(ui, "peers.bg", x, kContentTop, width, height, kPanelBackground);
    rect(ui, "peers.line", x + width, kContentTop, 1.0f, height, kBorder);
    drawLocalUserHeader(ui, x, kContentTop, width);
    rect(ui, "peers.search.bg", x + 20.0f, kContentTop + 96.0f, width - 40.0f,
         42.0f, {1.0f, 1.0f, 1.0f, 1.0f}, 7.0f, kBorder);
    icon(ui, "peers.search.icon", x + 32.0f, kContentTop + 102.0f, 30.0f, 0xE721,
         kText);
    text(ui, "peers.search.placeholder", x + 74.0f, kContentTop + 105.0f,
         width - 112.0f, 24.0f,
         "搜索设备", 14.0f, kSubtleText);
    text(ui, "peers.online.title", x + 42.0f, kContentTop + 166.0f, 120.0f, 24.0f,
         "在线 (5)", 15.0f);

    const std::array peers{
        PeerPreview{"demo-alex", "Alex-PC", "192.168.1.24", true, true},
        PeerPreview{"demo-desktop", "DESKTOP-J8K2TQ", "192.168.1.31", true, false},
        PeerPreview{"demo-laptop", "LAPTOP-9F3V2M", "192.168.1.42", true, false},
        PeerPreview{"demo-server", "DEV-SERVER", "192.168.1.10", true, false},
        PeerPreview{"demo-mark", "MARK-PC", "192.168.1.77", true, false},
        PeerPreview{"demo-finance", "FINANCE-PC", "192.168.1.15", false, false},
        PeerPreview{"demo-hr", "HR-LAPTOP", "192.168.1.28", false, false},
        PeerPreview{"demo-old", "OLD-PC", "192.168.1.55", false, false},
    };

    const float rowGap = height < 740.0f ? 59.0f : 72.0f;
    const float onlineStart = kContentTop + 210.0f;
    const float offlineTitleY = onlineStart + rowGap * 5.0f + 15.0f;
    text(ui, "peers.offline.title", x + 42.0f, offlineTitleY, 120.0f, 24.0f,
         "离线 (3)", 15.0f);

    for (int index = 0; index < static_cast<int>(peers.size()); ++index) {
        const float y = index < 5
            ? onlineStart + static_cast<float>(index) * rowGap
            : offlineTitleY + 40.0f + static_cast<float>(index - 5) * rowGap;
        peerRow(ui, peers[static_cast<std::size_t>(index)], index, x, width, y);
    }
}

void drawChatHeader(eui::Ui& ui, float x, float width)
{
    rect(ui, "chat.header.bg", x, kContentTop, width, kChatHeaderHeight - 1.0f,
         kPanelBackground);
    rect(ui, "chat.header.peer.line", x, kContentTop + 75.0f, width, 1.0f, kBorder);
    rect(ui, "chat.header.line", x, kContentTop + kChatHeaderHeight - 1.0f, width, 1.0f,
         kBorder);
    avatar(ui, "chat.header.avatar", x + 34.0f, kContentTop + 24.0f, 38.0f,
           "Alex-PC");
    statusDot(ui, "chat.header.dot", x + 96.0f, kContentTop + 50.0f, kGreen, 10.0f);
    text(ui, "chat.header.name", x + 96.0f, kContentTop + 17.0f, width - 210.0f,
         26.0f,
         "Alex-PC", 18.0f);
    text(ui, "chat.header.ip", x + 112.0f, kContentTop + 43.0f, width - 230.0f, 22.0f,
         "192.168.1.24",
         13.0f, kMutedText);
    icon(ui, "chat.header.search", x + width - 100.0f, kContentTop + 25.0f, 34.0f,
         0xE721, kText);
    icon(ui, "chat.header.more", x + width - 44.0f, kContentTop + 25.0f, 34.0f,
         0xE712, kText);
    rect(ui, "chat.tabs.chat.bg", x + 22.0f, kContentTop + 85.0f, 62.0f, 24.0f,
         kTealSoft, 6.0f, {0.640f, 0.880f, 0.870f, 1.0f});
    text(ui, "chat.tabs.chat.text", x + 22.0f, kContentTop + 87.0f, 62.0f, 20.0f,
         "聊天", 13.0f, kTeal, eui::HorizontalAlign::Center);
    text(ui, "chat.tabs.history", x + 98.0f, kContentTop + 87.0f, 62.0f, 20.0f,
         "历史", 13.0f, kMutedText, eui::HorizontalAlign::Center);
    text(ui, "chat.tabs.transfers", x + 174.0f, kContentTop + 87.0f, 62.0f, 20.0f,
         "传输", 13.0f, kMutedText, eui::HorizontalAlign::Center);
}

std::string getPeerDisplayName(const relaydesk::runtime::PeerListItem& peer)
{
    if (!peer.GetDisplayName().empty()) {
        return peer.GetDisplayName();
    }

    if (!peer.GetHostName().empty()) {
        return peer.GetHostName();
    }

    return peer.GetDeviceId();
}

std::string getSelectedPeerTitle(
    const std::optional<relaydesk::runtime::PeerListItem>& selectedPeer)
{
    if (!selectedPeer.has_value()) {
        return "未选择设备";
    }

    return getPeerDisplayName(selectedPeer.value());
}

std::string getSelectedPeerAddress(
    const std::optional<relaydesk::runtime::PeerListItem>& selectedPeer)
{
    if (!selectedPeer.has_value()) {
        return "等待发现设备";
    }

    return selectedPeer->GetAddress();
}

void drawRuntimeChatHeader(
    eui::Ui& ui,
    float x,
    float width,
    const std::optional<relaydesk::runtime::PeerListItem>& selectedPeer)
{
    const Color statusColor =
        selectedPeer.has_value() && selectedPeer->GetOnline() ? kGreen : kOffline;

    rect(ui, "chat.header.bg", x, kContentTop, width, kChatHeaderHeight - 1.0f,
         kPanelBackground);
    rect(ui, "chat.header.peer.line", x, kContentTop + 75.0f, width, 1.0f, kBorder);
    rect(ui, "chat.header.line", x, kContentTop + kChatHeaderHeight - 1.0f, width,
         1.0f, kBorder);
    avatar(ui, "chat.header.avatar", x + 34.0f, kContentTop + 24.0f, 38.0f,
           getSelectedPeerTitle(selectedPeer));
    statusDot(ui, "chat.header.dot", x + 96.0f, kContentTop + 50.0f, statusColor,
              10.0f);
    text(ui, "chat.header.name", x + 96.0f, kContentTop + 17.0f, width - 210.0f,
         26.0f, getSelectedPeerTitle(selectedPeer), 18.0f);
    text(ui, "chat.header.ip", x + 112.0f, kContentTop + 43.0f, width - 230.0f,
         22.0f, getSelectedPeerAddress(selectedPeer), 13.0f, kMutedText);
    icon(ui, "chat.header.search", x + width - 100.0f, kContentTop + 25.0f, 34.0f,
         0xE721, kText);
    icon(ui, "chat.header.more", x + width - 44.0f, kContentTop + 25.0f, 34.0f,
         0xE712, kText);
    rect(ui, "chat.tabs.chat.bg", x + 22.0f, kContentTop + 85.0f, 62.0f, 24.0f,
         kTealSoft, 6.0f, {0.640f, 0.880f, 0.870f, 1.0f});
    text(ui, "chat.tabs.chat.text", x + 22.0f, kContentTop + 87.0f, 62.0f, 20.0f,
         "聊天", 13.0f, kTeal, eui::HorizontalAlign::Center);
    text(ui, "chat.tabs.history", x + 98.0f, kContentTop + 87.0f, 62.0f, 20.0f,
         "历史", 13.0f, kMutedText, eui::HorizontalAlign::Center);
    text(ui, "chat.tabs.transfers", x + 174.0f, kContentTop + 87.0f, 72.0f,
         20.0f, "文件", 13.0f, kMutedText, eui::HorizontalAlign::Center);
}

void drawChatTimelineContent(eui::Ui& ui, float width)
{
    const float x = 0.0f;
    const auto rowY = [](float offset) {
        return offset;
    };
    const float todayX = std::max(16.0f, (width - 66.0f) * 0.5f);
    const float incomingWidth = std::min(380.0f, std::max(250.0f, width * 0.48f));
    const float outgoingWidth = std::min(235.0f, std::max(180.0f, width * 0.34f));
    const float reportLeft = std::clamp(width * 0.14f, 52.0f, 116.0f);
    const float reportRight = std::clamp(width * 0.10f, 52.0f, 94.0f);
    const float reportWidth = width - reportLeft - reportRight;
    const float specsWidth = width - 48.0f;

    rect(ui, "chat.today", todayX, rowY(25.0f), 66.0f, 30.0f, kPanelBackground, 8.0f,
         kBorder);
    text(ui, "chat.today.text", todayX, rowY(29.0f), 66.0f, 20.0f, "今天", 13.0f,
         kMutedText, eui::HorizontalAlign::Center);
    messageBubble(ui, "chat.msg.1", x + 16.0f, rowY(67.0f), incomingWidth,
                  "能把最新的 Q2 报告发我吗？", false);
    text(ui, "chat.msg.1.time", x + 30.0f + incomingWidth, rowY(77.0f), 70.0f,
         20.0f, "09:21", 12.0f, kSubtleText);
    messageBubble(ui, "chat.msg.2", x + width - outgoingWidth - 92.0f, rowY(131.0f),
                  outgoingWidth, "可以，正在上传。", true);
    text(ui, "chat.msg.2.time", x + width - 82.0f, rowY(141.0f), 70.0f, 20.0f,
         "09:21", 12.0f, kMutedText);

    transferCard(ui, "chat.transfer.report", x + reportLeft, rowY(189.0f), reportWidth,
                 {"Q2_Report_2024.pdf",
                  "24.8 MB",
                  "19.4 MB / 24.8 MB  -  5.2 MB/s  -  剩余 00:00:01",
                  0.78f,
                  kTeal});
    text(ui, "chat.transfer.report.time", x + width - 92.0f, rowY(301.0f), 70.0f,
         20.0f, "09:22", 12.0f, kMutedText);
    messageBubble(ui, "chat.msg.3", x + 16.0f, rowY(327.0f), incomingWidth,
                  "谢谢！设计说明也在吗？", false);
    text(ui, "chat.msg.3.time", x + 30.0f + incomingWidth, rowY(337.0f), 70.0f,
         20.0f, "09:23", 12.0f, kSubtleText);
    transferCard(ui, "chat.transfer.specs", x + 16.0f, rowY(395.0f), specsWidth,
                 {"Design_Specs_v2.zip",
                  "112.6 MB",
                  "50.7 MB / 112.6 MB  -  4.1 MB/s  -  剩余 00:00:15",
                  0.45f,
                  kAmber});
    text(ui, "chat.transfer.specs.time", x + width - 92.0f, rowY(507.0f), 70.0f,
         20.0f, "09:24", 12.0f, kMutedText);
    messageBubble(ui, "chat.msg.4", x + width - outgoingWidth - 95.0f, rowY(560.0f),
                  outgoingWidth, "是的，这是最新版本。", true);
    text(ui, "chat.msg.4.time", x + width - 82.0f, rowY(570.0f), 70.0f, 20.0f,
         "09:25", 12.0f, kMutedText);
}

void drawChatTimeline(eui::Ui& ui, float x, float y, float width, float height)
{
    rect(ui, "chat.bg", x, y, width, height, {1.0f, 1.0f, 1.0f, 1.0f});

    float& scrollOffset = ui.state<float>("chat.timeline.scroll.offset");
    ui.stack("chat.timeline.scroll.pos")
        .position(x, y)
        .size(width, height)
        .content([&] {
            components::scrollView(ui, "chat.timeline.scroll")
                .size(width, height)
                .offset(scrollOffset)
                .gap(0.0f)
                .step(56.0f)
                .scrollbarWidth(7.0f)
                .scrollbarGap(10.0f)
                .style(scrollStyle())
                .contentKey("relaydesk.chat.timeline.v1")
                .onChange([&scrollOffset](float value) {
                    scrollOffset = value;
                })
                .content([&](eui::Ui& contentUi, float contentWidth, float) {
                    contentUi.stack("chat.timeline.content")
                        .size(contentWidth, kChatTimelineContentHeight)
                        .content([&] {
                            drawChatTimelineContent(contentUi, contentWidth);
                        })
                        .build();
                })
                .build();
        })
        .build();
}

std::string deliveryStateText(relaydesk::storage::DeliveryState state)
{
    switch (state) {
    case relaydesk::storage::DeliveryState::Pending:
        return "发送中";
    case relaydesk::storage::DeliveryState::Sent:
        return "已发送";
    case relaydesk::storage::DeliveryState::Delivered:
        return "已送达";
    case relaydesk::storage::DeliveryState::Received:
        return "已接收";
    case relaydesk::storage::DeliveryState::Completed:
        return "已完成";
    case relaydesk::storage::DeliveryState::Failed:
        return "发送失败";
    case relaydesk::storage::DeliveryState::Cancelled:
        return "已取消";
    }

    return "";
}

void drawRuntimeMessageDeliveryState(
    eui::Ui& ui,
    const std::string& id,
    float bubbleX,
    float statusY,
    float bubbleWidth,
    const relaydesk::storage::ChatMessageRecord& message,
    relaydesk::runtime::RelayDeskRuntime& runtime)
{
    const bool failed =
        message.GetDeliveryState() == relaydesk::storage::DeliveryState::Failed;
    if (!failed) {
        text(ui,
             id + ".state",
             bubbleX,
             statusY,
             bubbleWidth - 4.0f,
             18.0f,
             deliveryStateText(message.GetDeliveryState()),
             11.0f,
             kSubtleText,
             eui::HorizontalAlign::Right);
        return;
    }

    constexpr float stateTextWidth = 72.0f;
    constexpr float retryGap = 6.0f;
    constexpr float retryButtonSize = 20.0f;
    const float rowWidth = stateTextWidth + retryGap + retryButtonSize;
    const float rowX = bubbleX + bubbleWidth - rowWidth - 4.0f;
    const float retryButtonX = rowX + stateTextWidth + retryGap;
    rect(ui,
         id + ".state.bg",
         rowX,
         statusY + 1.0f,
         stateTextWidth,
         18.0f,
         kDangerSoft,
         6.0f,
         kDanger);
    text(ui,
         id + ".state",
         rowX,
         statusY + 1.0f,
         stateTextWidth,
         18.0f,
         deliveryStateText(message.GetDeliveryState()),
         11.0f,
         kDanger,
         eui::HorizontalAlign::Center);
    rect(ui,
         id + ".retry.bg",
         retryButtonX,
         statusY,
         retryButtonSize,
         retryButtonSize,
         Color{1.0f, 1.0f, 1.0f, 0.82f},
         10.0f,
         kBorder);
    icon(ui,
         id + ".retry.icon",
         retryButtonX + 2.0f,
         statusY + 2.0f,
         16.0f,
         kRefreshIconCodePoint,
         kTeal);
    ui.rect(id + ".retry.hit")
        .position(retryButtonX, statusY)
        .size(retryButtonSize, retryButtonSize)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&runtime, messageId = message.GetMessageId()] {
            runtime.resendSelectedPeerMessage(messageId);
        })
        .build();
}

float runtimeMessageBubbleWidth(float timelineWidth, float availableBubbleWidth)
{
    const float maxWidth = availableBubbleWidth;
    if (maxWidth <= 210.0f) {
        return maxWidth;
    }

    return std::clamp(timelineWidth * 0.86f, 210.0f, maxWidth);
}

void drawRuntimeChatTimelineContent(
    eui::Ui& ui,
    float width,
    const std::vector<relaydesk::storage::ChatMessageRecord>& messages,
    relaydesk::runtime::RelayDeskRuntime& runtime)
{
    constexpr float avatarSize = 34.0f;
    constexpr float sidePadding = 22.0f;
    constexpr float avatarBubbleGap = 12.0f;
    const float availableBubbleWidth =
        std::max(160.0f, width - sidePadding * 2.0f - avatarSize - avatarBubbleGap);
    const float messageBubbleWidth =
        runtimeMessageBubbleWidth(width, availableBubbleWidth);
    float y = 22.0f;
    bool& stickerMenuOpen = ui.state<bool>("chat.sticker.context.open");
    float& stickerMenuX = ui.state<float>("chat.sticker.context.x");
    float& stickerMenuY = ui.state<float>("chat.sticker.context.y");
    std::string& stickerMenuPath =
        ui.state<std::string>("chat.sticker.context.path");
    std::string& stickerMenuName =
        ui.state<std::string>("chat.sticker.context.name");
    std::string& stickerMenuStatus =
        ui.state<std::string>("chat.sticker.context.status");

    for (std::size_t index = 0; index < messages.size(); ++index) {
        const auto& message = messages[index];
        const bool outgoing =
            message.GetDirection() == relaydesk::storage::MessageDirection::Outgoing;
        const float bubbleWidth = messageBubbleWidth;
        const float avatarX = outgoing
            ? width - sidePadding - avatarSize
            : sidePadding;
        const float bubbleX = outgoing
            ? avatarX - avatarBubbleGap - bubbleWidth
            : avatarX + avatarSize + avatarBubbleGap;
        const std::string id = "chat.runtime.message."
            + std::to_string(index);
        avatar(ui,
               id + ".avatar",
               avatarX,
               y + 4.0f,
               avatarSize,
               message.GetSenderDisplayNameSnapshot());
        const float bubbleHeight = drawMessageDocumentBubble(ui,
                                                             id,
                                                             bubbleX,
                                                             y,
                                                             bubbleWidth,
                                                             message,
                                                             outgoing,
                                                             stickerMenuOpen,
                                                             stickerMenuX,
                                                             stickerMenuY,
                                                             stickerMenuPath,
                                                             stickerMenuName);
        if (outgoing) {
            const bool failedStateInside =
                shouldDrawFailedDeliveryStateInsideBubble(message, outgoing);
            const float stateX = failedStateInside
                ? bubbleX + kMessageBubblePadding
                : bubbleX;
            const float stateY = failedStateInside
                ? y + bubbleHeight
                    - kMessageBubblePadding
                    - kFailedDeliveryStateHeight
                : y + bubbleHeight;
            const float stateWidth = failedStateInside
                ? bubbleWidth - kMessageBubblePadding * 2.0f
                : bubbleWidth;
            drawRuntimeMessageDeliveryState(ui,
                                           id,
                                           stateX,
                                           stateY,
                                           stateWidth,
                                           message,
                                           runtime);
        }
        const bool failedStateInside =
            shouldDrawFailedDeliveryStateInsideBubble(message, outgoing);
        y += bubbleHeight + (outgoing && !failedStateInside ? 24.0f : 16.0f);
    }

    if (stickerMenuOpen) {
        components::contextMenu(ui, "chat.sticker.context")
            .screen(width, kChatTimelineContentHeight)
            .position(stickerMenuX, stickerMenuY)
            .size(150.0f, 34.0f)
            .items({"收藏为表情", "取消"})
            .style(stickerContextMenuStyle())
            .open(stickerMenuOpen)
            .onSelect([&stickerMenuOpen,
                       &stickerMenuPath,
                       &stickerMenuName,
                       &stickerMenuStatus](int itemIndex) {
                if (itemIndex == 0 && !stickerMenuPath.empty()) {
                    const auto error =
                        favoriteStickerImage(stickerMenuPath, stickerMenuName);
                    stickerMenuStatus = error.value_or("已收藏为表情");
                }
                stickerMenuOpen = false;
            })
            .onDismiss([&stickerMenuOpen] {
                stickerMenuOpen = false;
            })
            .build();
    }
}

void drawRuntimeChatTimeline(
    eui::Ui& ui,
    float x,
    float y,
    float width,
    float height,
    const std::optional<relaydesk::runtime::PeerListItem>& selectedPeer,
    const std::vector<relaydesk::storage::ChatMessageRecord>& messages,
    relaydesk::runtime::RelayDeskRuntime& runtime)
{
    rect(ui, "chat.bg", x, y, width, height, {1.0f, 1.0f, 1.0f, 1.0f});

    if (selectedPeer.has_value() && !messages.empty()) {
        float& scrollOffset = ui.state<float>("chat.runtime.scroll.offset");
        constexpr float avatarSize = 34.0f;
        constexpr float sidePadding = 22.0f;
        constexpr float avatarBubbleGap = 12.0f;
        const float availableBubbleWidth =
            std::max(160.0f,
                     width - sidePadding * 2.0f - avatarSize - avatarBubbleGap);
        const float messageBubbleWidth =
            runtimeMessageBubbleWidth(width, availableBubbleWidth);
        float measuredContentHeight = 42.0f;
        for (const auto& message : messages) {
            const bool outgoing =
                message.GetDirection()
                == relaydesk::storage::MessageDirection::Outgoing;
            const float bubbleWidth = messageBubbleWidth;
            const float innerWidth =
                std::max(80.0f, bubbleWidth - kMessageBubblePadding * 2.0f);
            const bool failedStateInside =
                shouldDrawFailedDeliveryStateInsideBubble(message, outgoing);
            measuredContentHeight += messageDocumentBubbleHeight(message,
                                                                 innerWidth,
                                                                 outgoing)
                + (outgoing && !failedStateInside ? 24.0f : 16.0f);
        }
        const float contentHeight = std::max(height, measuredContentHeight);
        const float maxScrollOffset = std::max(0.0f, contentHeight - height);
        std::string& scrollPeerDeviceId =
            ui.state<std::string>("chat.runtime.scroll.peer.device_id");
        std::string& scrollTailMessageId =
            ui.state<std::string>("chat.runtime.scroll.tail.message_id");
        std::size_t& scrollMessageCount =
            ui.state<std::size_t>("chat.runtime.scroll.message_count");
        float& previousMaxScrollOffset =
            ui.state<float>("chat.runtime.scroll.previous_max_offset");
        float& previousContentHeight =
            ui.state<float>("chat.runtime.scroll.previous_content_height");
        const std::string& peerDeviceId = selectedPeer->GetDeviceId();
        const std::string& tailMessageId = messages.back().GetMessageId();
        const bool peerChanged = scrollPeerDeviceId != peerDeviceId;
        const bool tailChanged = scrollTailMessageId != tailMessageId
            || scrollMessageCount != messages.size();
        const bool contentHeightChanged =
            std::abs(previousContentHeight - contentHeight) > 0.5f;
        const bool wasAtBottom = previousMaxScrollOffset <= 0.5f
            || scrollOffset >= previousMaxScrollOffset - 8.0f;
        if (peerChanged || ((tailChanged || contentHeightChanged) && wasAtBottom)) {
            scrollOffset = maxScrollOffset;
        } else {
            scrollOffset = std::clamp(scrollOffset, 0.0f, maxScrollOffset);
        }
        scrollPeerDeviceId = peerDeviceId;
        scrollTailMessageId = tailMessageId;
        scrollMessageCount = messages.size();
        previousMaxScrollOffset = maxScrollOffset;
        previousContentHeight = contentHeight;
        const int contentHeightKey =
            static_cast<int>(std::ceil(contentHeight));
        const std::string timelineContentKey = "relaydesk.chat.runtime."
            + peerDeviceId
            + "."
            + std::to_string(messages.size())
            + "."
            + tailMessageId
            + "."
            + std::to_string(contentHeightKey);
        ui.stack("chat.runtime.scroll.pos")
            .position(x, y)
            .size(width, height)
            .content([&] {
                components::scrollView(ui, "chat.runtime.scroll")
                    .size(width, height)
                    .offset(scrollOffset)
                    .gap(0.0f)
                    .step(56.0f)
                    .scrollbarWidth(7.0f)
                    .scrollbarGap(10.0f)
                    .style(scrollStyle())
                    .contentKey(timelineContentKey)
                    .onChange([&scrollOffset](float value) {
                        scrollOffset = value;
                    })
                    .content([&](eui::Ui& contentUi, float contentWidth, float) {
                        contentUi.stack("chat.runtime.content")
                            .size(contentWidth, contentHeight)
                            .content([&] {
                                drawRuntimeChatTimelineContent(contentUi,
                                                               contentWidth,
                                                               messages,
                                                               runtime);
                            })
                            .build();
                    })
                    .build();
            })
            .build();
        return;
    }

    const std::string title = selectedPeer.has_value()
        ? "暂无本地消息"
        : "选择一个已发现设备";
    const std::string detail = selectedPeer.has_value()
        ? "发送或接收消息后会在这里显示"
        : "RelayDesk 正在监听发现端口";
    const float centerY = y + std::max(0.0f, height * 0.5f - 34.0f);
    text(ui, "chat.empty.title", x + 24.0f, centerY, width - 48.0f, 28.0f,
         title, 17.0f, kText, eui::HorizontalAlign::Center);
    text(ui, "chat.empty.detail", x + 24.0f, centerY + 34.0f, width - 48.0f,
         24.0f, detail, 13.0f, kMutedText, eui::HorizontalAlign::Center);
}

void drawComposer(eui::Ui& ui, float x, float y, float width)
{
    const float horizontalPadding = width < 560.0f ? 12.0f : 16.0f;
    const float innerPadding = width < 560.0f ? 8.0f : 12.0f;
    const float sendWidth = width < 560.0f ? 58.0f : 70.0f;
    const float sendX = x + width - horizontalPadding - innerPadding - sendWidth;
    const float iconSize = width < 560.0f ? 34.0f : 38.0f;
    const float firstActionX = width < 560.0f ? sendX - 48.0f : x + width - 272.0f;
    const float inputWidth = std::max(140.0f, firstActionX - (x + 28.0f) - innerPadding);

    rect(ui, "composer.bg", x + horizontalPadding, y, width - horizontalPadding * 2.0f,
         kComposerHeight, kPanelBackground, 8.0f, kBorder);
    ui.stack("composer.input.pos")
        .position(x + 28.0f, y + 13.0f)
        .size(inputWidth, 42.0f)
        .content([&] {
            components::input(ui, "composer.input")
                .size(inputWidth, 42.0f)
                .placeholder("给 Alex-PC 发消息")
                .fontSize(14.0f)
                .build();
        })
        .build();
    if (width < 560.0f) {
        icon(ui, "composer.file", firstActionX, y + 17.0f, iconSize, 0xE723, kText);
    } else {
        icon(ui, "composer.smile", firstActionX, y + 15.0f, iconSize, 0xE899, kText);
        icon(ui, "composer.file", x + width - 212.0f, y + 15.0f, iconSize, 0xE723,
             kText);
        icon(ui, "composer.folder", x + width - 152.0f, y + 15.0f, iconSize, 0xE8B7,
             kText);
    }
    rect(ui, "composer.send.bg", sendX, y + 13.0f, sendWidth, 42.0f, kTeal, 6.0f);
    text(ui, "composer.send.text", sendX, y + 21.0f, sendWidth, 24.0f, "发送", 14.0f,
         {1.0f, 1.0f, 1.0f, 1.0f}, eui::HorizontalAlign::Center);
}

void drawRuntimeComposer(
    eui::Ui& ui,
    float x,
    float y,
    float width,
    relaydesk::runtime::RelayDeskRuntime& runtime,
    const std::optional<relaydesk::runtime::PeerListItem>& selectedPeer)
{
    const float horizontalPadding = width < 560.0f ? 12.0f : 16.0f;
    const float composerX = x + horizontalPadding;
    const float composerWidth = width - horizontalPadding * 2.0f;
    const float inputX = composerX + 16.0f;
    const float inputY = y + 12.0f;
    const float toolbarY = y + kComposerHeight - 48.0f;
    const float sendWidth = width < 560.0f ? 60.0f : 72.0f;
    const float sendHeight = 36.0f;
    const float sendX = composerX + composerWidth - sendWidth - 16.0f;
    const float iconSize = width < 560.0f ? 28.0f : 32.0f;
    const float inputWidth = std::max(180.0f, composerWidth - 32.0f);
    const float inputHeight = std::max(70.0f, kComposerHeight - 66.0f);
    const float emojiButtonX = composerX + 16.0f;
    const float emojiButtonCenterX = emojiButtonX + iconSize * 0.5f;
    const std::string placeholder = selectedPeer.has_value()
        ? std::string("发给 ") + getPeerDisplayName(selectedPeer.value())
        : "请先选择设备";
    std::string& composerText = ui.state<std::string>("composer.input.value");
    ComposerCaretState& composerCaret =
        ui.state<ComposerCaretState>("composer.editor.caret");
    bool& emojiPickerOpen = ui.state<bool>("composer.emoji.open");
    int& emojiPickerTab = ui.state<int>("composer.emoji.tab");
    std::vector<std::string>& recentEmojis =
        ui.state<std::vector<std::string>>("composer.emoji.recent");
    std::vector<StickerPickerItem>& stickerItems =
        ui.state<std::vector<StickerPickerItem>>("composer.stickers.items");
    std::vector<ComposerDraftItem>& draftItems =
        ui.state<std::vector<ComposerDraftItem>>("composer.draft.items");
    std::string& stickerImportStatus =
        ui.state<std::string>("composer.stickers.import.status");
    bool& pasteShortcutDown = ui.state<bool>("composer.clipboard.paste.down");
    loadRecentEmojisOnce(ui, recentEmojis);
    loadStoredStickerPickerItemsOnce(ui, stickerItems);
    if (!composerText.empty()) {
        insertComposerDraftTextAtCaret(draftItems, composerCaret, composerText);
        composerText.clear();
    }
    insertComposerDraftAttachmentPathsAtCaret(
        draftItems,
        composerCaret,
        relaydesk::platform::consumeDroppedAttachmentPaths());
    const bool pasteShortcutNow = relaydesk::platform::isPasteShortcutDown();
    if (pasteShortcutNow && !pasteShortcutDown) {
        insertComposerDraftAttachmentPathsAtCaret(
            draftItems,
            composerCaret,
            relaydesk::platform::collectClipboardAttachmentPaths());
    }
    pasteShortcutDown = pasteShortcutNow;
    clampComposerCaret(draftItems, composerCaret);
    const bool hasMessageContent = hasComposerDraftContent(draftItems);
    const bool sendEnabled = selectedPeer.has_value() && hasMessageContent;
    auto submitMessage = [&runtime,
                          &composerCaret,
                          &emojiPickerOpen,
                          &draftItems,
                          sendEnabled] {
        if (!sendEnabled) {
            return;
        }

        std::vector<relaydesk::storage::ChatMessagePart> parts =
            makeComposerMessageParts(draftItems);
        if (parts.empty()) {
            return;
        }

        runtime.sendMessagePartsToSelectedPeer(std::move(parts));
        draftItems.clear();
        composerCaret = {};
        emojiPickerOpen = false;
    };
    auto toggleEmojiPicker = [&emojiPickerOpen] {
        emojiPickerOpen = !emojiPickerOpen;
    };
    auto closeEmojiPicker = [&emojiPickerOpen] {
        emojiPickerOpen = false;
    };
    auto selectEmoji = [&composerCaret,
                        &draftItems,
                        &emojiPickerOpen,
                        &recentEmojis](
                           const std::string& emoji) {
        insertComposerDraftTextAtCaret(draftItems, composerCaret, emoji);
        rememberRecentEmoji(recentEmojis, emoji);
        saveStoredRecentEmojis(recentEmojis);
        emojiPickerOpen = false;
    };
    auto selectSticker = [&composerCaret, &draftItems, &emojiPickerOpen](
                             const StickerPickerItem& sticker) {
        if (composerDraftAttachmentCount(draftItems) >= kMaxPendingAttachmentCount) {
            return;
        }

        insertComposerDraftAttachmentAtCaret(
            draftItems,
            composerCaret,
            makePendingAttachmentFromSticker(sticker));
        emojiPickerOpen = false;
    };
    auto selectAttachmentFiles = [&composerCaret, &draftItems, &emojiPickerOpen] {
        insertComposerDraftAttachmentPathsAtCaret(
            draftItems,
            composerCaret,
            selectAttachmentFilesFromDialog());
        emojiPickerOpen = false;
    };
    auto importStickers = [&emojiPickerTab,
                           &stickerImportStatus,
                           &stickerItems] {
        const std::optional<std::string> status = importStickerPackFromFileDialog();
        if (status.has_value()) {
            stickerImportStatus = status.value();
            stickerItems = loadStoredStickerPickerItems();
        }
        emojiPickerTab = 1;
    };
    auto selectEmojiPickerTab = [&emojiPickerTab](int tab) {
        emojiPickerTab = tab;
    };

    rect(ui, "composer.bg", composerX, y, composerWidth, kComposerHeight,
         kPanelBackground, 8.0f, kBorder);
    drawComposerEditor(ui,
                       inputX,
                       inputY,
                       inputWidth,
                       inputHeight,
                       draftItems,
                       composerCaret,
                       placeholder);
    if (width < 560.0f) {
        icon(ui, "composer.file", composerX + 16.0f, toolbarY, iconSize,
             0xE723, kText);
        ui.rect("composer.file.hit")
            .position(composerX + 11.0f, toolbarY - 5.0f)
            .size(iconSize + 10.0f, iconSize + 10.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(selectAttachmentFiles)
            .build();
    } else {
        const float firstIconX = emojiButtonX;
        const float iconGap = 46.0f;
        icon(ui, "composer.smile", firstIconX, toolbarY, iconSize, 0xE899,
             kText);
        ui.rect("composer.smile.hit")
            .position(firstIconX - 5.0f, toolbarY - 5.0f)
            .size(iconSize + 10.0f, iconSize + 10.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(toggleEmojiPicker)
            .build();
        icon(ui, "composer.file", firstIconX + iconGap, toolbarY, iconSize,
             0xE723, kText);
        ui.rect("composer.file.hit")
            .position(firstIconX + iconGap - 5.0f, toolbarY - 5.0f)
            .size(iconSize + 10.0f, iconSize + 10.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(selectAttachmentFiles)
            .build();
        icon(ui, "composer.folder", firstIconX + iconGap * 2.0f, toolbarY,
             iconSize, 0xE8B7, kText);
    }
    ui.rect("composer.send.bg")
        .position(sendX, toolbarY + 1.0f)
        .size(sendWidth, sendHeight)
        .color(sendEnabled ? kTeal : kOffline)
        .radius(6.0f)
        .onClick(submitMessage)
        .build();
    text(ui, "composer.send.text", sendX, toolbarY + 7.0f, sendWidth, 22.0f, "发送",
         14.0f, {1.0f, 1.0f, 1.0f, 1.0f}, eui::HorizontalAlign::Center);
    if (emojiPickerOpen) {
        ui.rect("emoji.picker.dismiss")
            .position(0.0f, 0.0f)
            .size(10000.0f, 10000.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(closeEmojiPicker)
            .build();
        const float pickerWidth =
            std::min(std::max(280.0f, composerWidth - 32.0f), 620.0f);
        const float pickerHeight =
            std::min(390.0f, std::max(240.0f, y - kContentTop - 12.0f));
        const float pickerX = std::clamp(emojiButtonCenterX - pickerWidth * 0.5f,
                                         x + 8.0f,
                                         x + width - pickerWidth - 8.0f);
        const float pickerY = std::max(kContentTop + 8.0f,
                                       toolbarY - pickerHeight - 2.0f);
        drawRichEmojiPicker(ui,
                            pickerX,
                            pickerY,
                            pickerWidth,
                            pickerHeight,
                            emojiButtonCenterX,
                            emojiPickerTab,
                            recentEmojis,
                            stickerItems,
                            stickerImportStatus,
                            selectEmoji,
                            selectSticker,
                            importStickers,
                            selectEmojiPickerTab);
    }
}

void transferSummary(eui::Ui& ui,
                     const std::string& id,
                     float x,
                     float y,
                     float width,
                     const TransferPreview& transfer)
{
    icon(ui, id + ".file", x, y + 2.0f, 36.0f, 0xE7C3, transfer.accent);
    text(ui, id + ".name", x + 46.0f, y + 0.0f, width - 112.0f, 24.0f,
         transfer.fileName, 14.0f);
    text(ui, id + ".dir", x + 46.0f, y + 24.0f, 150.0f, 20.0f, transfer.direction,
         12.0f, kMutedText);
    text(ui, id + ".pct", x + width - 54.0f, y + 18.0f, 54.0f, 22.0f,
         std::to_string(static_cast<int>(transfer.progress * 100.0f)) + "%", 14.0f,
         transfer.accent, eui::HorizontalAlign::Right);
    ui.stack(id + ".bar.pos")
        .position(x, y + 52.0f)
        .size(width, 6.0f)
        .content([&] {
            components::progress(ui, id + ".bar")
                .size(width, 6.0f)
                .value(transfer.progress)
                .style(progressStyle({0.830f, 0.840f, 0.845f, 1.0f}, transfer.accent))
                .build();
        })
        .build();
    text(ui, id + ".detail", x, y + 62.0f, width, 20.0f, transfer.detail, 12.0f,
         kMutedText);
}

void drawDetails(eui::Ui& ui, float x, float width, float height)
{
    const float bottom = kContentTop + height;
    const float summaryWidth = width - 44.0f;

    rect(ui, "details.bg", x, kContentTop, width, height, kPanelBackground);
    rect(ui, "details.line", x, kContentTop, 1.0f, height, kBorder);
    text(ui, "details.title", x + 22.0f, kContentTop + 19.0f, 170.0f, 28.0f,
         "Alex-PC", 17.0f);
    icon(ui, "details.close", x + width - 52.0f, kContentTop + 19.0f, 30.0f, 0xE711,
         kText);
    avatar(ui, "details.avatar", x + 50.0f, kContentTop + 73.0f, 78.0f,
           "Alex-PC");
    statusDot(ui, "details.status", x + 154.0f, kContentTop + 101.0f, kGreen, 12.0f);
    text(ui, "details.online", x + 176.0f, kContentTop + 91.0f, 120.0f, 28.0f,
         "在线", 18.0f, kGreen);
    text(ui, "details.ip", x + 176.0f, kContentTop + 125.0f, 120.0f, 24.0f,
         "192.168.1.24",
         15.0f, kText);
    rect(ui, "details.sep.1", x, kContentTop + 179.0f, width, 1.0f, kBorder);
    text(ui, "details.device.title", x + 22.0f, kContentTop + 203.0f, 160.0f,
         26.0f, "设备信息", 15.0f);
    const std::array labels{
        "电脑名", "用户", "系统", "IP 地址", "MAC 地址", "在线时长",
    };
    const std::array values{
        "Alex-PC", "Alex", "Windows 11 Pro 23H2", "192.168.1.24",
        "00-15-5D-8E-2A-7C", "2天 4时 18分",
    };
    for (int index = 0; index < static_cast<int>(labels.size()); ++index) {
        const float rowY = kContentTop + 253.0f + static_cast<float>(index) * 28.0f;
        text(ui, "details.label." + std::to_string(index), x + 22.0f, rowY,
             130.0f, 22.0f, labels[static_cast<std::size_t>(index)], 12.0f,
             kMutedText);
        text(ui, "details.value." + std::to_string(index), x + 170.0f, rowY,
             width - 190.0f, 22.0f, values[static_cast<std::size_t>(index)], 12.0f,
             kText, eui::HorizontalAlign::Right);
    }
    rect(ui, "details.sep.2", x, kContentTop + 417.0f, width, 1.0f, kBorder);
    text(ui, "details.transfers.title", x + 22.0f, kContentTop + 441.0f, 180.0f,
         26.0f, "活跃传输 (2)", 15.0f);
    const TransferPreview report{
        "Q2_Report_2024.pdf",
        "发给 Alex-PC",
        "19.4 MB / 24.8 MB  -  5.2 MB/s",
        0.78f,
        kTeal,
    };
    const TransferPreview specs{
        "Design_Specs_v2.zip",
        "来自 Alex-PC",
        "50.7 MB / 112.6 MB  -  4.1 MB/s",
        0.45f,
        kAmber,
    };
    const float firstTransferY = kContentTop + 486.0f;
    const float secondTransferY = std::min(kContentTop + 586.0f, bottom - 82.0f);
    transferSummary(ui, "details.transfer.report", x + 22.0f, firstTransferY,
                    summaryWidth, report);
    rect(ui, "details.transfer.sep", x + 22.0f, secondTransferY - 18.0f, summaryWidth,
         1.0f, kBorder);
    transferSummary(ui, "details.transfer.specs", x + 22.0f, secondTransferY,
                    summaryWidth, specs);
    if (height > 760.0f) {
        text(ui, "details.show.all", x + 22.0f, bottom - 80.0f, 180.0f, 24.0f,
             "查看全部传输", 14.0f, kTeal);
    }
}

void drawRuntimeDetails(
    eui::Ui& ui,
    float x,
    float width,
    float height,
    const std::optional<relaydesk::runtime::PeerListItem>& selectedPeer)
{
    rect(ui, "details.bg", x, kContentTop, width, height, kPanelBackground);
    rect(ui, "details.line", x, kContentTop, 1.0f, height, kBorder);
    text(ui, "details.title", x + 22.0f, kContentTop + 19.0f, width - 74.0f,
         28.0f, getSelectedPeerTitle(selectedPeer), 17.0f);
    icon(ui, "details.close", x + width - 52.0f, kContentTop + 19.0f, 30.0f, 0xE711,
         kText);
    avatar(ui, "details.avatar", x + 50.0f, kContentTop + 73.0f, 78.0f,
           getSelectedPeerTitle(selectedPeer));

    const bool online = selectedPeer.has_value() && selectedPeer->GetOnline();
    statusDot(ui, "details.status", x + 154.0f, kContentTop + 101.0f,
              online ? kGreen : kOffline, 12.0f);
    text(ui, "details.online", x + 176.0f, kContentTop + 91.0f, 120.0f, 28.0f,
         online ? "在线" : "离线", 18.0f, online ? kGreen : kOffline);
    text(ui, "details.ip", x + 176.0f, kContentTop + 125.0f, width - 198.0f,
         24.0f, getSelectedPeerAddress(selectedPeer), 15.0f, kText);
    rect(ui, "details.sep.1", x, kContentTop + 179.0f, width, 1.0f, kBorder);
    text(ui, "details.device.title", x + 22.0f, kContentTop + 203.0f,
         160.0f, 26.0f, "设备", 15.0f);

    const std::array labels{
        "显示名",
        "主机名",
        "地址",
    };
    const std::array values{
        selectedPeer.has_value() ? getPeerDisplayName(selectedPeer.value()) : "-",
        selectedPeer.has_value() ? selectedPeer->GetHostName() : "-",
        selectedPeer.has_value() ? selectedPeer->GetAddress() : "-",
    };

    for (int index = 0; index < static_cast<int>(labels.size()); ++index) {
        const float rowY = kContentTop + 253.0f + static_cast<float>(index) * 32.0f;
        text(ui, "details.label." + std::to_string(index), x + 22.0f, rowY,
             100.0f, 22.0f, labels[static_cast<std::size_t>(index)], 12.0f,
             kMutedText);
        text(ui, "details.value." + std::to_string(index), x + 128.0f, rowY,
             width - 150.0f, 22.0f, values[static_cast<std::size_t>(index)],
             12.0f, kText, eui::HorizontalAlign::Right);
    }

    rect(ui, "details.sep.2", x, kContentTop + 365.0f, width, 1.0f, kBorder);
    text(ui, "details.transfers.title", x + 22.0f, kContentTop + 389.0f,
         width - 44.0f, 26.0f, "传输", 15.0f);
    text(ui, "details.transfers.empty", x + 22.0f, kContentTop + 433.0f,
         width - 44.0f, 24.0f, "暂无活动传输", 13.0f, kMutedText);
}

void drawImagePreviewOverlay(eui::Ui& ui, float width, float height)
{
    bool& previewOpen = ui.state<bool>("chat.image.preview.open");
    std::string& previewPath = ui.state<std::string>("chat.image.preview.path");
    std::string& previewName = ui.state<std::string>("chat.image.preview.name");
    if (!previewOpen) {
        return;
    }

    ui.rect("chat.image.preview.backdrop")
        .position(0.0f, 0.0f)
        .size(width, height)
        .color({0.0f, 0.0f, 0.0f, 0.48f})
        .onClick([&previewOpen] {
            previewOpen = false;
        })
        .build();

    const float panelWidth = std::min(width - 48.0f, 960.0f);
    const float panelHeight = std::min(height - 56.0f, 760.0f);
    const float panelX = (width - panelWidth) * 0.5f;
    const float panelY = (height - panelHeight) * 0.5f;
    rect(ui, "chat.image.preview.panel", panelX, panelY, panelWidth, panelHeight,
         kPanelBackground, 8.0f, kBorder);
    ui.rect("chat.image.preview.panel.hit")
        .position(panelX, panelY)
        .size(panelWidth, panelHeight)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([] {})
        .build();

    text(ui,
         "chat.image.preview.title",
         panelX + 18.0f,
         panelY + 12.0f,
         panelWidth - 72.0f,
         24.0f,
         previewName.empty() ? "图片预览" : previewName,
         14.0f);
    icon(ui,
         "chat.image.preview.close.icon",
         panelX + panelWidth - 44.0f,
         panelY + 8.0f,
         32.0f,
         0xE711,
         kText);
    ui.rect("chat.image.preview.close.hit")
        .position(panelX + panelWidth - 48.0f, panelY + 4.0f)
        .size(40.0f, 40.0f)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&previewOpen] {
            previewOpen = false;
        })
        .build();

    if (previewPath.empty()) {
        text(ui,
             "chat.image.preview.empty",
             panelX + 18.0f,
             panelY + 70.0f,
             panelWidth - 36.0f,
             24.0f,
             "图片路径不可用",
             14.0f,
             kMutedText,
             eui::HorizontalAlign::Center);
        return;
    }

    ui.image("chat.image.preview.image")
        .position(panelX + 18.0f, panelY + 52.0f)
        .size(panelWidth - 36.0f, panelHeight - 70.0f)
        .path(previewPath)
        .contain()
        .radius(6.0f)
        .build();
}

void drawRelayDesk(eui::Ui& ui,
                   const eui::Screen& screen,
                   relaydesk::runtime::RelayDeskRuntime& runtime)
{
    relaydesk::platform::initializeAttachmentDropTarget();
    runtime.refreshPeersIfNeeded();

    const AppLayout layout = makeLayout(screen);
    const float composerY = layout.height - kComposerHeight - 16.0f;
    const float timelineY = kContentTop + kChatHeaderHeight;
    const float timelineHeight = std::max(1.0f, composerY - timelineY - 12.0f);
    const auto selectedPeer = runtime.GetSelectedPeer();

    rect(ui, "app.bg", 0.0f, 0.0f, layout.width, layout.height, kWindowBackground);

    if (layout.showPeers) {
        drawDiscoveredPeerList(ui,
                               runtime,
                               layout.peerX,
                               layout.peerWidth,
                               layout.contentHeight);
    }

    drawRuntimeChatHeader(ui, layout.chatX, layout.chatWidth, selectedPeer);
    drawRuntimeChatTimeline(ui,
                            layout.chatX,
                            timelineY,
                            layout.chatWidth,
                            timelineHeight,
                            selectedPeer,
                            runtime.GetSelectedPeerMessages(),
                            runtime);
    drawRuntimeComposer(ui,
                        layout.chatX,
                        composerY,
                        layout.chatWidth,
                        runtime,
                        selectedPeer);

    if (layout.showDetails) {
        drawRuntimeDetails(ui,
                           layout.detailX,
                           layout.detailWidth,
                           layout.contentHeight,
                           selectedPeer);
    }

    drawImagePreviewOverlay(ui, layout.width, layout.height);
}

} // namespace

namespace app {

const DslAppConfig& dslAppConfig()
{
    ensureAppStorage();
    static const DslAppConfig config = DslAppConfig{}
        .title("RelayDesk")
        .pageId("relaydesk")
        .clearColor(kWindowBackground)
        .iconPath("assets/icon.png")
        .textFont("C:/Windows/Fonts/msyh.ttc")
        .iconFont("C:/Windows/Fonts/segmdl2.ttf")
        .windowSize(1940, 1224)
        .showDebugStatsInTitle(false)
        .fps(90.0);
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen)
{
    auto& runtime = relaydesk::runtime::getRelayDeskRuntime();

    ui.stack("root")
        .size(screen.width, screen.height)
        .clip()
        .content([&] {
            rect(ui, "root.bg", 0.0f, 0.0f, screen.width, screen.height,
                 kWindowBackground);
            drawRelayDesk(ui, screen, runtime);
        })
        .build();
}

} // namespace app
