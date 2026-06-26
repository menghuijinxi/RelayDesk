#include "eui_neo.h"

#include "core/app_version.h"
#include "core/render/text.h"
#include "core/platform/platform.h"
#include "core/time.h"
#include "core/uuid.h"
#include "main/image_attachment_store.h"
#include "main/app_runtime.h"
#include "platform/attachment_input.h"
#include "platform/startup_launch.h"
#include "platform/text_encoding.h"
#include "storage/app_paths.h"
#include "storage/sticker_store.h"
#include "storage/ui_preferences.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#endif

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
constexpr Color kSelectionFill{0.000f, 0.590f, 0.590f, 0.18f};
constexpr Color kLinkText{0.000f, 0.440f, 0.720f, 1.0f};
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
constexpr float kChatLoadMoreTopThreshold = 36.0f;
constexpr float kComposerHeight = 196.0f;
constexpr float kMessageBubblePadding = 12.0f;
constexpr float kMessageTimestampWidth = 42.0f;
constexpr float kMessageTimestampGap = 8.0f;
constexpr float kFailedDeliveryStateHeight = 20.0f;
constexpr float kFailedDeliveryStateGap = 8.0f;
constexpr float kFailedDeliveryStateTextWidth = 72.0f;
constexpr float kFailedDeliveryStateRetryGap = 6.0f;
constexpr float kFailedDeliveryStateRetryButtonSize = 20.0f;
constexpr float kFailedDeliveryStateRightInset = 4.0f;
constexpr float kTransferActionButtonHeight = 22.0f;
constexpr float kFileDocumentCardDefaultHeight = 58.0f;
constexpr float kFileDocumentCardCompactHeight = 48.0f;
constexpr float kFileDocumentCardTitleLineHeight = 22.0f;
constexpr float kFileDocumentCardCompactTitleLineHeight = 21.0f;
constexpr std::size_t kFileDocumentCardMaxTitleLines = 3;
constexpr float kMessageFileProgressGap = 6.0f;
constexpr float kMessageFileProgressTrackHeight = 6.0f;
constexpr float kMessageFileProgressDetailGap = 5.0f;
constexpr float kMessageFileProgressDetailHeight = 18.0f;
constexpr float kMessageFileProgressBlockHeight =
    kMessageFileProgressGap
    + kMessageFileProgressTrackHeight
    + kMessageFileProgressDetailGap
    + kMessageFileProgressDetailHeight;
constexpr float kMessageFileTransferActionGap = 4.0f;
constexpr float kMessageFileTransferActionBottomPadding = 4.0f;
constexpr const char* kFileTypeIconAssetDirectory =
    "assets/third_party/vscode-icons/icons";
constexpr std::size_t kMaxRecentEmojiCount = 10;
constexpr std::size_t kMaxPendingAttachmentCount = 8;
constexpr std::chrono::seconds kScreenClipImportTimeout{90};

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
    int unreadCount;
};

struct TransferPreview {
    const char* fileName;
    const char* direction;
    const char* detail;
    float progress;
    Color accent;
};

struct SettingsCategoryItem {
    const char* title = "";
    const char* subtitle = "";
    int icon = 0;
};

constexpr std::array<SettingsCategoryItem, 6> kSettingsCategories = {{
    {"个人资料", "用户名与头像", 0xE77B},
    {"外观", "字体大小", 0xE8D2},
    {"发送", "回车发送方式", 0xE724},
    {"截图", "自定义快捷键", 0xE722},
    {"通知", "提示音与自启", 0xE7F4},
    {"更新", "检查新版本", 0xE895},
}};

struct FileTypeIconStyle {
    std::string iconFileName;
    std::string fallbackLabel;
    Color fallbackBackground;
    Color fallbackForeground;
    bool generic = false;
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
    Folder,
};

struct PendingAttachmentItem {
    PendingAttachmentKind kind = PendingAttachmentKind::File;
    std::string displayName;
    std::string localPath;
    std::string previewPath;
    std::string sha256;
    std::filesystem::path sourcePath;
    std::uintmax_t fileSize = 0;
    unsigned int imagePixelWidth = 0;
    unsigned int imagePixelHeight = 0;
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
                          bool compact,
                          bool wrapTitle = true,
                          bool drawBackground = true,
                          Color detailColor = kMutedText);
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
    bool showPeers;
};

AppLayout makeLayout(const eui::Screen& screen)
{
    AppLayout layout{};
    layout.width = std::max(screen.width, 640.0f);
    layout.height = std::max(screen.height, 520.0f);
    layout.contentHeight = std::max(1.0f, layout.height - kContentTop);
    layout.showPeers = layout.width >= 720.0f;
    layout.peerX = 0.0f;
    layout.peerWidth = layout.showPeers
        ? std::clamp(layout.width * 0.28f, 310.0f, 420.0f)
        : 0.0f;
    layout.chatX = layout.peerWidth + 1.0f;
    layout.chatWidth = layout.width - layout.chatX;

    if (layout.showPeers && layout.chatWidth < 360.0f) {
        layout.showPeers = false;
        layout.peerWidth = 0.0f;
        layout.chatX = 0.0f;
        layout.chatWidth = layout.width;
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
                   eui::HorizontalAlign align = eui::HorizontalAlign::Left,
                   bool wrap = false)
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
        .wrap(wrap)
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

bool isStandaloneEmojiCodepointText(const std::string& value)
{
    const unsigned int codepoint = utf8CodepointValue(value);
    if (codepoint == 0x200Du || codepoint == 0x20E3u
        || (codepoint >= 0xFE00u && codepoint <= 0xFE0Fu)) {
        return false;
    }
    return isEmojiCodepointText(value);
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

components::InputStyle settingsInputStyle()
{
    components::InputStyle style;
    style.background = {1.0f, 1.0f, 1.0f, 1.0f};
    style.hover = {1.0f, 1.0f, 1.0f, 1.0f};
    style.focused = {0.985f, 1.0f, 0.998f, 1.0f};
    style.pressed = style.focused;
    style.border = kBorder;
    style.focusBorder = kTeal;
    style.text = kText;
    style.placeholder = kSubtleText;
    style.cursor = kTeal;
    style.shadow = {};
    style.radius = 7.0f;
    return style;
}

components::DropdownStyle settingsDropdownStyle()
{
    components::DropdownStyle style;
    style.field = {1.0f, 1.0f, 1.0f, 1.0f};
    style.fieldHover = {0.985f, 1.0f, 0.998f, 1.0f};
    style.fieldPressed = {0.950f, 0.985f, 0.980f, 1.0f};
    style.popup = {1.0f, 1.0f, 1.0f, 1.0f};
    style.optionHover = kTealSoft;
    style.optionPressed = {0.790f, 0.940f, 0.930f, 1.0f};
    style.selected = {0.790f, 0.940f, 0.930f, 1.0f};
    style.text = kText;
    style.mutedText = kSubtleText;
    style.accent = kTeal;
    style.border = kBorder;
    style.shadow = {true, {0.0f, 5.0f}, 12.0f, 0.0f,
                    {0.0f, 0.0f, 0.0f, 0.12f}, false};
    style.radius = 7.0f;
    return style;
}

components::ButtonStyle settingsButtonStyle(bool primary)
{
    components::ButtonStyle style;
    if (primary) {
        style.normal = kTeal;
        style.hover = {0.000f, 0.670f, 0.660f, 1.0f};
        style.pressed = {0.000f, 0.500f, 0.500f, 1.0f};
        style.text = {1.0f, 1.0f, 1.0f, 1.0f};
        style.icon = style.text;
        style.border = {1.0f, style.normal};
    } else {
        style.normal = {1.0f, 1.0f, 1.0f, 1.0f};
        style.hover = {0.985f, 1.0f, 0.998f, 1.0f};
        style.pressed = {0.950f, 0.985f, 0.980f, 1.0f};
        style.text = kText;
        style.icon = kTeal;
        style.border = {1.0f, kBorder};
    }
    style.shadow = {};
    style.radius = 7.0f;
    style.pressScale = 0.985f;
    return style;
}

components::SwitchStyle settingsSwitchStyle()
{
    components::SwitchStyle style;
    style.off = {0.820f, 0.850f, 0.870f, 1.0f};
    style.on = kTeal;
    style.knob = {1.0f, 1.0f, 1.0f, 1.0f};
    style.text = kText;
    style.rowHover = {0.900f, 0.955f, 0.950f, 1.0f};
    style.rowPressed = {0.820f, 0.925f, 0.920f, 1.0f};
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

bool loadStoredLaunchAtStartupEnabled()
{
    try {
        const auto paths = relaydesk::storage::createAppPaths();
        return relaydesk::storage::loadLaunchAtStartupEnabled(paths);
    } catch (const std::exception&) {
        return true;
    }
}

bool saveStoredLaunchAtStartupEnabled(bool enabled)
{
    try {
        const auto paths = relaydesk::storage::createAppPaths();
        relaydesk::storage::saveLaunchAtStartupEnabled(paths, enabled);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool applyLaunchAtStartupEnabled(bool enabled)
{
    try {
        const auto paths = relaydesk::storage::createAppPaths();
        relaydesk::platform::setStartupLaunchEnabled(
            paths.GetExecutablePath(),
            enabled);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void syncLaunchAtStartupOnAppStart()
{
    static bool synced = false;
    if (synced) {
        return;
    }

    applyLaunchAtStartupEnabled(loadStoredLaunchAtStartupEnabled());
    synced = true;
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

bool isControlKeyDown()
{
#if defined(_WIN32)
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0
        || (GetKeyState(VK_LCONTROL) & 0x8000) != 0
        || (GetKeyState(VK_RCONTROL) & 0x8000) != 0;
#else
    return false;
#endif
}

std::filesystem::path resolveWorkRelativePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& relativePath)
{
    std::filesystem::path filePath = filesystemPathFromUtf8String(relativePath);
    if (filePath.is_relative()) {
        filePath = appPaths.GetWorkDirectory() / filePath;
    }
    return filePath.lexically_normal();
}

std::optional<std::filesystem::path> findRelativePathFromAncestor(
    const std::filesystem::path& startDirectory,
    const std::filesystem::path& relativePath)
{
    if (startDirectory.empty() || relativePath.empty() || relativePath.is_absolute()) {
        return std::nullopt;
    }

    std::error_code error;
    std::filesystem::path directory =
        std::filesystem::absolute(startDirectory, error);
    if (error) {
        directory = startDirectory;
        error.clear();
    }

    while (!directory.empty()) {
        const std::filesystem::path candidate = directory / relativePath;
        if (std::filesystem::exists(candidate, error) && !error) {
            const std::filesystem::path absolutePath =
                std::filesystem::absolute(candidate, error);
            return error ? candidate.lexically_normal()
                         : absolutePath.lexically_normal();
        }
        error.clear();

        const std::filesystem::path parent = directory.parent_path();
        if (parent.empty() || parent == directory) {
            break;
        }
        directory = parent;
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> findBundledAssetPath(
    const std::filesystem::path& relativePath)
{
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        if (const auto path =
                findRelativePathFromAncestor(appPaths.GetWorkDirectory(), relativePath)) {
            return path;
        }
    } catch (const std::exception&) {
    }

    std::error_code error;
    const std::filesystem::path currentDirectory =
        std::filesystem::current_path(error);
    if (!error) {
        return findRelativePathFromAncestor(currentDirectory, relativePath);
    }

    return std::nullopt;
}

std::string readTextFile(const std::filesystem::path& filePath)
{
    std::ifstream input(filePath, std::ios::binary);
    if (!input.good()) {
        return {};
    }

    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

#if defined(_WIN32)
std::string makeFileTypeIconResourceName(const std::string& iconFileName)
{
    std::string resourceName = "VSICON_";
    resourceName.reserve(resourceName.size() + iconFileName.size());
    for (const unsigned char character : iconFileName) {
        if (std::isalnum(character)) {
            resourceName.push_back(
                static_cast<char>(std::toupper(character)));
        } else {
            resourceName.push_back('_');
        }
    }
    return resourceName;
}

std::string readTextResource(const std::string& resourceName)
{
    const std::wstring wideResourceName =
        relaydesk::platform::utf8ToWide(resourceName);
    const HRSRC resource = FindResourceW(nullptr,
                                         wideResourceName.c_str(),
                                         RT_RCDATA);
    if (!resource) {
        return {};
    }

    const HGLOBAL loadedResource = LoadResource(nullptr, resource);
    if (!loadedResource) {
        return {};
    }

    const DWORD resourceSize = SizeofResource(nullptr, resource);
    const void* resourceData = LockResource(loadedResource);
    if (!resourceData || resourceSize == 0) {
        return {};
    }

    const auto* resourceText = static_cast<const char*>(resourceData);
    return std::string(resourceText, resourceText + resourceSize);
}
#endif

bool shellOpenPath(const std::filesystem::path& filePath)
{
#if defined(_WIN32)
    const HINSTANCE result = ShellExecuteW(nullptr,
                                           L"open",
                                           filePath.wstring().c_str(),
                                           nullptr,
                                           nullptr,
                                           SW_SHOWNORMAL);
    return reinterpret_cast<std::intptr_t>(result) > 32;
#else
    (void)filePath;
    return false;
#endif
}

#if defined(_WIN32)
std::wstring quoteWindowsShellArgument(const std::wstring& argument)
{
    return L"\"" + argument + L"\"";
}
#endif

bool shellRevealPath(const std::filesystem::path& filePath)
{
#if defined(_WIN32)
    std::error_code error;
    if (std::filesystem::is_regular_file(filePath, error)
        || std::filesystem::is_directory(filePath, error)) {
        const std::wstring parameters =
            L"/select," + quoteWindowsShellArgument(filePath.wstring());
        const HINSTANCE result = ShellExecuteW(nullptr,
                                               L"open",
                                               L"explorer.exe",
                                               parameters.c_str(),
                                               nullptr,
                                               SW_SHOWNORMAL);
        return reinterpret_cast<std::intptr_t>(result) > 32;
    }

    const std::filesystem::path directoryPath =
        std::filesystem::is_directory(filePath, error)
        ? filePath
        : filePath.parent_path();
    if (directoryPath.empty()) {
        return false;
    }
    return shellOpenPath(directoryPath);
#else
    (void)filePath;
    return false;
#endif
}

std::string fileTypeIconSvgMarkup(const std::string& iconFileName)
{
    static std::unordered_map<std::string, std::string> svgMarkupCache;
    if (iconFileName.empty()) {
        return {};
    }

    const auto cached = svgMarkupCache.find(iconFileName);
    if (cached != svgMarkupCache.end()) {
        return cached->second;
    }

    std::string markup;
#if defined(_WIN32)
    markup = readTextResource(makeFileTypeIconResourceName(iconFileName));
#endif
    if (!markup.empty()) {
        svgMarkupCache[iconFileName] = markup;
        return markup;
    }

    const auto path = findBundledAssetPath(
        std::filesystem::path(kFileTypeIconAssetDirectory) / iconFileName);
    if (path) {
        markup = readTextFile(*path);
    }

    svgMarkupCache[iconFileName] = markup;
    return markup;
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

bool isPathUnderDirectory(const std::filesystem::path& filePath,
                          const std::filesystem::path& directory)
{
    std::error_code error;
    const std::filesystem::path relativePath =
        std::filesystem::relative(filePath, directory, error);
    return !error
        && !relativePath.empty()
        && !relativePath.is_absolute()
        && !isParentTraversalPath(relativePath);
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

bool shouldMoveComposerImageSource(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& sourcePath)
{
    const std::string fileName = lowerAscii(sourcePath.filename().string());
    const bool generatedOutboxImage =
        fileName == "clipboard.bmp" || fileName == "screenshot.bmp";
    return generatedOutboxImage
        && isPathUnderDirectory(sourcePath, appPaths.GetOutboxDirectory());
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

std::string formatTransferRate(double bytesPerSecond)
{
    if (bytesPerSecond <= 0.0) {
        return "计算速度中";
    }

    return formatFileSize(static_cast<std::uintmax_t>(bytesPerSecond)) + "/s";
}

std::string formatRemainingTime(double seconds)
{
    if (seconds <= 1.0) {
        return "剩余 1 秒";
    }

    const int roundedSeconds = static_cast<int>(std::ceil(seconds));
    if (roundedSeconds < 60) {
        return "剩余 " + std::to_string(roundedSeconds) + " 秒";
    }

    const int minutes = roundedSeconds / 60;
    const int secondsPart = roundedSeconds % 60;
    if (secondsPart == 0) {
        return "剩余 " + std::to_string(minutes) + " 分钟";
    }
    return "剩余 " + std::to_string(minutes) + " 分 "
        + std::to_string(secondsPart) + " 秒";
}

std::string formatDurationClock(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }

    const int roundedSeconds = static_cast<int>(std::ceil(seconds));
    const int hours = roundedSeconds / 3600;
    const int minutes = (roundedSeconds / 60) % 60;
    const int secondsPart = roundedSeconds % 60;

    std::ostringstream output;
    output << std::setfill('0');
    output << std::setw(2) << hours << ':'
           << std::setw(2) << minutes << ':'
           << std::setw(2) << secondsPart;
    return output.str();
}

std::string formatMessageTimestamp(const std::string& timestamp)
{
    try {
        const std::chrono::system_clock::time_point timePoint =
            relaydesk::core::parseUtcTimestamp(timestamp);
        const std::time_t rawTime =
            std::chrono::system_clock::to_time_t(timePoint);
        std::tm localTime{};
#if defined(_WIN32)
        if (localtime_s(&localTime, &rawTime) != 0) {
            return {};
        }
#else
        if (localtime_r(&rawTime, &localTime) == nullptr) {
            return {};
        }
#endif
        std::ostringstream output;
        output << std::put_time(&localTime, "%H:%M");
        return output.str();
    } catch (const std::exception&) {
        if (timestamp.size() >= 16u && timestamp[13] == ':') {
            return timestamp.substr(11u, 5u);
        }
    }

    return {};
}

std::string appUpdateDownloadDetail(
    const relaydesk::runtime::AppUpdatePrompt& prompt)
{
    const std::uintmax_t receivedSize = prompt.GetReceivedSize();
    const std::uintmax_t expectedSize = prompt.GetExpectedSize();
    std::string detail = formatTransferRate(prompt.GetBytesPerSecond());
    if (expectedSize > 0) {
        detail += " · " + formatFileSize(receivedSize) + "/"
            + formatFileSize(expectedSize);
        const double bytesPerSecond = prompt.GetBytesPerSecond();
        if (prompt.GetState() == relaydesk::runtime::AppUpdatePromptState::Downloading
            && bytesPerSecond > 0.0
            && receivedSize < expectedSize) {
            const double remainingSeconds =
                static_cast<double>(expectedSize - receivedSize) / bytesPerSecond;
            detail += "，" + formatRemainingTime(remainingSeconds);
        }
    } else {
        detail += " · 正在准备更新包";
    }
    return detail;
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

std::string fileIconExtensionKey(const std::string& fileName)
{
    const std::filesystem::path filePath(fileName);
    std::string baseName = filePath.filename().string();
    if (baseName.empty()) {
        baseName = fileName;
    }

    const std::string lowerBaseName = lowerAscii(baseName);
    if (lowerBaseName == ".gitattributes" || lowerBaseName == ".gitignore") {
        return "git";
    }
    if (lowerBaseName == "cmakelists.txt") {
        return "cmake";
    }

    std::string extension = lowerAscii(std::filesystem::path(baseName).extension().string());
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }
    return extension.empty() ? "file" : extension;
}

template <std::size_t Size>
bool matchesFileExtension(const std::string& extension,
                          const std::array<const char*, Size>& candidates)
{
    return std::find(candidates.begin(), candidates.end(), extension)
        != candidates.end();
}

FileTypeIconStyle makeIconStyle(const char* iconFileName,
                                std::string fallbackLabel,
                                Color fallbackBackground,
                                Color fallbackForeground,
                                bool generic = false)
{
    return {
        iconFileName,
        std::move(fallbackLabel),
        fallbackBackground,
        fallbackForeground,
        generic
    };
}

FileTypeIconStyle makeFileTypeIconStyle(const std::string& fileName)
{
    const std::string extension = fileIconExtensionKey(fileName);
    if (matchesFileExtension(extension, std::array{"git"})) {
        return makeIconStyle("file_type_git.svg",
                             "GIT",
                             {0.950f, 0.300f, 0.120f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "sln") {
        return makeIconStyle("file_type_sln.svg",
                             "VS",
                             {0.485f, 0.265f, 0.835f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "vcxproj") {
        return makeIconStyle("file_type_vcxproj.svg",
                             "VC",
                             {0.485f, 0.265f, 0.835f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "csproj") {
        return makeIconStyle("file_type_csproj.svg",
                             "CS",
                             {0.485f, 0.265f, 0.835f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "fsproj") {
        return makeIconStyle("file_type_fsproj.svg",
                             "FS",
                             {0.485f, 0.265f, 0.835f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"uproject", "uasset", "umap"})) {
        return makeIconStyle("default_file.svg",
                             "UE",
                             {0.145f, 0.155f, 0.175f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f},
                             true);
    }
    if (matchesFileExtension(extension, std::array{"exe", "msi", "app", "dmg"})) {
        return makeIconStyle("file_type_binary.svg",
                             "EXE",
                             {0.090f, 0.445f, 0.790f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"bat", "cmd"})) {
        return makeIconStyle("file_type_bat.svg",
                             "BAT",
                             {0.120f, 0.545f, 0.545f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"ps1", "psm1", "psd1"})) {
        return makeIconStyle("file_type_powershell.svg",
                             "PS",
                             {0.120f, 0.545f, 0.545f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"sh", "bash", "zsh"})) {
        return makeIconStyle("file_type_shell.svg",
                             "SH",
                             {0.120f, 0.545f, 0.545f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "jar") {
        return makeIconStyle("file_type_jar.svg",
                             "JAR",
                             {0.120f, 0.545f, 0.545f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "lnk") {
        return makeIconStyle("file_type_lnk.svg",
                             "LNK",
                             {0.120f, 0.545f, 0.545f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension,
                             std::array{"mp4", "mov", "webm", "avi", "mkv", "wmv"})) {
        return makeIconStyle("file_type_video.svg",
                             "VID",
                             {0.805f, 0.190f, 0.420f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension,
                             std::array{"mp3", "wav", "flac", "aac", "ogg"})) {
        return makeIconStyle("file_type_audio.svg",
                             "AUD",
                             {0.650f, 0.250f, 0.760f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "svg") {
        return makeIconStyle("file_type_svg.svg",
                             "SVG",
                             {0.120f, 0.615f, 0.390f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "avif") {
        return makeIconStyle("file_type_avif.svg",
                             "AVIF",
                             {0.120f, 0.615f, 0.390f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension,
                             std::array{"png", "jpg", "jpeg", "gif", "webp", "bmp"})) {
        return makeIconStyle("file_type_image.svg",
                             "IMG",
                             {0.120f, 0.615f, 0.390f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension,
                             std::array{"zip", "rar", "7z", "tar", "gz", "bz2"})) {
        return makeIconStyle("file_type_zip.svg",
                             "ZIP",
                             {0.820f, 0.520f, 0.080f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"pdf"})) {
        return makeIconStyle("file_type_pdf.svg",
                             "PDF",
                             {0.855f, 0.180f, 0.160f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension,
                             std::array{"doc", "docx", "rtf", "odt"})) {
        return makeIconStyle("file_type_word.svg",
                             "DOC",
                             {0.160f, 0.390f, 0.820f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension,
                             std::array{"xls", "xlsx", "csv", "ods"})) {
        return makeIconStyle("file_type_excel.svg",
                             "XLS",
                             {0.130f, 0.570f, 0.320f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension,
                             std::array{"ppt", "pptx", "odp"})) {
        return makeIconStyle("file_type_powerpoint.svg",
                             "PPT",
                             {0.870f, 0.365f, 0.130f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"md", "markdown"})) {
        return makeIconStyle("file_type_markdown.svg",
                             "MD",
                             {0.385f, 0.475f, 0.575f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "log") {
        return makeIconStyle("file_type_log.svg",
                             "LOG",
                             {0.385f, 0.475f, 0.575f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"txt", "text"})) {
        return makeIconStyle("file_type_text.svg",
                             "TXT",
                             {0.385f, 0.475f, 0.575f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"cpp", "cxx", "cc"})) {
        return makeIconStyle("file_type_cpp.svg",
                             "C++",
                             {0.205f, 0.345f, 0.780f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "c") {
        return makeIconStyle("file_type_c.svg",
                             "C",
                             {0.205f, 0.345f, 0.780f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "h") {
        return makeIconStyle("file_type_cheader.svg",
                             "H",
                             {0.205f, 0.345f, 0.780f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"hpp", "hh", "hxx"})) {
        return makeIconStyle("file_type_cppheader.svg",
                             "H++",
                             {0.205f, 0.345f, 0.780f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "cs") {
        return makeIconStyle("file_type_csharp.svg",
                             "CS",
                             {0.245f, 0.455f, 0.765f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "java") {
        return makeIconStyle("file_type_java.svg",
                             "JAVA",
                             {0.245f, 0.455f, 0.765f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "py") {
        return makeIconStyle("file_type_python.svg",
                             "PY",
                             {0.245f, 0.455f, 0.765f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "go") {
        return makeIconStyle("file_type_go.svg",
                             "GO",
                             {0.245f, 0.455f, 0.765f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "rs") {
        return makeIconStyle("file_type_rust.svg",
                             "RS",
                             {0.245f, 0.455f, 0.765f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "swift") {
        return makeIconStyle("file_type_swift.svg",
                             "SWIFT",
                             {0.245f, 0.455f, 0.765f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "php") {
        return makeIconStyle("file_type_php.svg",
                             "PHP",
                             {0.245f, 0.455f, 0.765f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"rb", "ruby"})) {
        return makeIconStyle("file_type_ruby.svg",
                             "RB",
                             {0.245f, 0.455f, 0.765f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "jsx") {
        return makeIconStyle("file_type_reactjs.svg",
                             "JSX",
                             {0.865f, 0.690f, 0.080f, 1.0f},
                             {0.120f, 0.105f, 0.070f, 1.0f});
    }
    if (extension == "tsx") {
        return makeIconStyle("file_type_reactts.svg",
                             "TSX",
                             {0.865f, 0.690f, 0.080f, 1.0f},
                             {0.120f, 0.105f, 0.070f, 1.0f});
    }
    if (extension == "js") {
        return makeIconStyle("file_type_js.svg",
                             "JS",
                             {0.865f, 0.690f, 0.080f, 1.0f},
                             {0.120f, 0.105f, 0.070f, 1.0f});
    }
    if (extension == "ts") {
        return makeIconStyle("file_type_typescript.svg",
                             "TS",
                             {0.865f, 0.690f, 0.080f, 1.0f},
                             {0.120f, 0.105f, 0.070f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"html", "htm"})) {
        return makeIconStyle("file_type_html.svg",
                             "HTML",
                             {0.890f, 0.390f, 0.120f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "css") {
        return makeIconStyle("file_type_css.svg",
                             "CSS",
                             {0.890f, 0.390f, 0.120f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"scss", "sass"})) {
        return makeIconStyle("file_type_scss.svg",
                             "SCSS",
                             {0.890f, 0.390f, 0.120f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "json") {
        return makeIconStyle("file_type_json.svg",
                             "JSON",
                             {0.430f, 0.455f, 0.510f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "xml") {
        return makeIconStyle("file_type_xml.svg",
                             "XML",
                             {0.430f, 0.455f, 0.510f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"yaml", "yml"})) {
        return makeIconStyle("file_type_yaml.svg",
                             "YML",
                             {0.430f, 0.455f, 0.510f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "toml") {
        return makeIconStyle("file_type_toml.svg",
                             "TOML",
                             {0.430f, 0.455f, 0.510f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "ini") {
        return makeIconStyle("file_type_ini.svg",
                             "INI",
                             {0.430f, 0.455f, 0.510f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"cfg", "conf", "config"})) {
        return makeIconStyle("file_type_config.svg",
                             "CFG",
                             {0.430f, 0.455f, 0.510f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension, std::array{"cmake", "mk", "make"})) {
        return makeIconStyle("file_type_cmake.svg",
                             "MAKE",
                             {0.210f, 0.495f, 0.545f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "sql") {
        return makeIconStyle("file_type_sql.svg",
                             "SQL",
                             {0.380f, 0.470f, 0.790f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "sqlite") {
        return makeIconStyle("file_type_sqlite.svg",
                             "DB",
                             {0.380f, 0.470f, 0.790f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (extension == "db") {
        return makeIconStyle("file_type_db.svg",
                             "DB",
                             {0.380f, 0.470f, 0.790f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }
    if (matchesFileExtension(extension,
                             std::array{"ttf", "otf", "woff", "woff2"})) {
        return makeIconStyle("file_type_font.svg",
                             "FNT",
                             {0.420f, 0.365f, 0.700f, 1.0f},
                             {1.0f, 1.0f, 1.0f, 1.0f});
    }

    return makeIconStyle("default_file.svg",
                         fileTypeTag(fileName),
                         {0.925f, 0.935f, 0.950f, 1.0f},
                         kMutedText,
                         true);
}

float fileIconLabelFontSize(const std::string& label, float iconSize)
{
    if (label.size() <= 2u) {
        return iconSize * 0.330f;
    }
    if (label.size() == 3u) {
        return iconSize * 0.285f;
    }
    return iconSize * 0.225f;
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

std::vector<std::string> splitUtf8Codepoints(const std::string& value);

float estimateParagraphHeight(const std::string& value,
                              float width,
                              float fontSize,
                              float lineHeight)
{
    const std::size_t lineCount =
        estimateWrappedLineCount(value, width, fontSize);
    return std::max(lineHeight, static_cast<float>(lineCount) * lineHeight);
}

float fileDocumentCardTitleFontSize(bool compact)
{
    return compact ? 13.0f : 14.0f;
}

float fileDocumentCardTitleLineHeight(bool compact)
{
    return compact ? kFileDocumentCardCompactTitleLineHeight
                   : kFileDocumentCardTitleLineHeight;
}

float fileDocumentCardTitleWidth(float cardWidth, bool compact)
{
    const float iconSize = compact ? 34.0f : 40.0f;
    const float iconX = 9.0f;
    const float textX = iconX + iconSize + 10.0f;
    return std::max(40.0f, cardWidth - textX - 12.0f);
}

std::size_t textLineCount(const std::string& value)
{
    if (value.empty()) {
        return 1u;
    }
    return static_cast<std::size_t>(std::count(value.begin(), value.end(), '\n'))
        + 1u;
}

std::vector<std::string> wrapTextToMeasuredLines(const std::string& value,
                                                 float width,
                                                 float fontSize,
                                                 std::size_t maxLines)
{
    std::vector<std::string> lines;
    if (value.empty() || maxLines == 0u) {
        return lines;
    }

    std::string currentLine;
    for (const std::string& codepoint : splitUtf8Codepoints(value)) {
        if (codepoint == "\n") {
            lines.push_back(currentLine);
            currentLine.clear();
            if (lines.size() >= maxLines) {
                break;
            }
            continue;
        }

        std::string candidate = currentLine + codepoint;
        const float candidateWidth =
            core::TextPrimitive::measureTextWidth(candidate, "", fontSize);
        if (!currentLine.empty()
            && std::isfinite(candidateWidth)
            && candidateWidth > width
            && lines.size() + 1u < maxLines) {
            lines.push_back(currentLine);
            currentLine = codepoint;
            continue;
        }

        currentLine = std::move(candidate);
    }

    if (lines.size() < maxLines && !currentLine.empty()) {
        lines.push_back(currentLine);
    }
    return lines;
}

std::string fitTextToMeasuredWidth(std::string value, float width, float fontSize)
{
    constexpr const char* suffix = "...";
    const float suffixWidth =
        core::TextPrimitive::measureTextWidth(suffix, "", fontSize);
    const float maxTextWidth = std::max(0.0f, width - suffixWidth);

    while (!value.empty()) {
        const float measuredWidth =
            core::TextPrimitive::measureTextWidth(value, "", fontSize);
        if (std::isfinite(measuredWidth) && measuredWidth <= maxTextWidth) {
            break;
        }
        const auto codepoints = splitUtf8Codepoints(value);
        if (codepoints.empty()) {
            value.clear();
            break;
        }
        value.resize(value.size() - codepoints.back().size());
    }

    return value + suffix;
}

std::string wrappedFileDocumentCardTitle(const std::string& title,
                                         float cardWidth,
                                         bool compact)
{
    const float titleWidth = fileDocumentCardTitleWidth(cardWidth, compact);
    const float fontSize = fileDocumentCardTitleFontSize(compact);
    std::vector<std::string> lines =
        wrapTextToMeasuredLines(title,
                                titleWidth,
                                fontSize,
                                kFileDocumentCardMaxTitleLines);
    if (lines.empty()) {
        return title;
    }

    std::size_t renderedCodepoints = 0;
    for (const std::string& line : lines) {
        renderedCodepoints += utf8CodepointCount(line);
    }
    const bool truncated = renderedCodepoints < utf8CodepointCount(title);
    if (truncated) {
        lines.back() =
            fitTextToMeasuredWidth(lines.back(), titleWidth, fontSize);
    }

    std::ostringstream titleText;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0u) {
            titleText << '\n';
        }
        titleText << lines[index];
    }
    return titleText.str();
}

float fileDocumentCardHeight(float cardWidth,
                             const std::string& title,
                             bool compact,
                             bool actions = false)
{
    const std::string wrappedTitle =
        wrappedFileDocumentCardTitle(title, cardWidth, compact);
    const std::size_t lineCount = textLineCount(wrappedTitle);
    const float baseHeight = compact ? kFileDocumentCardCompactHeight
                                     : kFileDocumentCardDefaultHeight;
    const float extraTitleHeight =
        static_cast<float>(lineCount - 1u)
        * fileDocumentCardTitleLineHeight(compact);
    const float cardHeight = baseHeight + extraTitleHeight;
    if (!actions) {
        return cardHeight;
    }

    return cardHeight
        + kMessageFileTransferActionGap
        + kTransferActionButtonHeight
        + kMessageFileTransferActionBottomPadding;
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

std::uintmax_t directoryContentSizeOrZero(const std::filesystem::path& directory)
{
    std::uintmax_t totalSize = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        std::error_code entryError;
        if (iterator->is_regular_file(entryError) && !entryError) {
            const std::uintmax_t fileSize =
                std::filesystem::file_size(iterator->path(), entryError);
            if (!entryError) {
                totalSize += fileSize;
            }
        }
        iterator.increment(error);
    }
    return totalSize;
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

struct ComposerImageStage {
    std::filesystem::path sourcePath;
    std::filesystem::path previewPath;
    std::string sha256;
};

ComposerImageStage stageComposerImageFiles(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& sourcePath)
{
    const std::string displayName =
        filesystemPathToUtf8String(sourcePath.filename());
    const bool removeSource =
        shouldMoveComposerImageSource(appPaths, sourcePath);
    const std::optional<relaydesk::runtime::StoredImageAttachment> storedImage =
        relaydesk::runtime::storePreviewableImageAttachment(appPaths,
                                                            sourcePath,
                                                            displayName,
                                                            removeSource);
    if (!storedImage.has_value()) {
        throw std::runtime_error("Image cannot be previewed.");
    }

    ComposerImageStage stage;
    stage.sourcePath = storedImage->GetImagePath();
    stage.previewPath = storedImage->GetThumbnailPath().value_or(stage.sourcePath);
    stage.sha256 = storedImage->GetSha256();
    return stage;
}

std::optional<PendingAttachmentItem> makePendingAttachmentFromPath(
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

    const auto appPaths = relaydesk::storage::createAppPaths();
    relaydesk::storage::ensureAppDirectories(appPaths);
    const std::filesystem::path absolutePath = makeAbsolutePath(filePath);
    const bool imageAttachment = regularFile && isImageAttachmentPath(absolutePath);
    std::filesystem::path sourcePath = absolutePath;
    std::filesystem::path previewPath = absolutePath;
    std::string localPath = makeAttachmentLocalPath(appPaths, absolutePath);
    std::string sha256;
    if (imageAttachment) {
        try {
            const ComposerImageStage imageStage =
                stageComposerImageFiles(appPaths, absolutePath);
            sourcePath = imageStage.sourcePath;
            previewPath = imageStage.previewPath;
            localPath = makeAttachmentLocalPath(appPaths, imageStage.sourcePath);
            sha256 = imageStage.sha256;
        } catch (const std::exception&) {
            previewPath = absolutePath;
        }
    }

    PendingAttachmentItem attachment;
    if (directory) {
        attachment.kind = PendingAttachmentKind::Folder;
    } else {
        attachment.kind =
            imageAttachment ? PendingAttachmentKind::Image : PendingAttachmentKind::File;
    }
    attachment.displayName = filesystemPathToUtf8String(absolutePath.filename());
    attachment.localPath = localPath;
    attachment.previewPath = makeAttachmentLocalPath(appPaths, previewPath);
    attachment.sha256 = std::move(sha256);
    attachment.sourcePath = sourcePath;
    attachment.fileSize = directory
        ? directoryContentSizeOrZero(absolutePath)
        : fileSizeOrZero(absolutePath);
    if (imageAttachment) {
        applyImageSizeMetadata(attachment, sourcePath);
    }
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
    attachment.previewPath = sticker.relativePath;
    attachment.sourcePath = imagePath;
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(appPaths);
        const std::optional<relaydesk::runtime::StoredImageAttachment> storedImage =
            relaydesk::runtime::storePreviewableImageAttachment(
                appPaths,
                imagePath,
                attachment.displayName,
                false);
        if (storedImage.has_value()) {
            attachment.localPath =
                makeAttachmentLocalPath(appPaths, storedImage->GetImagePath());
            attachment.previewPath = makeAttachmentLocalPath(
                appPaths,
                storedImage->GetThumbnailPath().value_or(storedImage->GetImagePath()));
            attachment.sourcePath = storedImage->GetImagePath();
            attachment.sha256 = storedImage->GetSha256();
        }
    } catch (const std::exception&) {
    }
    attachment.fileSize = fileSizeOrZero(imagePath);
    applyImageSizeMetadata(attachment, attachment.sourcePath);
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
    std::size_t selectionAnchor = 0;
    float preferredX = 0.0f;
    bool hasPreferredX = false;
    float selectionBoundsX = 0.0f;
    float selectionBoundsY = 0.0f;
    float selectionBoundsWidth = 0.0f;
    float selectionBoundsHeight = 0.0f;
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

std::string utf8SubstringByCodepointRange(const std::string& value,
                                          std::size_t begin,
                                          std::size_t end)
{
    if (begin > end) {
        std::swap(begin, end);
    }
    const std::size_t byteBegin =
        utf8ByteOffsetForCodepointIndex(value, begin);
    const std::size_t byteEnd =
        utf8ByteOffsetForCodepointIndex(value, end);
    return value.substr(byteBegin, byteEnd - byteBegin);
}

struct MessageTextLinkSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string url;
};

std::vector<MessageTextLinkSpan> findMessageTextLinkSpans(
    const std::string& value);

std::string linkUrlForByteOffset(
    const std::vector<MessageTextLinkSpan>& linkSpans,
    std::size_t byteOffset);

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
    const std::size_t documentLength = composerDraftDocumentLength(draftItems);
    caret.position = std::min(caret.position, documentLength);
    caret.selectionAnchor = std::min(caret.selectionAnchor, documentLength);
}

std::pair<std::size_t, std::size_t> composerSelectionRange(
    const ComposerCaretState& caret)
{
    return {
        std::min(caret.selectionAnchor, caret.position),
        std::max(caret.selectionAnchor, caret.position)
    };
}

bool hasComposerSelection(const ComposerCaretState& caret)
{
    return caret.selectionAnchor != caret.position;
}

void clearComposerSelection(ComposerCaretState& caret)
{
    caret.selectionAnchor = caret.position;
}

void updateComposerSelectionAfterMove(ComposerCaretState& caret,
                                      std::size_t previousPosition,
                                      bool keepSelection,
                                      bool hadSelection)
{
    if (keepSelection) {
        if (!hadSelection) {
            caret.selectionAnchor = previousPosition;
        }
        return;
    }

    clearComposerSelection(caret);
}

void moveComposerCaretTo(ComposerCaretState& caret,
                         std::size_t position,
                         bool keepSelection)
{
    const std::size_t previousPosition = caret.position;
    const bool hadSelection = hasComposerSelection(caret);
    caret.position = position;
    caret.hasPreferredX = false;
    updateComposerSelectionAfterMove(caret,
                                     previousPosition,
                                     keepSelection,
                                     hadSelection);
}

std::string composerDraftRangeText(
    const std::vector<ComposerDraftItem>& draftItems,
    std::size_t begin,
    std::size_t end)
{
    if (begin > end) {
        std::swap(begin, end);
    }

    std::string selectedText;
    std::size_t consumedLength = 0;
    for (const ComposerDraftItem& item : draftItems) {
        const std::size_t itemLength = composerDraftItemAtomLength(item);
        const std::size_t itemBegin = consumedLength;
        const std::size_t itemEnd = consumedLength + itemLength;
        if (end <= itemBegin) {
            break;
        }
        if (begin >= itemEnd) {
            consumedLength = itemEnd;
            continue;
        }

        if (item.type == ComposerDraftItemType::Text) {
            const std::size_t overlapBegin = std::max(begin, itemBegin) - itemBegin;
            const std::size_t overlapEnd = std::min(end, itemEnd) - itemBegin;
            const std::size_t byteBegin =
                utf8ByteOffsetForCodepointIndex(item.text, overlapBegin);
            const std::size_t byteEnd =
                utf8ByteOffsetForCodepointIndex(item.text, overlapEnd);
            selectedText.append(item.text.substr(byteBegin, byteEnd - byteBegin));
        } else if (!selectedText.empty() && selectedText.back() != ' ') {
            selectedText.push_back(' ');
        }

        consumedLength = itemEnd;
    }
    return selectedText;
}

bool removeComposerDraftSelection(std::vector<ComposerDraftItem>& draftItems,
                                  ComposerCaretState& caret);

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

    (void)removeComposerDraftSelection(draftItems, caret);
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
                clearComposerSelection(caret);
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
            clearComposerSelection(caret);
            caret.hasPreferredX = false;
            normalizeComposerDraftItems(draftItems);
            return;
        }
        ++consumedLength;
        if (caret.position <= consumedLength) {
            insertComposerDraftTextItemAt(draftItems, index + 1u, std::move(text));
            caret.position += insertedLength;
            clearComposerSelection(caret);
            caret.hasPreferredX = false;
            normalizeComposerDraftItems(draftItems);
            return;
        }
    }

    draftItems.push_back(makeComposerDraftTextItem(std::move(text)));
    caret.position += insertedLength;
    clearComposerSelection(caret);
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

    (void)removeComposerDraftSelection(draftItems, caret);
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
                clearComposerSelection(caret);
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
            clearComposerSelection(caret);
            caret.hasPreferredX = false;
            return;
        }
        ++consumedLength;
        if (caret.position <= consumedLength) {
            draftItems.insert(
                draftItems.begin() + static_cast<std::ptrdiff_t>(index + 1u),
                makeComposerDraftAttachmentItem(std::move(attachment)));
            ++caret.position;
            clearComposerSelection(caret);
            caret.hasPreferredX = false;
            return;
        }
    }

    draftItems.push_back(makeComposerDraftAttachmentItem(std::move(attachment)));
    ++caret.position;
    clearComposerSelection(caret);
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

void pollPendingScreenClipCapture(
    bool& pending,
    std::uint32_t& clipboardSequence,
    std::chrono::steady_clock::time_point& startedAt,
    std::vector<ComposerDraftItem>& draftItems,
    ComposerCaretState& composerCaret)
{
    if (!pending) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (startedAt.time_since_epoch().count() != 0
        && now - startedAt > kScreenClipImportTimeout) {
        pending = false;
        return;
    }

    const std::uint32_t currentSequence =
        relaydesk::platform::getClipboardSequenceNumber();
    if (currentSequence == 0 || currentSequence == clipboardSequence) {
        return;
    }

    clipboardSequence = currentSequence;
    const std::vector<std::filesystem::path> imagePaths =
        relaydesk::platform::collectClipboardImageAttachmentPaths();
    if (imagePaths.empty()) {
        return;
    }

    insertComposerDraftAttachmentPathsAtCaret(
        draftItems,
        composerCaret,
        imagePaths);
    pending = false;
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
        clearComposerSelection(caret);
        caret.hasPreferredX = false;
    }
}

void removeComposerDraftAtomAfterCaret(std::vector<ComposerDraftItem>& draftItems,
                                       ComposerCaretState& caret)
{
    clampComposerCaret(draftItems, caret);
    if (removeComposerDraftAtomAt(draftItems, caret.position)) {
        clearComposerSelection(caret);
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
    clearComposerSelection(caret);
    caret.hasPreferredX = false;
    normalizeComposerDraftItems(draftItems);
    clampComposerCaret(draftItems, caret);
}

bool removeComposerDraftSelection(std::vector<ComposerDraftItem>& draftItems,
                                  ComposerCaretState& caret)
{
    clampComposerCaret(draftItems, caret);
    if (!hasComposerSelection(caret)) {
        return false;
    }

    const auto [begin, end] = composerSelectionRange(caret);
    for (std::size_t position = end; position > begin; --position) {
        (void)removeComposerDraftAtomAt(draftItems, position - 1u);
    }
    caret.position = begin;
    clearComposerSelection(caret);
    caret.hasPreferredX = false;
    normalizeComposerDraftItems(draftItems);
    clampComposerCaret(draftItems, caret);
    return true;
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

std::vector<std::filesystem::path> selectAttachmentFolderFromDialog()
{
    std::vector<std::filesystem::path> folderPaths;
    const std::optional<std::filesystem::path> folderPath =
        relaydesk::platform::selectFolderFromDialog();
    if (folderPath.has_value()) {
        folderPaths.push_back(folderPath.value());
    }
    return folderPaths;
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
        if (localPathText.empty()) {
            localPathText = makeAttachmentLocalPath(appPaths, localPath);
        }
        if (attachment.kind != PendingAttachmentKind::Folder) {
            fileSize = fileSizeOrZero(localPath);
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }

    relaydesk::storage::ChatMessagePart part;
    switch (attachment.kind) {
    case PendingAttachmentKind::Image:
        part.SetType(relaydesk::storage::MessagePartType::Image);
        break;
    case PendingAttachmentKind::File:
        part.SetType(relaydesk::storage::MessagePartType::File);
        break;
    case PendingAttachmentKind::Folder:
        part.SetType(relaydesk::storage::MessagePartType::Folder);
        break;
    }
    part.SetTransferId(relaydesk::core::createUuidV4());
    part.SetTransferState(relaydesk::storage::TransferState::Pending);
    part.SetFileName(attachment.displayName);
    part.SetFileSize(fileSize);
    part.SetTransferredSize(0);
    if (!attachment.sha256.empty()) {
        part.SetSha256(attachment.sha256);
    }
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

struct OpenableMessagePath {
    std::filesystem::path path;
    bool directory = false;
};

std::optional<OpenableMessagePath> resolveOpenableMessagePath(
    const relaydesk::storage::ChatMessagePart& part);

bool shouldShowOpenActionsForTransferState(
    relaydesk::storage::TransferState transferState,
    bool outgoing)
{
    if (outgoing) {
        return transferState == relaydesk::storage::TransferState::Completed
            || transferState == relaydesk::storage::TransferState::Cancelled
            || transferState == relaydesk::storage::TransferState::Rejected;
    }

    return transferState == relaydesk::storage::TransferState::Completed
        || transferState == relaydesk::storage::TransferState::Cancelled;
}

bool shouldShowIncomingTransferAcceptActions(
    relaydesk::storage::TransferState transferState)
{
    return transferState == relaydesk::storage::TransferState::Offered
        || transferState == relaydesk::storage::TransferState::Interrupted;
}

bool messageFilePartHasActions(
    const relaydesk::storage::ChatMessagePart& part,
    bool outgoing)
{
    if (!part.GetTransferState().has_value()) {
        return false;
    }

    const relaydesk::storage::TransferState state =
        part.GetTransferState().value();
    if (shouldShowOpenActionsForTransferState(state, outgoing)) {
        if (!outgoing
            && state == relaydesk::storage::TransferState::Cancelled) {
            return resolveOpenableMessagePath(part).has_value();
        }
        return true;
    }
    if (outgoing) {
        return state == relaydesk::storage::TransferState::Offered
            || state == relaydesk::storage::TransferState::Interrupted
            || state == relaydesk::storage::TransferState::Failed
            || state == relaydesk::storage::TransferState::Transferring;
    }

    return shouldShowIncomingTransferAcceptActions(state)
        || state == relaydesk::storage::TransferState::Transferring;
}

float messageFileNodeWidth(float flowWidth)
{
    if (flowWidth < 420.0f) {
        return flowWidth;
    }
    return std::clamp(flowWidth * 0.72f, 320.0f, flowWidth);
}

std::optional<OpenableMessagePath> resolveOpenableMessagePath(
    const relaydesk::storage::ChatMessagePart& part)
{
    const bool folder = part.GetType() == relaydesk::storage::MessagePartType::Folder;
    if ((part.GetType() != relaydesk::storage::MessagePartType::File && !folder)
        || !part.GetLocalPath().has_value()
        || part.GetLocalPath().value().empty()) {
        return std::nullopt;
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        const std::filesystem::path filePath =
            resolveWorkRelativePath(appPaths, part.GetLocalPath().value());
        std::error_code error;
        if (std::filesystem::is_directory(filePath, error) && !error) {
            return OpenableMessagePath{filePath, true};
        }
        error.clear();
        if (std::filesystem::is_regular_file(filePath, error) && !error) {
            return OpenableMessagePath{filePath, false};
        }
    } catch (const std::exception&) {
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> resolveMessageThumbnailPath(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::storage::ChatMessagePart& part)
{
    if (part.GetType() != relaydesk::storage::MessagePartType::Image
        || !part.GetSha256().has_value()
        || part.GetSha256().value().empty()) {
        return std::nullopt;
    }

    const std::filesystem::path jpgThumbnailPath =
        appPaths.GetImageThumbnailsDirectory()
        / (part.GetSha256().value() + ".jpg");
    std::error_code error;
    if (std::filesystem::is_regular_file(jpgThumbnailPath, error)) {
        return jpgThumbnailPath;
    }

    const std::filesystem::path pngThumbnailPath =
        appPaths.GetImageThumbnailsDirectory()
        / (part.GetSha256().value() + ".png");
    error.clear();
    if (std::filesystem::is_regular_file(pngThumbnailPath, error)) {
        return pngThumbnailPath;
    }

    return std::nullopt;
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

    return "";
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

std::optional<int> transferProgressPercent(
    const relaydesk::storage::ChatMessagePart& part)
{
    if (!part.GetFileSize().has_value()
        || part.GetFileSize().value() == 0
        || !part.GetTransferredSize().has_value()) {
        return std::nullopt;
    }

    const double ratio = static_cast<double>(part.GetTransferredSize().value())
        / static_cast<double>(part.GetFileSize().value());
    return static_cast<int>(std::round(std::clamp(ratio, 0.0, 1.0) * 100.0));
}

bool shouldShowTransferProgress(relaydesk::storage::TransferState state)
{
    switch (state) {
    case relaydesk::storage::TransferState::Transferring:
    case relaydesk::storage::TransferState::Completed:
    case relaydesk::storage::TransferState::Failed:
    case relaydesk::storage::TransferState::Cancelled:
        return true;
    case relaydesk::storage::TransferState::Pending:
    case relaydesk::storage::TransferState::Offered:
    case relaydesk::storage::TransferState::Interrupted:
    case relaydesk::storage::TransferState::Rejected:
        return false;
    }

    return false;
}

bool shouldShowTransferProgressBlock(
    const relaydesk::storage::ChatMessagePart& part)
{
    return part.GetTransferState().has_value()
        && part.GetTransferState().value()
            == relaydesk::storage::TransferState::Transferring
        && part.GetFileSize().has_value()
        && part.GetFileSize().value() > 0
        && part.GetTransferredSize().has_value();
}

std::string transferFileNameLeaf(std::string fileName)
{
    const std::size_t position = fileName.find_last_of("/\\");
    if (position != std::string::npos) {
        fileName = fileName.substr(position + 1);
    }
    if (fileName.empty() || fileName == "." || fileName == "..") {
        return "transfer.bin";
    }

    for (char& value : fileName) {
        if (value == '/' || value == '\\' || value == ':' || value == '*'
            || value == '?' || value == '"' || value == '<' || value == '>'
            || value == '|') {
            value = '_';
        }
    }
    return fileName;
}

bool incomingTransferTargetExists(
    const relaydesk::storage::ChatMessagePart& part)
{
    const bool folder = part.GetType() == relaydesk::storage::MessagePartType::Folder;
    if ((part.GetType() != relaydesk::storage::MessagePartType::File && !folder)
        || !part.GetFileName().has_value()) {
        return false;
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        const std::filesystem::path targetPath =
            appPaths.GetInboxDirectory()
            / filesystemPathFromUtf8String(
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
        const auto appPaths = relaydesk::storage::createAppPaths();
        return relaydesk::platform::selectSavePathFromDialog(
            appPaths.GetInboxDirectory(),
            transferFileNameLeaf(fileName));
    } catch (const std::exception&) {
        return std::nullopt;
    }
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

float messageFilePartNodeHeight(
    const relaydesk::storage::ChatMessagePart& part,
    float width,
    bool outgoing)
{
    float height = fileDocumentCardHeight(width,
                                          messagePartTitle(part),
                                          false);
    if (shouldShowTransferProgressBlock(part)) {
        height += kMessageFileProgressBlockHeight;
    }
    if (messageFilePartHasActions(part, outgoing)) {
        height += kMessageFileTransferActionGap
            + kTransferActionButtonHeight
            + kMessageFileTransferActionBottomPadding;
    }
    return height;
}

std::string messagePartDetail(const relaydesk::storage::ChatMessagePart& part,
                              bool outgoing)
{
    std::string detail;
    if (part.GetType() == relaydesk::storage::MessagePartType::Folder) {
        detail = "文件夹";
        if (part.GetFileSize().has_value() && part.GetFileSize().value() > 0) {
            detail += " · " + formatFileSize(part.GetFileSize().value());
        }
    } else if (part.GetFileSize().has_value()) {
        detail = formatFileSize(part.GetFileSize().value());
    }

    if (part.GetTransferState().has_value()) {
        const relaydesk::storage::TransferState transferState =
            part.GetTransferState().value();
        const std::string stateText =
            transferStateText(transferState, outgoing);
        if (!stateText.empty()) {
            if (!detail.empty()) {
                detail += " · ";
            }
            detail += stateText;
        }
    }
    return detail;
}

Color messagePartDetailColor(const relaydesk::storage::ChatMessagePart& part)
{
    if (!part.GetTransferState().has_value()) {
        return kMutedText;
    }

    const relaydesk::storage::TransferState state =
        part.GetTransferState().value();
    if (state == relaydesk::storage::TransferState::Failed
        || state == relaydesk::storage::TransferState::Cancelled
        || state == relaydesk::storage::TransferState::Rejected) {
        return kDanger;
    }
    if (state == relaydesk::storage::TransferState::Interrupted) {
        return kAmber;
    }

    return kMutedText;
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
        .path(sticker.relativePath.empty() ? sticker.absolutePath
                                           : sticker.relativePath)
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
    float fontSize = 14.0f;
    float lineHeight = 22.0f;
    bool emoji = false;
    std::string linkUrl;
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
constexpr float kComposerEditorEmojiFontSize = 24.0f;
constexpr float kComposerEditorEmojiLineHeight = 32.0f;
constexpr float kComposerEditorScrollbarWidth = 7.0f;
constexpr float kComposerEditorScrollbarGap = 6.0f;
constexpr float kComposerEditorImageMaxWidth = 260.0f;
constexpr float kComposerEditorImageMaxHeight = 104.0f;
constexpr float kComposerEditorImageFallbackWidth = 156.0f;
constexpr float kComposerEditorImageFallbackHeight = 88.0f;

float composerEditorTextAtomFontSize(const std::string& value)
{
    return isStandaloneEmojiCodepointText(value)
        ? kComposerEditorEmojiFontSize
        : kComposerEditorFontSize;
}

float composerEditorTextAtomLineHeight(const std::string& value)
{
    return isStandaloneEmojiCodepointText(value)
        ? kComposerEditorEmojiLineHeight
        : kComposerEditorLineHeight;
}

float composerEditorTextAtomWidth(const std::string& value)
{
    if (value.empty() || value == "\n") {
        return 0.0f;
    }

    const bool emoji = isStandaloneEmojiCodepointText(value);
    const float fontSize = composerEditorTextAtomFontSize(value);
    const float measuredWidth =
        core::TextPrimitive::measureTextWidth(value, "", fontSize);
    if (std::isfinite(measuredWidth) && measuredWidth > 0.0f) {
        return emoji
            ? std::max(measuredWidth, kComposerEditorEmojiFontSize * 0.85f)
            : measuredWidth;
    }
    const float fallbackWidth =
        static_cast<float>(utf8CodepointCount(value)) * fontSize * 0.62f;
    return emoji
        ? std::max(kComposerEditorEmojiFontSize * 0.85f, fallbackWidth)
        : std::max(4.0f, fallbackWidth);
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
            const std::vector<MessageTextLinkSpan> linkSpans =
                findMessageTextLinkSpans(item.text);
            std::size_t byteOffset = 0;
            for (const std::string& codepoint : splitUtf8Codepoints(item.text)) {
                if (codepoint == "\n") {
                    setComposerEditorCaretLocation(layout,
                                                   documentPosition,
                                                   cursor.x,
                                                   cursor.y,
                                                   kComposerEditorLineHeight);
                    ++documentPosition;
                    byteOffset += codepoint.size();
                    advanceComposerEditorLine(cursor, 0.0f, rowGap);
                    setComposerEditorCaretLocation(layout,
                                                   documentPosition,
                                                   cursor.x,
                                                   cursor.y,
                                                   kComposerEditorLineHeight);
                    continue;
                }

                const bool emoji = isStandaloneEmojiCodepointText(codepoint);
                const float atomWidth = composerEditorTextAtomWidth(codepoint);
                const float atomFontSize = composerEditorTextAtomFontSize(codepoint);
                const float atomLineHeight =
                    composerEditorTextAtomLineHeight(codepoint);
                if (cursor.x > 0.0f && cursor.x + atomWidth > width) {
                    advanceComposerEditorLine(cursor, 0.0f, rowGap);
                }

                setComposerEditorCaretLocation(layout,
                                               documentPosition,
                                               cursor.x,
                                               cursor.y,
                                               atomLineHeight);
                const std::string linkUrl =
                    emoji ? std::string{} : linkUrlForByteOffset(linkSpans, byteOffset);
                layout.textAtoms.push_back(
                    {documentPosition,
                     codepoint,
                     cursor.x,
                     cursor.y,
                     atomWidth,
                     atomFontSize,
                     atomLineHeight,
                     emoji,
                     linkUrl});
                cursor.x += atomWidth;
                cursor.lineHeight =
                    std::max(cursor.lineHeight, atomLineHeight);
                ++documentPosition;
                byteOffset += codepoint.size();
                setComposerEditorCaretLocation(layout,
                                               documentPosition,
                                               cursor.x,
                                               cursor.y,
                                               atomLineHeight);
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
                                 int direction,
                                 bool keepSelection)
{
    if (layout.caretLocations.empty()) {
        caret.position = 0u;
        clearComposerSelection(caret);
        return;
    }

    caret.position = std::min(caret.position, layout.caretLocations.size() - 1u);
    const ComposerEditorCaretLocation& current =
        layout.caretLocations[caret.position];
    const std::size_t previousPosition = caret.position;
    const bool hadSelection = hasComposerSelection(caret);
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
    updateComposerSelectionAfterMove(caret,
                                     previousPosition,
                                     keepSelection,
                                     hadSelection);
}

void handleComposerEditorKeyboardEvent(
    std::vector<ComposerDraftItem>& draftItems,
    ComposerCaretState& caret,
    const ComposerEditorLayout& layout,
    const core::KeyboardEvent& event)
{
    clampComposerCaret(draftItems, caret);
    const std::size_t documentLength = composerDraftDocumentLength(draftItems);
    if (event.copy || event.cut) {
        const auto [begin, end] = composerSelectionRange(caret);
        if (begin != end) {
            core::window::setClipboardText(
                composerDraftRangeText(draftItems, begin, end));
            if (event.cut) {
                (void)removeComposerDraftSelection(draftItems, caret);
            }
        }
        return;
    }
    if (event.selectAll) {
        caret.selectionAnchor = 0u;
        caret.position = documentLength;
        caret.hasPreferredX = false;
        return;
    }

    const bool keepSelection = event.shift;
    if (event.left) {
        if (!keepSelection && hasComposerSelection(caret)) {
            caret.position = composerSelectionRange(caret).first;
            clearComposerSelection(caret);
            caret.hasPreferredX = false;
        } else if (caret.position > 0u) {
            moveComposerCaretTo(caret, caret.position - 1u, keepSelection);
        } else if (!keepSelection) {
            clearComposerSelection(caret);
        }
    }
    if (event.right) {
        if (!keepSelection && hasComposerSelection(caret)) {
            caret.position = composerSelectionRange(caret).second;
            clearComposerSelection(caret);
            caret.hasPreferredX = false;
        } else if (caret.position < documentLength) {
            moveComposerCaretTo(caret, caret.position + 1u, keepSelection);
        } else if (!keepSelection) {
            clearComposerSelection(caret);
        }
    }
    if (event.up) {
        if (!keepSelection && hasComposerSelection(caret)) {
            caret.position = composerSelectionRange(caret).first;
            clearComposerSelection(caret);
        }
        moveComposerCaretVertically(caret, layout, -1, keepSelection);
    }
    if (event.down) {
        if (!keepSelection && hasComposerSelection(caret)) {
            caret.position = composerSelectionRange(caret).second;
            clearComposerSelection(caret);
        }
        moveComposerCaretVertically(caret, layout, 1, keepSelection);
    }
    if (event.home) {
        moveComposerCaretTo(caret,
                            composerCaretLineEdge(layout, caret.position, false),
                            keepSelection);
    }
    if (event.end) {
        moveComposerCaretTo(caret,
                            composerCaretLineEdge(layout, caret.position, true),
                            keepSelection);
    }
    if (event.backspace) {
        if (!removeComposerDraftSelection(draftItems, caret)) {
            removeComposerDraftAtomBeforeCaret(draftItems, caret);
        }
    }
    if (event.del) {
        if (!removeComposerDraftSelection(draftItems, caret)) {
            removeComposerDraftAtomAfterCaret(draftItems, caret);
        }
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

void drawComposerEditorSelectionHighlight(eui::Ui& ui,
                                          const std::string& id,
                                          const ComposerEditorLayout& layout,
                                          const ComposerCaretState& caret)
{
    if (!hasComposerSelection(caret)) {
        return;
    }

    const auto [begin, end] = composerSelectionRange(caret);
    std::size_t selectionIndex = 0;
    for (const ComposerEditorTextAtom& atom : layout.textAtoms) {
        if (atom.position < begin || atom.position >= end) {
            continue;
        }
        rect(ui,
             id + ".selection.text." + std::to_string(selectionIndex),
             atom.x,
             atom.y + 2.0f,
             std::max(1.0f, atom.width),
             std::max(16.0f, atom.lineHeight - 4.0f),
             kSelectionFill,
             2.0f);
        ++selectionIndex;
    }

    for (const ComposerEditorAttachmentAtom& node : layout.attachmentAtoms) {
        if (node.position < begin || node.position >= end) {
            continue;
        }
        rect(ui,
             id + ".selection.attachment." + std::to_string(node.position),
             node.x - 3.0f,
             node.y - 3.0f,
             node.width + 6.0f,
             node.height + 6.0f,
             kSelectionFill,
             8.0f);
    }
}

void drawComposerEditorTextAtoms(eui::Ui& ui,
                                 const std::string& id,
                                 const ComposerEditorLayout& layout)
{
    std::string fragmentText;
    float fragmentX = 0.0f;
    float fragmentY = 0.0f;
    float fragmentWidth = 0.0f;
    float fragmentFontSize = kComposerEditorFontSize;
    float fragmentLineHeight = kComposerEditorLineHeight;
    bool fragmentEmoji = false;
    std::string fragmentLinkUrl;
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
                      fragmentLineHeight,
                      fragmentText,
                      fragmentFontSize,
                      fragmentLineHeight,
                      fragmentLinkUrl.empty() ? kText : kLinkText);
        if (!fragmentLinkUrl.empty()) {
            rect(ui,
                 id + ".text.link.underline." + std::to_string(fragmentIndex),
                 fragmentX,
                 fragmentY + fragmentLineHeight - 4.0f,
                 std::max(1.0f, fragmentWidth),
                 1.0f,
                 kLinkText,
                 0.5f);
        }
        fragmentText.clear();
        fragmentWidth = 0.0f;
        fragmentLinkUrl.clear();
        ++fragmentIndex;
    };

    for (const ComposerEditorTextAtom& atom : layout.textAtoms) {
        const bool sameLine = !fragmentText.empty()
            && std::fabs(atom.y - fragmentY) < 0.5f
            && std::fabs(atom.x - (fragmentX + fragmentWidth)) < 1.5f
            && std::fabs(atom.fontSize - fragmentFontSize) < 0.5f
            && std::fabs(atom.lineHeight - fragmentLineHeight) < 0.5f
            && atom.emoji == fragmentEmoji
            && atom.linkUrl == fragmentLinkUrl;
        if (!sameLine) {
            flushFragment();
            fragmentX = atom.x;
            fragmentY = atom.y;
            fragmentFontSize = atom.fontSize;
            fragmentLineHeight = atom.lineHeight;
            fragmentEmoji = atom.emoji;
            fragmentLinkUrl = atom.linkUrl;
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
                         attachment.kind == PendingAttachmentKind::Folder,
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
                        const std::string& placeholder,
                        const std::function<void()>& onSubmit)
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
                                    caret.selectionBoundsX =
                                        static_cast<float>(bounds.x);
                                    caret.selectionBoundsY =
                                        static_cast<float>(bounds.y);
                                    caret.selectionBoundsWidth =
                                        static_cast<float>(bounds.width);
                                    caret.selectionBoundsHeight =
                                        static_cast<float>(bounds.height);
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
                                    clearComposerSelection(caret);
                                    caret.hasPreferredX = false;
                                })
                                .onDrag([&caret,
                                         &scrollOffset,
                                         layout,
                                         contentWidth](
                                             const core::dsl::DragEvent& event) {
                                    const float scale = contentWidth > 0.0f
                                        ? caret.selectionBoundsWidth / contentWidth
                                        : 1.0f;
                                    const float localX = static_cast<float>(
                                        (event.x - caret.selectionBoundsX)
                                        / std::max(0.001f, scale));
                                    const float localY = static_cast<float>(
                                        (event.y - caret.selectionBoundsY)
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
                                              layout,
                                              onSubmit](
                                                  const core::KeyboardEvent& event) {
                                    if (event.enter && !isControlKeyDown()) {
                                        onSubmit();
                                        return;
                                    }
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
                                drawComposerEditorSelectionHighlight(
                                    contentUi,
                                    "composer.editor",
                                    layout,
                                    caret);
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

    if (peer.unreadCount > 0) {
        const std::string unreadText =
            peer.unreadCount > 99 ? "99+" : std::to_string(peer.unreadCount);
        const float badgeWidth = unreadText.size() >= 3
            ? 30.0f
            : (unreadText.size() == 2 ? 26.0f : 22.0f);
        const float badgeHeight = 22.0f;
        const float badgeX = x + 66.0f;
        const float badgeY = y - 7.0f;
        rect(ui,
             id + ".unread.bg",
             badgeX,
             badgeY,
             badgeWidth,
             badgeHeight,
             Color{0.980f, 0.290f, 0.320f, 1.0f},
             badgeHeight * 0.5f,
             Color{1.0f, 1.0f, 1.0f, 1.0f});
        text(ui,
             id + ".unread.text",
             badgeX,
             badgeY + 2.0f,
             badgeWidth,
             16.0f,
             unreadText,
             11.0f,
             Color{1.0f, 1.0f, 1.0f, 1.0f},
             eui::HorizontalAlign::Center);
    }
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

bool drawFileTypeAssetIcon(eui::Ui& ui,
                           const std::string& id,
                           float x,
                           float y,
                           float size,
                           const std::string& iconFileName)
{
    const std::string svgMarkup = fileTypeIconSvgMarkup(iconFileName);
    if (svgMarkup.empty()) {
        return false;
    }

    ui.svg(id)
        .markup(svgMarkup)
        .position(x, y)
        .size(size, size)
        .contain()
        .build();
    return true;
}

void drawFallbackFileTypeIcon(eui::Ui& ui,
                              const std::string& id,
                              float x,
                              float y,
                              float size,
                              const FileTypeIconStyle& style)
{
    rect(ui, id + ".icon.bg", x, y, size, size, style.fallbackBackground, 7.0f);
    if (style.generic && style.fallbackLabel == "FILE") {
        icon(ui, id + ".icon", x + 2.0f, y + 2.0f, size - 4.0f, 0xE7C3,
             style.fallbackForeground);
        return;
    }

    const float labelHeight = std::max(14.0f, size * 0.42f);
    text(ui,
         id + ".icon.label",
         x,
         y + (size - labelHeight) * 0.5f,
         size,
         labelHeight,
         style.fallbackLabel,
         fileIconLabelFontSize(style.fallbackLabel, size),
         style.fallbackForeground,
         eui::HorizontalAlign::Center);
}

void drawFileTypeIcon(eui::Ui& ui,
                      const std::string& id,
                      float x,
                      float y,
                      float size,
                      const std::string& fileName,
                      bool folder)
{
    const float iconInset = std::max(2.0f, size * 0.08f);
    const float assetSize = size - iconInset * 2.0f;
    if (folder) {
        if (drawFileTypeAssetIcon(ui,
                                  id + ".icon.asset",
                                  x + iconInset,
                                  y + iconInset,
                                  assetSize,
                                  "default_folder.svg")) {
            return;
        }

        rect(ui, id + ".icon.bg", x, y, size, size,
             {0.890f, 0.950f, 0.990f, 1.0f}, 7.0f);
        icon(ui, id + ".icon", x + 2.0f, y + 2.0f, size - 4.0f, 0xE8B7, kTeal);
        return;
    }

    const FileTypeIconStyle style = makeFileTypeIconStyle(fileName);
    if (drawFileTypeAssetIcon(ui,
                              id + ".icon.asset",
                              x + iconInset,
                              y + iconInset,
                              assetSize,
                              style.iconFileName)) {
        return;
    }

    drawFallbackFileTypeIcon(ui, id, x, y, size, style);
}

void drawFileDocumentCard(eui::Ui& ui,
                          const std::string& id,
                          float x,
                          float y,
                          float width,
                          const std::string& title,
                          const std::string& detail,
                          bool folder,
                          bool compact,
                          bool wrapTitle,
                          bool drawBackground,
                          Color detailColor)
{
    const float height = wrapTitle ? fileDocumentCardHeight(width,
                                                            title,
                                                            compact)
                                   : (compact ? kFileDocumentCardCompactHeight
                                              : kFileDocumentCardDefaultHeight);
    const float iconSize = compact ? 34.0f : 40.0f;
    const float iconX = x + 9.0f;
    const float iconY = y + (height - iconSize) * 0.5f;

    if (drawBackground) {
        rect(ui, id + ".bg", x, y, width, height,
             {0.972f, 0.976f, 0.982f, 1.0f}, 8.0f, kBorder);
    }
    drawFileTypeIcon(ui, id, iconX, iconY, iconSize, title, folder);

    const float textX = iconX + iconSize + 10.0f;
    const float titleWidth = fileDocumentCardTitleWidth(width, compact);
    const std::string titleText = wrapTitle
        ? wrappedFileDocumentCardTitle(title, width, compact)
        : title;
    const float titleLineHeight = fileDocumentCardTitleLineHeight(compact);
    const float titleHeight =
        static_cast<float>(textLineCount(titleText)) * titleLineHeight;
    paragraphText(ui,
                  id + ".title",
                  textX,
                  y + (compact ? 5.0f : 7.0f),
                  titleWidth,
                  titleHeight,
                  titleText,
                  fileDocumentCardTitleFontSize(compact),
                  titleLineHeight,
                  kText,
                  eui::HorizontalAlign::Left,
                  wrapTitle);
    if (!detail.empty()) {
        text(ui,
             id + ".detail",
             textX,
             y + (compact ? 26.0f : 31.0f)
                 + std::max(0.0f, titleHeight - titleLineHeight),
             titleWidth,
             18.0f,
             detail,
             compact ? 11.0f : 12.0f,
             detailColor);
    }
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
    std::size_t textPosition = 0;
    std::string text;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float fontSize = kComposerEditorFontSize;
    float lineHeight = kComposerEditorLineHeight;
    bool emoji = false;
    std::string linkUrl;
};

struct MessageFlowTextLine {
    float y = 0.0f;
    float lineHeight = kComposerEditorLineHeight;
    std::vector<const MessageFlowTextAtom*> atoms;
};

struct MessageFlowTextCaret {
    std::size_t position = 0;
    float x = 0.0f;
    float y = 0.0f;
    float height = kComposerEditorLineHeight;
};

struct MessageFlowTextCaretLine {
    float y = 0.0f;
    float height = kComposerEditorLineHeight;
    std::vector<const MessageFlowTextCaret*> carets;
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
    std::vector<MessageFlowTextCaret> textCarets;
    std::vector<MessageFlowPartNode> partNodes;
    float contentWidth = 0.0f;
    float contentHeight = kComposerEditorLineHeight;
};

struct MessageDocumentBubbleMetrics {
    MessageFlowLayout layout;
    float width = 0.0f;
    float height = 0.0f;
};

struct MessageTextSelectionState {
    std::string messageId;
    std::size_t anchor = 0;
    std::size_t cursor = 0;
    std::size_t anchorLineIndex = 0;
    std::size_t cursorLineIndex = 0;
    float boundsX = 0.0f;
    float boundsY = 0.0f;
    float boundsWidth = 0.0f;
    float boundsHeight = 0.0f;
};

struct RuntimeTimelineViewport {
    float x = 0.0f;
    float y = 0.0f;
    float scrollOffset = 0.0f;
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

void expandMessageFlowContentWidth(MessageFlowLayout& layout, float width)
{
    layout.contentWidth = std::max(layout.contentWidth, width);
}

void addMessageFlowTextCaret(MessageFlowLayout& layout,
                             std::size_t position,
                             float x,
                             float y,
                             float height)
{
    if (!layout.textCarets.empty()) {
        const MessageFlowTextCaret& lastCaret = layout.textCarets.back();
        if (lastCaret.position == position
            && std::fabs(lastCaret.x - x) < 0.5f
            && std::fabs(lastCaret.y - y) < 0.5f) {
            return;
        }
    }

    layout.textCarets.push_back({position, x, y, height});
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
        ? kComposerEditorEmojiFontSize
        : kComposerEditorFontSize;
}

float messageTextPartLineHeight(
    const relaydesk::storage::ChatMessagePart& part)
{
    return part.GetType() == relaydesk::storage::MessagePartType::Emoji
        ? kComposerEditorEmojiLineHeight
        : kComposerEditorLineHeight;
}

bool asciiStartsWithInsensitive(const std::string& value,
                                std::size_t offset,
                                const char* prefix)
{
    for (std::size_t index = 0; prefix[index] != '\0'; ++index) {
        if (offset + index >= value.size()) {
            return false;
        }
        const auto actual =
            static_cast<unsigned char>(value[offset + index]);
        const auto expected = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(actual) != std::tolower(expected)) {
            return false;
        }
    }
    return true;
}

bool isUrlTerminatingByte(unsigned char value)
{
    return value <= 0x20u;
}

bool isUrlTrailingPunctuation(unsigned char value)
{
    switch (value) {
    case '.':
    case ',':
    case '!':
    case '?':
    case ';':
    case ':':
    case ')':
    case ']':
    case '}':
    case '>':
        return true;
    default:
        return false;
    }
}

std::vector<MessageTextLinkSpan> findMessageTextLinkSpans(
    const std::string& value)
{
    std::vector<MessageTextLinkSpan> spans;
    for (std::size_t offset = 0; offset < value.size();) {
        const bool hasHttpPrefix =
            asciiStartsWithInsensitive(value, offset, "http://")
            || asciiStartsWithInsensitive(value, offset, "https://");
        const bool hasWwwPrefix =
            asciiStartsWithInsensitive(value, offset, "www.");
        if (!hasHttpPrefix && !hasWwwPrefix) {
            ++offset;
            continue;
        }

        std::size_t end = offset;
        while (end < value.size()
               && !isUrlTerminatingByte(
                   static_cast<unsigned char>(value[end]))) {
            ++end;
        }
        while (end > offset
               && isUrlTrailingPunctuation(
                   static_cast<unsigned char>(value[end - 1u]))) {
            --end;
        }
        if (end == offset) {
            ++offset;
            continue;
        }

        std::string url = value.substr(offset, end - offset);
        if (hasWwwPrefix) {
            url = "https://" + url;
        }
        spans.push_back({offset, end, std::move(url)});
        offset = end;
    }
    return spans;
}

std::string linkUrlForByteOffset(
    const std::vector<MessageTextLinkSpan>& linkSpans,
    std::size_t byteOffset)
{
    for (const MessageTextLinkSpan& span : linkSpans) {
        if (byteOffset >= span.begin && byteOffset < span.end) {
            return span.url;
        }
    }
    return {};
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
        const float nodeWidth = composerDraftFileNodeWidth(flowWidth);
        return {nodeWidth,
                fileDocumentCardHeight(nodeWidth, messagePartTitle(part), false)};
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
    float flowWidth,
    bool outgoing)
{
    switch (part.GetType()) {
    case relaydesk::storage::MessagePartType::Image:
        return messageImageNodeSize(part, flowWidth);
    case relaydesk::storage::MessagePartType::File:
    case relaydesk::storage::MessagePartType::Folder: {
        const float nodeWidth = messageFileNodeWidth(flowWidth);
        return {nodeWidth,
                messageFilePartNodeHeight(part, nodeWidth, outgoing)};
    }
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
    std::size_t& textPosition,
    bool& hasVisiblePart)
{
    const std::string value = messageTextPartValue(part);
    const std::vector<MessageTextLinkSpan> linkSpans =
        findMessageTextLinkSpans(value);
    const float partFontSize = messageTextPartFontSize(part);
    const float partLineHeight = messageTextPartLineHeight(part);
    std::size_t byteOffset = 0;
    addMessageFlowTextCaret(layout,
                            textPosition,
                            cursor.x,
                            cursor.y,
                            partLineHeight);
    for (const std::string& codepoint : splitUtf8Codepoints(value)) {
        if (codepoint == "\n") {
            expandMessageFlowContentWidth(layout, cursor.x);
            hasVisiblePart = true;
            byteOffset += codepoint.size();
            ++textPosition;
            advanceComposerEditorLine(cursor, 0.0f, kMessageFlowRowGap);
            addMessageFlowTextCaret(layout,
                                    textPosition,
                                    cursor.x,
                                    cursor.y,
                                    partLineHeight);
            continue;
        }

        const bool emoji = part.GetType() == relaydesk::storage::MessagePartType::Emoji
            || isStandaloneEmojiCodepointText(codepoint);
        const float atomFontSize = emoji
            ? kComposerEditorEmojiFontSize
            : partFontSize;
        const float atomLineHeight = emoji
            ? kComposerEditorEmojiLineHeight
            : partLineHeight;
        const float atomWidth = messageTextAtomWidth(codepoint, atomFontSize);
        if (cursor.x > 0.0f && cursor.x + atomWidth > width) {
            expandMessageFlowContentWidth(layout, cursor.x);
            advanceComposerEditorLine(cursor, 0.0f, kMessageFlowRowGap);
            addMessageFlowTextCaret(layout,
                                    textPosition,
                                    cursor.x,
                                    cursor.y,
                                    atomLineHeight);
        }

        const std::string linkUrl =
            emoji ? std::string{} : linkUrlForByteOffset(linkSpans, byteOffset);
        layout.textAtoms.push_back(
            {partIndex,
             textPosition,
             codepoint,
             cursor.x,
             cursor.y,
             atomWidth,
             atomFontSize,
             atomLineHeight,
             emoji,
             linkUrl});
        cursor.x += atomWidth;
        expandMessageFlowContentWidth(layout, cursor.x);
        cursor.lineHeight = std::max(cursor.lineHeight, atomLineHeight);
        hasVisiblePart = true;
        byteOffset += codepoint.size();
        ++textPosition;
        addMessageFlowTextCaret(layout,
                                textPosition,
                                cursor.x,
                                cursor.y,
                                atomLineHeight);
    }
}

void addMessageNodePartToLayout(
    MessageFlowLayout& layout,
    ComposerFlowCursor& cursor,
    std::size_t partIndex,
    const relaydesk::storage::ChatMessagePart& part,
    float width,
    bool outgoing,
    bool& hasVisiblePart)
{
    const ComposerAttachmentNodeSize nodeSize =
        messagePartNodeSize(part, width, outgoing);
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
    const float nodeRight = cursor.x + nodeSize.width;
    expandMessageFlowContentWidth(layout, nodeRight);
    cursor.x = nodeRight + kMessageFlowAttachmentGap;
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
    std::size_t textPosition = 0;

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
                                       textPosition,
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
                                       message.GetDirection()
                                           == relaydesk::storage::MessageDirection::Outgoing,
                                       hasVisiblePart);
            break;
        }
    }

    layout.contentHeight = hasVisiblePart
        ? cursor.y + std::max(cursor.lineHeight, kComposerEditorLineHeight)
        : kComposerEditorLineHeight;
    return layout;
}

std::pair<std::size_t, std::size_t> messageTextSelectionRange(
    const MessageTextSelectionState& selection)
{
    return {
        std::min(selection.anchor, selection.cursor),
        std::max(selection.anchor, selection.cursor)
    };
}

bool hasMessageTextSelection(const MessageTextSelectionState& selection,
                             const std::string& messageId)
{
    return selection.messageId == messageId
        && selection.anchor != selection.cursor;
}

std::vector<MessageFlowTextLine> messageTextLayoutLines(
    const MessageFlowLayout& layout)
{
    std::vector<MessageFlowTextLine> lines;
    lines.reserve(layout.textAtoms.size());
    for (const MessageFlowTextAtom& atom : layout.textAtoms) {
        if (lines.empty()
            || std::fabs(atom.y - lines.back().y) > 0.5f) {
            lines.push_back(MessageFlowTextLine{atom.y, atom.lineHeight, {&atom}});
            continue;
        }

        MessageFlowTextLine& line = lines.back();
        line.lineHeight = std::max(line.lineHeight, atom.lineHeight);
        line.atoms.push_back(&atom);
    }
    return lines;
}

std::vector<MessageFlowTextCaretLine> messageTextCaretLines(
    const MessageFlowLayout& layout)
{
    std::vector<MessageFlowTextCaretLine> lines;
    lines.reserve(layout.textCarets.size());
    for (const MessageFlowTextCaret& caret : layout.textCarets) {
        if (lines.empty()
            || std::fabs(caret.y - lines.back().y) > 0.5f) {
            lines.push_back(MessageFlowTextCaretLine{
                caret.y,
                caret.height,
                {&caret}
            });
            continue;
        }

        MessageFlowTextCaretLine& line = lines.back();
        line.height = std::max(line.height, caret.height);
        line.carets.push_back(&caret);
    }
    return lines;
}

std::size_t messageTextDocumentLength(
    const relaydesk::storage::ChatMessageRecord& message)
{
    std::size_t length = 0;
    for (const relaydesk::storage::ChatMessagePart& part : message.GetParts()) {
        switch (part.GetType()) {
        case relaydesk::storage::MessagePartType::Text:
        case relaydesk::storage::MessagePartType::Emoji:
            length += utf8CodepointCount(messageTextPartValue(part));
            break;
        case relaydesk::storage::MessagePartType::Image:
        case relaydesk::storage::MessagePartType::File:
        case relaydesk::storage::MessagePartType::Folder:
            break;
        }
    }
    return length;
}

std::string messageTextRangeText(
    const relaydesk::storage::ChatMessageRecord& message,
    std::size_t begin,
    std::size_t end)
{
    if (begin > end) {
        std::swap(begin, end);
    }

    std::string selectedText;
    std::size_t position = 0;
    for (const relaydesk::storage::ChatMessagePart& part : message.GetParts()) {
        switch (part.GetType()) {
        case relaydesk::storage::MessagePartType::Text:
        case relaydesk::storage::MessagePartType::Emoji:
            break;
        case relaydesk::storage::MessagePartType::Image:
        case relaydesk::storage::MessagePartType::File:
        case relaydesk::storage::MessagePartType::Folder:
            continue;
        }

        for (const std::string& codepoint
             : splitUtf8Codepoints(messageTextPartValue(part))) {
            if (position >= begin && position < end) {
                selectedText += codepoint;
            }
            ++position;
            if (position >= end) {
                return selectedText;
            }
        }
    }
    return selectedText;
}

struct MessageTextPointHit {
    std::size_t position = 0;
    std::size_t lineIndex = 0;
};

MessageTextPointHit messageTextPointHitFromPoint(const MessageFlowLayout& layout,
                                                float x,
                                                float y)
{
    if (!layout.textCarets.empty()) {
        const std::vector<MessageFlowTextCaretLine> lines =
            messageTextCaretLines(layout);
        const MessageFlowTextCaretLine* bestLine = &lines.front();
        float bestLineDistance = 0.0f;
        bool hasBestLineDistance = false;
        std::size_t bestLineIndex = 0;
        std::size_t currentLineIndex = 0;
        for (const MessageFlowTextCaretLine& line : lines) {
            const float top = line.y;
            const float bottom = line.y + line.height;
            const float verticalDistance = y < top
                ? top - y
                : (y > bottom ? y - bottom : 0.0f);
            if (!hasBestLineDistance || verticalDistance < bestLineDistance) {
                bestLineDistance = verticalDistance;
                bestLine = &line;
                bestLineIndex = currentLineIndex;
                hasBestLineDistance = true;
            }
            ++currentLineIndex;
        }

        std::size_t bestPosition = bestLine->carets.front()->position;
        float bestDistance = std::fabs(x - bestLine->carets.front()->x);
        for (const MessageFlowTextCaret* caret : bestLine->carets) {
            const float distance = std::fabs(x - caret->x);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestPosition = caret->position;
            }
        }
        return {bestPosition, bestLineIndex};
    }

    if (layout.textAtoms.empty()) {
        return {};
    }

    const std::vector<MessageFlowTextLine> lines = messageTextLayoutLines(layout);

    const MessageFlowTextLine* bestLine = &lines.front();
    float bestLineDistance = 0.0f;
    bool hasBestLineDistance = false;
    std::size_t bestLineIndex = 0;
    std::size_t currentLineIndex = 0;
    for (const MessageFlowTextLine& line : lines) {
        const float top = line.y;
        const float bottom = line.y + line.lineHeight;
        const float verticalDistance = y < top
            ? top - y
            : (y > bottom ? y - bottom : 0.0f);
        if (!hasBestLineDistance || verticalDistance < bestLineDistance) {
            bestLineDistance = verticalDistance;
            bestLine = &line;
            bestLineIndex = currentLineIndex;
            hasBestLineDistance = true;
        }
        ++currentLineIndex;
    }

    std::size_t bestPosition = bestLine->atoms.front()->textPosition;
    float bestDistance = std::fabs(x - bestLine->atoms.front()->x);
    auto considerPosition = [&](std::size_t position, float positionX) {
        const float distance = std::fabs(x - positionX);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestPosition = position;
        }
    };
    for (const MessageFlowTextAtom* atom : bestLine->atoms) {
        considerPosition(atom->textPosition, atom->x);
        considerPosition(atom->textPosition + 1u, atom->x + atom->width);
    }

    return {bestPosition, bestLineIndex};
}

std::size_t messageTextLineEdgePosition(const MessageFlowLayout& layout,
                                        std::size_t lineIndex,
                                        bool trailingEdge)
{
    const std::vector<MessageFlowTextCaretLine> caretLines =
        messageTextCaretLines(layout);
    if (!caretLines.empty()) {
        const std::size_t clampedIndex =
            std::min(lineIndex, caretLines.size() - 1u);
        const MessageFlowTextCaretLine& line = caretLines[clampedIndex];
        if (!line.carets.empty()) {
            return trailingEdge
                ? line.carets.back()->position
                : line.carets.front()->position;
        }
    }

    const std::vector<MessageFlowTextLine> lines = messageTextLayoutLines(layout);
    if (lines.empty()) {
        return 0u;
    }

    const std::size_t clampedIndex = std::min(lineIndex, lines.size() - 1u);
    const MessageFlowTextLine& line = lines[clampedIndex];
    if (line.atoms.empty()) {
        return 0u;
    }

    return trailingEdge
        ? line.atoms.back()->textPosition + 1u
        : line.atoms.front()->textPosition;
}

float messageTextLineCaretX(const MessageFlowTextCaretLine& line,
                            std::size_t position)
{
    if (line.carets.empty()) {
        return 0.0f;
    }

    if (position <= line.carets.front()->position) {
        return line.carets.front()->x;
    }
    for (const MessageFlowTextCaret* caret : line.carets) {
        if (caret->position >= position) {
            return caret->x;
        }
    }
    return line.carets.back()->x;
}

std::string messageTextLinkUrlFromPoint(const MessageFlowLayout& layout,
                                        float x,
                                        float y)
{
    for (const MessageFlowTextAtom& atom : layout.textAtoms) {
        if (atom.linkUrl.empty()) {
            continue;
        }

        const float right = atom.x + std::max(1.0f, atom.width);
        const float bottom = atom.y + std::max(1.0f, atom.lineHeight);
        if (x >= atom.x && x <= right && y >= atom.y && y <= bottom) {
            return atom.linkUrl;
        }
    }
    return {};
}

std::optional<relaydesk::storage::TransferState> messageTransferFooterState(
    const relaydesk::storage::ChatMessageRecord& message)
{
    (void)message;
    return std::nullopt;
}

bool shouldDrawFailedDeliveryStateInsideBubble(
    const relaydesk::storage::ChatMessageRecord& message,
    bool outgoing)
{
    return outgoing
        && (message.GetDeliveryState() == relaydesk::storage::DeliveryState::Failed
            || messageTransferFooterState(message).has_value());
}

float messageDeliveryStateFooterHeight(
    const relaydesk::storage::ChatMessageRecord& message,
    bool outgoing)
{
    return shouldDrawFailedDeliveryStateInsideBubble(message, outgoing)
        ? kFailedDeliveryStateGap + kFailedDeliveryStateHeight
        : 0.0f;
}

float messageDeliveryStateMinimumContentWidth(
    const relaydesk::storage::ChatMessageRecord& message,
    bool outgoing)
{
    if (!shouldDrawFailedDeliveryStateInsideBubble(message, outgoing)) {
        return 0.0f;
    }

    if (messageTransferFooterState(message).has_value()) {
        return kFailedDeliveryStateTextWidth + kFailedDeliveryStateRightInset;
    }

    return kFailedDeliveryStateTextWidth
        + kFailedDeliveryStateRetryGap
        + kFailedDeliveryStateRetryButtonSize
        + kFailedDeliveryStateRightInset;
}

MessageDocumentBubbleMetrics makeMessageDocumentBubbleMetrics(
    const relaydesk::storage::ChatMessageRecord& message,
    float maxWidth,
    bool outgoing)
{
    constexpr float minimumBubbleWidth = 72.0f;
    const float resolvedMaxWidth = std::max(1.0f, maxWidth);
    const float innerWidth =
        std::max(80.0f, resolvedMaxWidth - kMessageBubblePadding * 2.0f);

    MessageDocumentBubbleMetrics metrics;
    metrics.layout = makeMessageFlowLayout(message, innerWidth);
    const float contentWidth =
        std::max(metrics.layout.contentWidth,
                 messageDeliveryStateMinimumContentWidth(message, outgoing));
    const float minWidth = std::min(minimumBubbleWidth, resolvedMaxWidth);
    metrics.width = std::clamp(contentWidth + kMessageBubblePadding * 2.0f,
                               minWidth,
                               resolvedMaxWidth);
    metrics.height = metrics.layout.contentHeight
        + kMessageBubblePadding * 2.0f
        + messageDeliveryStateFooterHeight(message, outgoing);
    return metrics;
}

void drawMessageImagePart(eui::Ui& ui,
                          const std::string& id,
                          float x,
                          float y,
                          float width,
                          float height,
                          const relaydesk::storage::ChatMessagePart& part,
                          bool outgoing,
                          const RuntimeTimelineViewport& viewport,
                          bool& stickerMenuOpen,
                          float& stickerMenuX,
                          float& stickerMenuY,
                          std::string& stickerMenuPath,
                          std::string& stickerMenuName,
                          std::string& stickerMenuCopyPath)
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
                             messagePartDetail(part, outgoing),
                             false,
                             false,
                             true,
                             true,
                             messagePartDetailColor(part));
        return;
    }

    const auto appPaths = relaydesk::storage::createAppPaths();
    const std::filesystem::path displayPath =
        resolveMessageThumbnailPath(appPaths, part).value_or(imagePath.value());
    const std::string displayPathText =
        makeAttachmentLocalPath(appPaths, displayPath);
    const std::string imagePathText =
        makeAttachmentLocalPath(appPaths, imagePath.value());
    const std::string imageCopyPathText =
        filesystemPathToUtf8String(imagePath.value());
    const std::string imageName = messagePartTitle(part);
    const float imageWidth = width;
    rect(ui, id + ".frame", x, y, imageWidth, height,
         {1.0f, 1.0f, 1.0f, 0.72f}, 8.0f, kBorder);
    ui.image(id + ".image")
        .position(x + 6.0f, y + 6.0f)
        .size(imageWidth - 12.0f, height - 12.0f)
        .path(displayPathText)
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
                        &stickerMenuCopyPath,
                        imagePathText,
                        imageCopyPathText,
                        imageName,
                        viewport](const eui::PointerEvent& event,
                                  const eui::Rect&) {
            stickerMenuOpen = true;
            stickerMenuX = static_cast<float>(event.x) - viewport.x;
            stickerMenuY = static_cast<float>(event.y)
                - viewport.y
                + viewport.scrollOffset;
            stickerMenuPath = imagePathText;
            stickerMenuName = imageName;
            stickerMenuCopyPath = imageCopyPathText;
        })
        .build();
}

void drawTransferActionButton(eui::Ui& ui,
                              const std::string& id,
                              float x,
                              float y,
                              float width,
                              const std::string& label,
                              bool primary,
                              const std::function<void()>& onClick)
{
    const Color fill = primary ? kTeal : Color{1.0f, 1.0f, 1.0f, 0.86f};
    const Color border = primary ? kTeal : kBorder;
    const Color foreground = primary ? Color{1.0f, 1.0f, 1.0f, 1.0f} : kText;
    rect(ui, id + ".bg", x, y, width, 22.0f, fill, 6.0f, border);
    text(ui,
         id + ".text",
         x,
         y + 3.0f,
         width,
         16.0f,
         label,
         11.0f,
         foreground,
         eui::HorizontalAlign::Center);
    ui.rect(id + ".hit")
        .position(x, y)
        .size(width, 22.0f)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick(onClick)
        .build();
}

double estimateTransferBytesPerSecond(eui::Ui& ui,
                                      const std::string& id,
                                      std::uintmax_t transferredSize,
                                      bool transferring)
{
    const auto now = std::chrono::steady_clock::now();
    double& lastTime = ui.state<double>(id + ".last_time");
    double& lastBytes = ui.state<double>(id + ".last_bytes");
    double& bytesPerSecond = ui.state<double>(id + ".bytes_per_second");
    const double nowSeconds =
        std::chrono::duration<double>(now.time_since_epoch()).count();

    if (!transferring) {
        lastTime = nowSeconds;
        lastBytes = static_cast<double>(transferredSize);
        bytesPerSecond = 0.0;
        return 0.0;
    }

    if (lastTime <= 0.0 || lastBytes > static_cast<double>(transferredSize)) {
        lastTime = nowSeconds;
        lastBytes = static_cast<double>(transferredSize);
        return bytesPerSecond;
    }

    const double elapsedSeconds = nowSeconds - lastTime;
    const double deltaBytes = static_cast<double>(transferredSize) - lastBytes;
    if (elapsedSeconds >= 0.2 && deltaBytes >= 0.0) {
        const double currentSpeed = deltaBytes / elapsedSeconds;
        bytesPerSecond = bytesPerSecond <= 0.0
            ? currentSpeed
            : bytesPerSecond * 0.65 + currentSpeed * 0.35;
        lastTime = nowSeconds;
        lastBytes = static_cast<double>(transferredSize);
    }

    return bytesPerSecond;
}

std::string transferProgressDetailText(eui::Ui& ui,
                                       const std::string& id,
                                       const relaydesk::storage::ChatMessagePart& part)
{
    const std::uintmax_t totalSize = part.GetFileSize().value_or(0u);
    const std::uintmax_t transferredSize = std::min(
        part.GetTransferredSize().value_or(0u),
        totalSize);
    std::string detail = formatFileSize(transferredSize)
        + " / "
        + formatFileSize(totalSize);

    const bool transferring = part.GetTransferState().has_value()
        && part.GetTransferState().value()
            == relaydesk::storage::TransferState::Transferring;
    const double bytesPerSecond =
        estimateTransferBytesPerSecond(ui,
                                       id,
                                       transferredSize,
                                       transferring);
    if (transferring && bytesPerSecond > 1.0) {
        const double remainingBytes =
            static_cast<double>(totalSize - transferredSize);
        detail += " - " + formatTransferRate(bytesPerSecond);
        detail += " - 剩余 "
            + formatDurationClock(remainingBytes / bytesPerSecond);
    }
    return detail;
}

void drawMessageFileProgress(eui::Ui& ui,
                             const std::string& id,
                             float x,
                             float y,
                             float width,
                             const relaydesk::storage::ChatMessagePart& part)
{
    const std::optional<int> percent = transferProgressPercent(part);
    const float progress = percent.has_value()
        ? std::clamp(static_cast<float>(percent.value()) / 100.0f, 0.0f, 1.0f)
        : 0.0f;
    const float percentWidth = 44.0f;
    const float trackWidth = std::max(20.0f, width - percentWidth - 12.0f);
    const float trackY = y + kMessageFileProgressGap;
    rect(ui,
         id + ".track",
         x,
         trackY,
         trackWidth,
         kMessageFileProgressTrackHeight,
         {0.790f, 0.810f, 0.820f, 1.0f},
         3.0f);
    rect(ui,
         id + ".fill",
         x,
         trackY,
         trackWidth * progress,
         kMessageFileProgressTrackHeight,
         kTeal,
         3.0f);
    if (percent.has_value()) {
        text(ui,
             id + ".percent",
             x + trackWidth + 12.0f,
             y - 1.0f,
             percentWidth,
             18.0f,
             std::to_string(percent.value()) + "%",
             12.0f,
             kText,
             eui::HorizontalAlign::Right);
    }

    text(ui,
         id + ".detail",
         x,
         trackY + kMessageFileProgressTrackHeight
             + kMessageFileProgressDetailGap,
         width,
         kMessageFileProgressDetailHeight,
         transferProgressDetailText(ui, id + ".speed", part),
         11.0f,
         kMutedText);
}

void drawFileCleanedNotice(eui::Ui& ui,
                           const std::string& id,
                           float x,
                           float y,
                           float width)
{
    constexpr float noticeWidth = 92.0f;
    const float noticeX = x + width - noticeWidth - 8.0f;
    rect(ui,
         id + ".bg",
         noticeX,
         y,
         noticeWidth,
         kTransferActionButtonHeight,
         kAmberSoft,
         6.0f,
         kAmber);
    text(ui,
         id + ".text",
         noticeX,
         y + 3.0f,
         noticeWidth,
         16.0f,
         "文件已被清理",
         11.0f,
         kAmber,
         eui::HorizontalAlign::Center);
}

void drawMessageFilePart(eui::Ui& ui,
                         const std::string& id,
                         float x,
                         float y,
                         float width,
                         const relaydesk::storage::ChatMessageRecord& message,
                         const relaydesk::storage::ChatMessagePart& part,
                         bool outgoing,
                         relaydesk::runtime::RelayDeskRuntime& runtime)
{
    const bool folder =
        part.GetType() == relaydesk::storage::MessagePartType::Folder;
    const float cardHeight = fileDocumentCardHeight(width,
                                                    messagePartTitle(part),
                                                    false);
    drawFileDocumentCard(ui,
                         id,
                         x,
                         y,
                         width,
                         messagePartTitle(part),
                         messagePartDetail(part, outgoing),
                         folder,
                         false,
                         true,
                         false,
                         messagePartDetailColor(part));
    float actionBaseY = y + cardHeight;
    if (shouldShowTransferProgressBlock(part)) {
        drawMessageFileProgress(ui,
                                id + ".progress",
                                x + 2.0f,
                                actionBaseY,
                                width - 4.0f,
                                part);
        actionBaseY += kMessageFileProgressBlockHeight;
    }

    const std::optional<relaydesk::storage::TransferState> transferState =
        part.GetTransferState();
    const bool canAcceptIncomingTransfer =
        !outgoing
        && transferState.has_value()
        && shouldShowIncomingTransferAcceptActions(transferState.value());
    if (transferState.has_value()
        && !canAcceptIncomingTransfer
        && shouldShowOpenActionsForTransferState(transferState.value(),
                                                 outgoing)) {
        const std::optional<OpenableMessagePath> openablePath =
            resolveOpenableMessagePath(part);
        if (!openablePath.has_value()) {
            if (outgoing
                || transferState.value()
                    == relaydesk::storage::TransferState::Completed) {
                drawFileCleanedNotice(
                    ui,
                    id + ".cleaned",
                    x,
                    actionBaseY + kMessageFileTransferActionGap,
                    width);
            }
            return;
        }

        constexpr float openButtonWidth = 48.0f;
        constexpr float revealButtonWidth = 82.0f;
        constexpr float gap = 6.0f;
        const float rowY = actionBaseY + kMessageFileTransferActionGap;
        const float rowWidth =
            folder ? revealButtonWidth : openButtonWidth + gap + revealButtonWidth;
        float buttonX = x + width - rowWidth - 8.0f;
        if (!folder) {
            drawTransferActionButton(
                ui,
                id + ".open",
                buttonX,
                rowY,
                openButtonWidth,
                "打开",
                true,
                [openPath = openablePath->path] {
                    shellOpenPath(openPath);
                });
            buttonX += openButtonWidth + gap;
        }
        drawTransferActionButton(
            ui,
            id + ".reveal",
            buttonX,
            rowY,
            revealButtonWidth,
            "打开文件夹",
            false,
            [openPath = openablePath->path, folder] {
                if (folder) {
                    shellOpenPath(openPath);
                    return;
                }
                shellRevealPath(openPath);
        });
        return;
    }

    if (transferState.has_value()) {
        const relaydesk::storage::TransferState state = transferState.value();
        if (outgoing
            && state == relaydesk::storage::TransferState::Interrupted
            && (part.GetType() == relaydesk::storage::MessagePartType::File
                || part.GetType()
                    == relaydesk::storage::MessagePartType::Folder)) {
            constexpr float continueButtonWidth = 72.0f;
            const float rowY = actionBaseY + kMessageFileTransferActionGap;
            const float buttonX = x + width - continueButtonWidth - 8.0f;
            drawTransferActionButton(
                ui,
                id + ".resume_send",
                buttonX,
                rowY,
                continueButtonWidth,
                "继续发送",
                true,
                [&runtime,
                 messageId = message.GetMessageId(),
                 partId = part.GetPartId()] {
                    runtime.sendSelectedPeerFileTransfer(messageId, partId);
                });
            return;
        }

        if (outgoing
            && state == relaydesk::storage::TransferState::Failed
            && (part.GetType() == relaydesk::storage::MessagePartType::File
                || part.GetType()
                    == relaydesk::storage::MessagePartType::Folder)) {
            constexpr float resendButtonWidth = 72.0f;
            const float rowY = actionBaseY + kMessageFileTransferActionGap;
            const float buttonX = x + width - resendButtonWidth - 8.0f;
            drawTransferActionButton(
                ui,
                id + ".resend",
                buttonX,
                rowY,
                resendButtonWidth,
                "重新发送",
                true,
                [&runtime,
                 deliveryFailed =
                     message.GetDeliveryState()
                         == relaydesk::storage::DeliveryState::Failed,
                 messageId = message.GetMessageId(),
                 partId = part.GetPartId()] {
                    if (deliveryFailed) {
                        runtime.resendSelectedPeerMessage(messageId);
                    } else {
                        runtime.sendSelectedPeerFileTransfer(messageId, partId);
                    }
                });
            return;
        }

        if (outgoing
            && state == relaydesk::storage::TransferState::Offered
            && (part.GetType() == relaydesk::storage::MessagePartType::File
                || part.GetType()
                    == relaydesk::storage::MessagePartType::Folder)) {
            constexpr float sendButtonWidth = 72.0f;
            constexpr float cancelButtonWidth = 48.0f;
            constexpr float gap = 6.0f;
            const float rowY = actionBaseY + kMessageFileTransferActionGap;
            const float rowWidth = sendButtonWidth + gap + cancelButtonWidth;
            float buttonX = x + width - rowWidth - 8.0f;
            drawTransferActionButton(
                ui,
                id + ".send",
                buttonX,
                rowY,
                sendButtonWidth,
                "主动发送",
                true,
                [&runtime,
                 messageId = message.GetMessageId(),
                 partId = part.GetPartId()] {
                    runtime.sendSelectedPeerFileTransfer(messageId, partId);
                });
            buttonX += sendButtonWidth + gap;
            drawTransferActionButton(
                ui,
                id + ".cancel",
                buttonX,
                rowY,
                cancelButtonWidth,
                "取消",
                false,
                [&runtime,
                 messageId = message.GetMessageId(),
                 partId = part.GetPartId()] {
                    runtime.cancelSelectedPeerFileTransfer(messageId, partId);
                });
            return;
        }

        const bool canCancel = (outgoing
                                && state
                                    == relaydesk::storage::TransferState::Transferring)
            || (!outgoing
                && state == relaydesk::storage::TransferState::Transferring);
        if (canCancel) {
            constexpr float cancelButtonWidth = 48.0f;
            const float rowY = actionBaseY + kMessageFileTransferActionGap;
            const float buttonX = x + width - cancelButtonWidth - 8.0f;
            drawTransferActionButton(
                ui,
                id + ".cancel",
                buttonX,
                rowY,
                cancelButtonWidth,
                "取消",
                false,
                [&runtime,
                 messageId = message.GetMessageId(),
                 partId = part.GetPartId()] {
                    runtime.cancelSelectedPeerFileTransfer(messageId, partId);
                });
            return;
        }
    }

    if (outgoing
        || !transferState.has_value()
        || !shouldShowIncomingTransferAcceptActions(transferState.value())) {
        return;
    }

    if (transferState.value()
        == relaydesk::storage::TransferState::Interrupted) {
        constexpr float continueButtonWidth = 72.0f;
        const float rowY = actionBaseY + kMessageFileTransferActionGap;
        const float buttonX = x + width - continueButtonWidth - 8.0f;
        drawTransferActionButton(
            ui,
            id + ".resume_receive",
            buttonX,
            rowY,
            continueButtonWidth,
            "继续接收",
            true,
            [&runtime,
             messageId = message.GetMessageId(),
             partId = part.GetPartId()] {
                runtime.acceptSelectedPeerFileTransfer(messageId, partId, false);
            });
        return;
    }

    const bool targetExists = incomingTransferTargetExists(part);
    const float buttonWidth = 56.0f;
    const float gap = 6.0f;
    const float rowY = actionBaseY + kMessageFileTransferActionGap;
    const float rowWidth = targetExists
        ? buttonWidth * 4.0f + gap * 3.0f
        : buttonWidth * 3.0f + gap * 2.0f;
    float buttonX = x + width - rowWidth - 8.0f;
    drawTransferActionButton(
        ui,
        id + ".accept",
        buttonX,
        rowY,
        buttonWidth,
        "接收",
        true,
        [&runtime,
         messageId = message.GetMessageId(),
         partId = part.GetPartId()] {
            runtime.acceptSelectedPeerFileTransfer(messageId, partId, false);
        });
    buttonX += buttonWidth + gap;
    drawTransferActionButton(
        ui,
        id + ".save_as",
        buttonX,
        rowY,
        buttonWidth,
        "另存为",
        false,
        [&runtime,
         fileName = part.GetFileName().value_or(std::string()),
         messageId = message.GetMessageId(),
         partId = part.GetPartId()] {
            const std::optional<std::filesystem::path> savePath =
                selectIncomingTransferSavePath(fileName);
            if (savePath.has_value()) {
                runtime.acceptSelectedPeerFileTransferAs(messageId,
                                                         partId,
                                                         savePath.value());
            }
        });
    buttonX += buttonWidth + gap;
    if (targetExists) {
        drawTransferActionButton(
            ui,
            id + ".overwrite",
            buttonX,
            rowY,
            buttonWidth,
            "覆盖",
            false,
            [&runtime,
             messageId = message.GetMessageId(),
             partId = part.GetPartId()] {
                runtime.acceptSelectedPeerFileTransfer(messageId, partId, true);
            });
        buttonX += buttonWidth + gap;
    }
    drawTransferActionButton(
        ui,
        id + ".reject",
        buttonX,
        rowY,
        buttonWidth,
        "拒绝",
        false,
        [&runtime,
         messageId = message.GetMessageId(),
         partId = part.GetPartId()] {
            runtime.rejectSelectedPeerFileTransfer(messageId, partId);
        });
}

void drawMessageFlowTextSelectionHighlight(
    eui::Ui& ui,
    const std::string& id,
    const MessageFlowLayout& layout,
    float x,
    float y,
    const std::string& messageId,
    const MessageTextSelectionState& selection)
{
    if (!hasMessageTextSelection(selection, messageId)) {
        return;
    }

    const auto [begin, end] = messageTextSelectionRange(selection);
    std::size_t selectionIndex = 0;
    for (const MessageFlowTextAtom& atom : layout.textAtoms) {
        if (atom.textPosition < begin || atom.textPosition >= end) {
            continue;
        }

        rect(ui,
             id + ".text.selection." + std::to_string(selectionIndex),
             x + atom.x,
             y + atom.y + 2.0f,
             std::max(1.0f, atom.width),
             std::max(16.0f, atom.lineHeight - 4.0f),
             kSelectionFill,
             2.0f);
        ++selectionIndex;
    }
}

void updateMessageTextSelectionFromPoint(
    const MessageFlowLayout& layout,
    float pointerX,
    float pointerY,
    const eui::Rect& bounds,
    float width,
    float height,
    float scrollOffset,
    MessageTextSelectionState& selection)
{
    const float scaleX = width > 0.0f
        ? static_cast<float>(bounds.width) / width
        : 1.0f;
    const float scaleY = height > 0.0f
        ? static_cast<float>(bounds.height) / height
        : 1.0f;
    const float localX = static_cast<float>(
        (pointerX - bounds.x) / std::max(0.001f, scaleX));
    const float localY = static_cast<float>(
        (pointerY - bounds.y) / std::max(0.001f, scaleY))
        + scrollOffset;
    const MessageTextPointHit hit =
        messageTextPointHitFromPoint(layout, localX, localY);
    selection.cursor = hit.position;
    selection.cursorLineIndex = hit.lineIndex;
}

void drawMessageFlowTextInteractionLayer(
    eui::Ui& ui,
    const std::string& id,
    const MessageFlowLayout& layout,
    float x,
    float y,
    float width,
    float height,
    float scrollOffset,
    const std::string& messageId,
    const std::string& messageText,
    MessageTextSelectionState& selection)
{
    if (layout.textAtoms.empty()) {
        return;
    }

    const std::size_t documentLength = utf8CodepointCount(messageText);
    ui.rect(id + ".text.hit")
        .position(x, y)
        .size(std::max(1.0f, width), std::max(1.0f, height))
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .focusable()
        .imeRect(1.0f, 1.0f, 1.5f, kComposerEditorLineHeight)
        .onPress([&selection,
                  layout,
                  messageId,
                  width,
                  height,
                  scrollOffset](const eui::PointerEvent& event,
                                const eui::Rect& bounds) {
            selection.messageId = messageId;
            selection.boundsX = static_cast<float>(bounds.x);
            selection.boundsY = static_cast<float>(bounds.y);
            selection.boundsWidth = static_cast<float>(bounds.width);
            selection.boundsHeight = static_cast<float>(bounds.height);
            updateMessageTextSelectionFromPoint(layout,
                                                static_cast<float>(event.x),
                                                static_cast<float>(event.y),
                                                bounds,
                                                width,
                                                height,
                                                scrollOffset,
                                                selection);
            selection.anchor = selection.cursor;
            selection.anchorLineIndex = selection.cursorLineIndex;
        })
        .onMove([&selection,
                 layout,
                 messageId,
                 width,
                 height,
                 scrollOffset](const eui::PointerEvent& event,
                               const eui::Rect& bounds) -> bool {
            if (!event.down || selection.messageId != messageId) {
                return false;
            }

            updateMessageTextSelectionFromPoint(layout,
                                                static_cast<float>(event.x),
                                                static_cast<float>(event.y),
                                                bounds,
                                                width,
                                                height,
                                                scrollOffset,
                                                selection);
            return true;
        })
        .onDrag([&selection,
                 layout,
                 messageId,
                 width,
                 height,
                 scrollOffset](const core::dsl::DragEvent& event) {
            if (selection.messageId != messageId) {
                return;
            }

            const eui::Rect bounds{
                selection.boundsX,
                selection.boundsY,
                selection.boundsWidth,
                selection.boundsHeight
            };
            updateMessageTextSelectionFromPoint(layout,
                                                static_cast<float>(event.x),
                                                static_cast<float>(event.y),
                                                bounds,
                                                width,
                                                height,
                                                scrollOffset,
                                                selection);
        })
        .onRelease([&selection,
                    layout,
                    messageId,
                    width,
                    height,
                    scrollOffset](const eui::PointerEvent& event,
                                  const eui::Rect& bounds) {
            if (selection.messageId != messageId
                || hasMessageTextSelection(selection, messageId)) {
                return;
            }

            const float scaleX = width > 0.0f
                ? static_cast<float>(bounds.width) / width
                : 1.0f;
            const float scaleY = height > 0.0f
                ? static_cast<float>(bounds.height) / height
                : 1.0f;
            const float localX = static_cast<float>(
                (event.x - bounds.x) / std::max(0.001f, scaleX));
            const float localY = static_cast<float>(
                (event.y - bounds.y) / std::max(0.001f, scaleY))
                + scrollOffset;
            const std::string linkUrl =
                messageTextLinkUrlFromPoint(layout, localX, localY);
            if (!linkUrl.empty()) {
                (void)core::platform::openUrl(linkUrl);
            }
        })
        .onTextInput([&selection,
                      messageId,
                      messageText,
                      documentLength](const core::KeyboardEvent& event) {
            if (selection.messageId != messageId) {
                return;
            }
            if (event.selectAll) {
                selection.anchor = 0u;
                selection.cursor = documentLength;
                return;
            }
            if (!event.copy || !hasMessageTextSelection(selection, messageId)) {
                return;
            }

            auto [begin, end] = messageTextSelectionRange(selection);
            begin = std::min(begin, documentLength);
            end = std::min(end, documentLength);
            if (begin != end) {
                core::window::setClipboardText(
                    utf8SubstringByCodepointRange(messageText, begin, end));
            }
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
    std::string fragmentLinkUrl;
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
                      fragmentLineHeight,
                      fragmentLinkUrl.empty() ? kText : kLinkText);
        if (!fragmentLinkUrl.empty()) {
            const float underlineY =
                y + fragmentY + fragmentLineHeight - 4.0f;
            rect(ui,
                 id + ".text.link.underline."
                     + std::to_string(fragmentIndex),
                 x + fragmentX,
                 underlineY,
                 std::max(1.0f, fragmentWidth),
                 1.0f,
                 kLinkText,
                 0.5f);
        }
        fragmentText.clear();
        fragmentWidth = 0.0f;
        fragmentLinkUrl.clear();
        ++fragmentIndex;
    };

    for (const MessageFlowTextAtom& atom : layout.textAtoms) {
        const bool sameLine = !fragmentText.empty()
            && std::fabs(atom.y - fragmentY) < 0.5f
            && std::fabs(atom.x - (fragmentX + fragmentWidth)) < 1.5f
            && std::fabs(atom.fontSize - fragmentFontSize) < 0.5f
            && std::fabs(atom.lineHeight - fragmentLineHeight) < 0.5f
            && atom.emoji == fragmentEmoji
            && atom.linkUrl == fragmentLinkUrl;
        if (!sameLine) {
            flushFragment();
            fragmentX = atom.x;
            fragmentY = atom.y;
            fragmentFontSize = atom.fontSize;
            fragmentLineHeight = atom.lineHeight;
            fragmentEmoji = atom.emoji;
            fragmentLinkUrl = atom.linkUrl;
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
                         const relaydesk::storage::ChatMessageRecord& message,
                         const relaydesk::storage::ChatMessagePart& part,
                         const MessageFlowPartNode& node,
                         bool outgoing,
                         relaydesk::runtime::RelayDeskRuntime& runtime,
                         const RuntimeTimelineViewport& viewport,
                         bool& stickerMenuOpen,
                         float& stickerMenuX,
                         float& stickerMenuY,
                         std::string& stickerMenuPath,
                         std::string& stickerMenuName,
                         std::string& stickerMenuCopyPath)
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
                             outgoing,
                             viewport,
                             stickerMenuOpen,
                             stickerMenuX,
                             stickerMenuY,
                             stickerMenuPath,
                             stickerMenuName,
                             stickerMenuCopyPath);
        return;
    case relaydesk::storage::MessagePartType::File:
        drawMessageFilePart(ui,
                            id,
                            x,
                            y,
                            node.width,
                            message,
                            part,
                            outgoing,
                            runtime);
        return;
    case relaydesk::storage::MessagePartType::Folder:
        drawMessageFilePart(ui,
                            id,
                            x,
                            y,
                            node.width,
                            message,
                            part,
                            outgoing,
                            runtime);
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
    const MessageDocumentBubbleMetrics& metrics,
    const relaydesk::storage::ChatMessageRecord& message,
    bool outgoing,
    relaydesk::runtime::RelayDeskRuntime& runtime,
    const RuntimeTimelineViewport& viewport,
    bool& stickerMenuOpen,
    float& stickerMenuX,
    float& stickerMenuY,
    std::string& stickerMenuPath,
    std::string& stickerMenuName,
    std::string& stickerMenuCopyPath,
    MessageTextSelectionState& selection)
{
    const float width = metrics.width;
    const float bubbleHeight = metrics.height;
    const MessageFlowLayout& layout = metrics.layout;
    const Color fill = outgoing ? kTealSoft : Color{0.990f, 0.990f, 0.992f, 1.0f};
    const std::size_t messageTextLength = messageTextDocumentLength(message);
    const std::string messageText =
        messageTextRangeText(message, 0u, messageTextLength);

    rect(ui, id + ".bg", x, y, width, bubbleHeight, fill, 9.0f, kBorder);

    const float contentX = x + kMessageBubblePadding;
    const float contentY = y + kMessageBubblePadding;
    drawMessageFlowTextSelectionHighlight(ui,
                                          id,
                                          layout,
                                          contentX,
                                          contentY,
                                          message.GetMessageId(),
                                          selection);
    drawMessageFlowTextAtoms(ui, id, layout, contentX, contentY);
    drawMessageFlowTextInteractionLayer(ui,
                                        id,
                                        layout,
                                        contentX,
                                        contentY,
                                        std::max(1.0f,
                                                 width
                                                     - kMessageBubblePadding * 2.0f),
                                        layout.contentHeight,
                                        viewport.scrollOffset,
                                        message.GetMessageId(),
                                        messageText,
                                        selection);
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
                            message,
                            message.GetParts()[node.partIndex],
                            node,
                            outgoing,
                            runtime,
                            viewport,
                            stickerMenuOpen,
                            stickerMenuX,
                            stickerMenuY,
                            stickerMenuPath,
                            stickerMenuName,
                            stickerMenuCopyPath);
    }

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
    drawFileTypeIcon(ui, id + ".file", x + 18.0f, y + 22.0f, 42.0f,
                     transfer.fileName, false);
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
    bool& settingsOpen = ui.state<bool>("settings.open");

    avatar(ui, "local.avatar", x + 22.0f, y + 16.0f, 44.0f, displayName);
    text(ui, "local.name", x + 78.0f, y + 15.0f, width - 132.0f, 26.0f,
         displayName, 17.0f);
    text(ui, "local.address", x + 78.0f, y + 41.0f, width - 132.0f, 22.0f,
         makeLocalStatusText(runtime), 13.0f, kMutedText);
    ui.rect("local.settings.hit")
        .position(x + width - 56.0f, y + 17.0f)
        .size(38.0f, 38.0f)
        .states(settingsOpen ? kTealSoft : Color{0.0f, 0.0f, 0.0f, 0.0f},
                kTealSoft,
                {0.790f, 0.940f, 0.930f, 1.0f})
        .radius(19.0f)
        .onClick([&settingsOpen] {
            settingsOpen = true;
        })
        .build();
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
            peer.GetUnreadMessageCount(),
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
        .onClick([&ui, &runtime, deviceId] {
            bool& settingsOpen = ui.state<bool>("settings.open");
            settingsOpen = false;
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
        PeerPreview{"demo-alex", "Alex-PC", "192.168.1.24", true, true, 0},
        PeerPreview{
            "demo-desktop", "DESKTOP-J8K2TQ", "192.168.1.31", true, false, 0},
        PeerPreview{
            "demo-laptop", "LAPTOP-9F3V2M", "192.168.1.42", true, false, 0},
        PeerPreview{"demo-server", "DEV-SERVER", "192.168.1.10", true, false, 0},
        PeerPreview{"demo-mark", "MARK-PC", "192.168.1.77", true, false, 0},
        PeerPreview{
            "demo-finance", "FINANCE-PC", "192.168.1.15", false, false, 0},
        PeerPreview{"demo-hr", "HR-LAPTOP", "192.168.1.28", false, false, 0},
        PeerPreview{"demo-old", "OLD-PC", "192.168.1.55", false, false, 0},
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
    const std::optional<relaydesk::storage::TransferState> transferFooterState =
        messageTransferFooterState(message);
    if (transferFooterState.has_value()) {
        const float rowX = bubbleX + bubbleWidth - kFailedDeliveryStateTextWidth
            - kFailedDeliveryStateRightInset;
        rect(ui,
             id + ".state.bg",
             rowX,
             statusY + 1.0f,
             kFailedDeliveryStateTextWidth,
             18.0f,
             kDangerSoft,
             6.0f,
             kDanger);
        text(ui,
             id + ".state",
             rowX,
             statusY + 1.0f,
             kFailedDeliveryStateTextWidth,
             18.0f,
             transferStateText(transferFooterState.value()),
             11.0f,
             kDanger,
             eui::HorizontalAlign::Center);
        return;
    }

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

    const float rowWidth = kFailedDeliveryStateTextWidth
        + kFailedDeliveryStateRetryGap
        + kFailedDeliveryStateRetryButtonSize;
    const float rowX = bubbleX + bubbleWidth - rowWidth
        - kFailedDeliveryStateRightInset;
    const float retryButtonX = rowX + kFailedDeliveryStateTextWidth
        + kFailedDeliveryStateRetryGap;
    rect(ui,
         id + ".state.bg",
         rowX,
         statusY + 1.0f,
         kFailedDeliveryStateTextWidth,
         18.0f,
         kDangerSoft,
         6.0f,
         kDanger);
    text(ui,
         id + ".state",
         rowX,
         statusY + 1.0f,
         kFailedDeliveryStateTextWidth,
         18.0f,
         deliveryStateText(message.GetDeliveryState()),
         11.0f,
         kDanger,
         eui::HorizontalAlign::Center);
    rect(ui,
         id + ".retry.bg",
         retryButtonX,
         statusY,
         kFailedDeliveryStateRetryButtonSize,
         kFailedDeliveryStateRetryButtonSize,
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
        .size(kFailedDeliveryStateRetryButtonSize,
              kFailedDeliveryStateRetryButtonSize)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&runtime, messageId = message.GetMessageId()] {
            runtime.resendSelectedPeerMessage(messageId);
        })
        .build();
}

void drawRuntimeMessageTimestamp(eui::Ui& ui,
                                 const std::string& id,
                                 float bubbleX,
                                 float y,
                                 float bubbleWidth,
                                 bool outgoing,
                                 const std::string& createdAt)
{
    const std::string timestamp = formatMessageTimestamp(createdAt);
    if (timestamp.empty()) {
        return;
    }

    const float timestampX = outgoing
        ? bubbleX - kMessageTimestampGap - kMessageTimestampWidth
        : bubbleX + bubbleWidth + kMessageTimestampGap;
    text(ui,
         id + ".time",
         timestampX,
         y + 8.0f,
         kMessageTimestampWidth,
         18.0f,
         timestamp,
         11.0f,
         kSubtleText,
         outgoing ? eui::HorizontalAlign::Right
                  : eui::HorizontalAlign::Left);
}

float runtimeMessageBubbleMaxWidth(float timelineWidth,
                                   float availableBubbleWidth)
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
    float contentHeight,
    const RuntimeTimelineViewport& viewport,
    const std::vector<relaydesk::storage::ChatMessageRecord>& messages,
    relaydesk::runtime::RelayDeskRuntime& runtime)
{
    constexpr float avatarSize = 34.0f;
    constexpr float sidePadding = 22.0f;
    constexpr float avatarBubbleGap = 12.0f;
    const float availableBubbleWidth =
        std::max(160.0f,
                 width
                     - sidePadding * 2.0f
                     - avatarSize
                     - avatarBubbleGap
                     - kMessageTimestampGap
                     - kMessageTimestampWidth);
    const float messageBubbleMaxWidth =
        runtimeMessageBubbleMaxWidth(width, availableBubbleWidth);
    float y = 22.0f;
    bool& stickerMenuOpen = ui.state<bool>("chat.sticker.context.open");
    float& stickerMenuX = ui.state<float>("chat.sticker.context.x");
    float& stickerMenuY = ui.state<float>("chat.sticker.context.y");
    std::string& stickerMenuPath =
        ui.state<std::string>("chat.sticker.context.path");
    std::string& stickerMenuName =
        ui.state<std::string>("chat.sticker.context.name");
    std::string& stickerMenuCopyPath =
        ui.state<std::string>("chat.sticker.context.copy.path");
    std::string& stickerMenuStatus =
        ui.state<std::string>("chat.sticker.context.status");
    MessageTextSelectionState& messageTextSelection =
        ui.state<MessageTextSelectionState>("chat.runtime.text.selection");

    for (std::size_t index = 0; index < messages.size(); ++index) {
        const auto& message = messages[index];
        const bool outgoing =
            message.GetDirection() == relaydesk::storage::MessageDirection::Outgoing;
        const MessageDocumentBubbleMetrics metrics =
            makeMessageDocumentBubbleMetrics(message,
                                             messageBubbleMaxWidth,
                                             outgoing);
        const float bubbleWidth = metrics.width;
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
                                                             metrics,
                                                             message,
                                                             outgoing,
                                                             runtime,
                                                             viewport,
                                                             stickerMenuOpen,
                                                             stickerMenuX,
                                                             stickerMenuY,
                                                             stickerMenuPath,
                                                             stickerMenuName,
                                                             stickerMenuCopyPath,
                                                             messageTextSelection);
        drawRuntimeMessageTimestamp(ui,
                                    id,
                                    bubbleX,
                                    y,
                                    bubbleWidth,
                                    outgoing,
                                    message.GetCreatedAt());
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
            .screen(width, contentHeight)
            .position(stickerMenuX, stickerMenuY)
            .size(172.0f, 34.0f)
            .items({"收藏为表情", "复制图片", "取消"})
            .style(stickerContextMenuStyle())
            .open(stickerMenuOpen)
            .onSelect([&stickerMenuOpen,
                       &stickerMenuPath,
                       &stickerMenuName,
                       &stickerMenuCopyPath,
                       &stickerMenuStatus](int itemIndex) {
                if (itemIndex == 0 && !stickerMenuPath.empty()) {
                    const auto error =
                        favoriteStickerImage(stickerMenuPath, stickerMenuName);
                    stickerMenuStatus = error.value_or("已收藏为表情");
                } else if (itemIndex == 1 && !stickerMenuCopyPath.empty()) {
                    const bool copied = relaydesk::platform::copyImageFileToClipboard(
                        filesystemPathFromUtf8String(stickerMenuCopyPath));
                    stickerMenuStatus = copied ? "已复制到剪贴板" : "图片复制失败";
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
                     width
                         - sidePadding * 2.0f
                         - avatarSize
                         - avatarBubbleGap
                         - kMessageTimestampGap
                         - kMessageTimestampWidth);
        const float messageBubbleMaxWidth =
            runtimeMessageBubbleMaxWidth(width, availableBubbleWidth);
        float measuredContentHeight = 42.0f;
        for (const auto& message : messages) {
            const bool outgoing =
                message.GetDirection()
                == relaydesk::storage::MessageDirection::Outgoing;
            const MessageDocumentBubbleMetrics metrics =
                makeMessageDocumentBubbleMetrics(message,
                                                 messageBubbleMaxWidth,
                                                 outgoing);
            const bool failedStateInside =
                shouldDrawFailedDeliveryStateInsideBubble(message, outgoing);
            measuredContentHeight += metrics.height
                + (outgoing && !failedStateInside ? 24.0f : 16.0f);
        }
        const float contentHeight = std::max(height, measuredContentHeight);
        const float maxScrollOffset = std::max(0.0f, contentHeight - height);
        std::string& scrollPeerDeviceId =
            ui.state<std::string>("chat.runtime.scroll.peer.device_id");
        std::string& scrollTailMessageId =
            ui.state<std::string>("chat.runtime.scroll.tail.message_id");
        std::string& scrollHeadMessageId =
            ui.state<std::string>("chat.runtime.scroll.head.message_id");
        std::size_t& scrollMessageCount =
            ui.state<std::size_t>("chat.runtime.scroll.message_count");
        float& previousMaxScrollOffset =
            ui.state<float>("chat.runtime.scroll.previous_max_offset");
        float& previousContentHeight =
            ui.state<float>("chat.runtime.scroll.previous_content_height");
        std::uint64_t& autoScrollRevision =
            ui.state<std::uint64_t>("chat.runtime.scroll.auto_revision");
        const std::string& peerDeviceId = selectedPeer->GetDeviceId();
        const std::string& headMessageId = messages.front().GetMessageId();
        const std::string& tailMessageId = messages.back().GetMessageId();
        const bool peerChanged = scrollPeerDeviceId != peerDeviceId;
        const bool tailMessageChanged = scrollTailMessageId != tailMessageId;
        const bool messageCountChanged = scrollMessageCount != messages.size();
        const bool tailChanged = tailMessageChanged || messageCountChanged;
        const bool headChanged = scrollHeadMessageId != headMessageId;
        const bool olderMessagesPrepended = !peerChanged
            && headChanged
            && !tailMessageChanged
            && scrollMessageCount < messages.size();
        const bool contentHeightChanged =
            std::abs(previousContentHeight - contentHeight) > 0.5f;
        const bool wasAtBottom = previousMaxScrollOffset <= 0.5f
            || scrollOffset >= previousMaxScrollOffset - 8.0f;
        if (peerChanged || ((tailChanged || contentHeightChanged) && wasAtBottom)) {
            scrollOffset = maxScrollOffset;
            ++autoScrollRevision;
        } else if (olderMessagesPrepended && !wasAtBottom) {
            scrollOffset = std::clamp(
                scrollOffset + (contentHeight - previousContentHeight),
                0.0f,
                maxScrollOffset);
        } else {
            scrollOffset = std::clamp(scrollOffset, 0.0f, maxScrollOffset);
        }
        scrollPeerDeviceId = peerDeviceId;
        scrollHeadMessageId = headMessageId;
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
        const std::string scrollViewId = "chat.runtime.scroll."
            + std::to_string(autoScrollRevision);
        ui.stack("chat.runtime.scroll.pos")
            .position(x, y)
            .size(width, height)
            .content([&] {
                components::scrollView(ui, scrollViewId)
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
                        const RuntimeTimelineViewport viewport{x, y, scrollOffset};
                        contentUi.stack("chat.runtime.content")
                            .size(contentWidth, contentHeight)
                            .content([&] {
                                drawRuntimeChatTimelineContent(contentUi,
                                                               contentWidth,
                                                               contentHeight,
                                                               viewport,
                                                               messages,
                                                               runtime);
                            })
                            .build();
                    })
                    .build();
            })
            .build();
        if (scrollOffset <= kChatLoadMoreTopThreshold
            && runtime.GetSelectedPeerHasMoreMessages()) {
            runtime.loadMoreSelectedPeerMessages();
        }
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
    bool& screenClipPending = ui.state<bool>("composer.screenshot.pending");
    std::uint32_t& screenClipClipboardSequence =
        ui.state<std::uint32_t>("composer.screenshot.clipboard.sequence");
    std::chrono::steady_clock::time_point& screenClipStartedAt =
        ui.state<std::chrono::steady_clock::time_point>("composer.screenshot.started");
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
    pollPendingScreenClipCapture(screenClipPending,
                                 screenClipClipboardSequence,
                                 screenClipStartedAt,
                                 draftItems,
                                 composerCaret);
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
    auto selectAttachmentFolder = [&composerCaret, &draftItems, &emojiPickerOpen] {
        insertComposerDraftAttachmentPathsAtCaret(
            draftItems,
            composerCaret,
            selectAttachmentFolderFromDialog());
        emojiPickerOpen = false;
    };
    auto startScreenClip = [&composerCaret,
                            &draftItems,
                            &emojiPickerOpen,
                            &screenClipClipboardSequence,
                            &screenClipPending,
                            &screenClipStartedAt] {
        if (composerDraftAttachmentCount(draftItems) >= kMaxPendingAttachmentCount) {
            return;
        }

        screenClipClipboardSequence =
            relaydesk::platform::getClipboardSequenceNumber();
        if (!relaydesk::platform::startScreenClipCapture()) {
            screenClipPending = false;
            return;
        }

        screenClipPending = true;
        screenClipStartedAt = std::chrono::steady_clock::now();
        clearComposerSelection(composerCaret);
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
                       placeholder,
                       submitMessage);
    if (width < 560.0f) {
        const float firstIconX = composerX + 16.0f;
        const float iconGap = 42.0f;
        icon(ui, "composer.screenshot", firstIconX, toolbarY, iconSize,
             0xE722, screenClipPending ? kTeal : kText);
        ui.rect("composer.screenshot.hit")
            .position(firstIconX - 5.0f, toolbarY - 5.0f)
            .size(iconSize + 10.0f, iconSize + 10.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(startScreenClip)
            .build();
        icon(ui, "composer.file", firstIconX + iconGap, toolbarY, iconSize,
             0xE723, kText);
        ui.rect("composer.file.hit")
            .position(firstIconX + iconGap - 5.0f, toolbarY - 5.0f)
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
        icon(ui, "composer.screenshot", firstIconX + iconGap, toolbarY, iconSize,
             0xE722, screenClipPending ? kTeal : kText);
        ui.rect("composer.screenshot.hit")
            .position(firstIconX + iconGap - 5.0f, toolbarY - 5.0f)
            .size(iconSize + 10.0f, iconSize + 10.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(startScreenClip)
            .build();
        icon(ui, "composer.file", firstIconX + iconGap * 2.0f, toolbarY, iconSize,
             0xE723, kText);
        ui.rect("composer.file.hit")
            .position(firstIconX + iconGap * 2.0f - 5.0f, toolbarY - 5.0f)
            .size(iconSize + 10.0f, iconSize + 10.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(selectAttachmentFiles)
            .build();
        icon(ui, "composer.folder", firstIconX + iconGap * 3.0f, toolbarY,
             iconSize, 0xE8B7, kText);
        ui.rect("composer.folder.hit")
            .position(firstIconX + iconGap * 3.0f - 5.0f, toolbarY - 5.0f)
            .size(iconSize + 10.0f, iconSize + 10.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(selectAttachmentFolder)
            .build();
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

std::string localDisplayName(
    const relaydesk::runtime::RelayDeskRuntime& runtime)
{
    const auto& localUser = runtime.GetLocalUser();
    return localUser.GetDisplayName().empty()
        ? "RelayDesk"
        : localUser.GetDisplayName();
}

std::string trimAsciiWhitespace(std::string value)
{
    const auto first = std::find_if(
        value.begin(),
        value.end(),
        [](unsigned char ch) {
            return std::isspace(ch) == 0;
        });
    const auto last = std::find_if(
        value.rbegin(),
        value.rend(),
        [](unsigned char ch) {
            return std::isspace(ch) == 0;
        }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

float settingsFieldWidth(float contentWidth)
{
    return std::min(380.0f, std::max(240.0f, contentWidth * 0.56f));
}

void drawSettingsTitle(eui::Ui& ui,
                       const std::string& id,
                       float x,
                       float y,
                       float width,
                       const std::string& title,
                       const std::string& detail)
{
    text(ui, id + ".title", x, y, width, 28.0f, title, 19.0f, kText);
    text(ui, id + ".detail", x, y + 32.0f, width, 22.0f, detail, 13.0f,
         kMutedText);
}

void drawSettingsRowLabel(eui::Ui& ui,
                          const std::string& id,
                          float x,
                          float y,
                          float width,
                          const std::string& title,
                          const std::string& detail)
{
    text(ui, id + ".title", x, y, width, 24.0f, title, 14.0f, kText);
    text(ui, id + ".detail", x, y + 25.0f, width, 22.0f, detail, 12.0f,
         kMutedText);
}

void drawSettingsCategoryRail(eui::Ui& ui,
                              float x,
                              float y,
                              float width,
                              float height,
                              int& selectedCategory)
{
    rect(ui, "settings.rail.bg", x, y, width, height, kPanelBackground);
    rect(ui, "settings.rail.line", x + width, y, 1.0f, height, kBorder);
    text(ui, "settings.rail.title", x + 24.0f, y + 24.0f, width - 48.0f,
         30.0f, "设置", 22.0f, kText);
    text(ui, "settings.rail.detail", x + 24.0f, y + 58.0f, width - 48.0f,
         22.0f, "界面占位", 13.0f, kMutedText);

    const float rowX = x + 14.0f;
    const float rowW = width - 28.0f;
    const float firstY = y + 108.0f;
    constexpr float rowH = 58.0f;
    constexpr float rowGap = 8.0f;
    for (int index = 0; index < static_cast<int>(kSettingsCategories.size());
         ++index) {
        const auto& item = kSettingsCategories[static_cast<std::size_t>(index)];
        const float rowY = firstY + static_cast<float>(index) * (rowH + rowGap);
        const bool active = index == selectedCategory;
        if (active) {
            rect(ui,
                 "settings.rail.active." + std::to_string(index),
                 rowX,
                 rowY,
                 rowW,
                 rowH,
                 kTealSoft,
                 7.0f);
            rect(ui,
                 "settings.rail.active.mark." + std::to_string(index),
                 rowX,
                 rowY + 10.0f,
                 3.0f,
                 rowH - 20.0f,
                 kTeal,
                 2.0f);
        }
        ui.rect("settings.rail.hit." + std::to_string(index))
            .position(rowX, rowY)
            .size(rowW, rowH)
            .states(Color{0.0f, 0.0f, 0.0f, 0.0f},
                    active ? kTealSoft
                           : Color{0.955f, 0.975f, 0.975f, 1.0f},
                    Color{0.900f, 0.955f, 0.950f, 1.0f})
            .radius(7.0f)
            .onClick([&selectedCategory, index] {
                selectedCategory = index;
            })
            .build();
        icon(ui,
             "settings.rail.icon." + std::to_string(index),
             rowX + 12.0f,
             rowY + 13.0f,
             32.0f,
             item.icon,
             active ? kTeal : kMutedText);
        text(ui,
             "settings.rail.label." + std::to_string(index),
             rowX + 52.0f,
             rowY + 9.0f,
             rowW - 68.0f,
             22.0f,
             item.title,
             14.0f,
             active ? kText : kMutedText);
        text(ui,
             "settings.rail.sub." + std::to_string(index),
             rowX + 52.0f,
             rowY + 32.0f,
             rowW - 68.0f,
             20.0f,
             item.subtitle,
             12.0f,
             kSubtleText);
    }
}

void drawSettingsInput(eui::Ui& ui,
                       const std::string& id,
                       float x,
                       float y,
                       float width,
                       std::string& value,
                       const std::string& placeholder)
{
    ui.stack(id + ".pos")
        .position(x, y)
        .size(width, 40.0f)
        .content([&] {
            components::input(ui, id)
                .size(width, 40.0f)
                .text(value)
                .placeholder(placeholder)
                .fontSize(14.0f)
                .inset(12.0f)
                .style(settingsInputStyle())
                .onChange([&value](const std::string& next) {
                    value = next;
                })
                .build();
        })
        .build();
}

void drawSettingsButton(eui::Ui& ui,
                        const std::string& id,
                        float x,
                        float y,
                        float width,
                        float height,
                        const std::string& label,
                        bool primary,
                        std::function<void()> onClick)
{
    ui.stack(id + ".pos")
        .position(x, y)
        .size(width, height)
        .content([&] {
            components::button(ui, id)
                .size(width, height)
                .text(label)
                .fontSize(14.0f)
                .style(settingsButtonStyle(primary))
                .onClick(std::move(onClick))
                .build();
        })
        .build();
}

void drawSettingsProfilePage(eui::Ui& ui,
                             float x,
                             float y,
                             float width,
                             relaydesk::runtime::RelayDeskRuntime& runtime)
{
    bool& initialized = ui.state<bool>("settings.profile.initialized");
    std::string& userName = ui.state<std::string>("settings.profile.username");
    std::string& savedUserName =
        ui.state<std::string>("settings.profile.saved_username");
    std::string& status = ui.state<std::string>("settings.profile.status");
    bool& statusIsError = ui.state<bool>("settings.profile.status_error");
    const std::string currentUserName = localDisplayName(runtime);
    if (!initialized || savedUserName != currentUserName) {
        userName = currentUserName;
        savedUserName = currentUserName;
        status.clear();
        statusIsError = false;
        initialized = true;
    }
    if (userName != savedUserName && statusIsError) {
        status.clear();
        statusIsError = false;
    }
    if (userName != savedUserName && !statusIsError) {
        status = "有未保存的修改";
    }

    drawSettingsTitle(ui, "settings.profile.header", x, y, width,
                      "个人资料", "用户名会保存到本机身份，并用于局域网心跳广播。");
    const float rowY = y + 86.0f;
    const float labelW = std::min(180.0f, width * 0.34f);
    const float fieldX = x + labelW + 26.0f;
    const float fieldAreaW = std::max(1.0f, width - labelW - 26.0f);
    const float saveButtonW = 76.0f;
    const bool inlineActions = fieldAreaW >= 340.0f;
    const float baseFieldW =
        std::min(settingsFieldWidth(fieldAreaW), fieldAreaW);
    const float fieldW = inlineActions
        ? std::min(baseFieldW, fieldAreaW - saveButtonW - 12.0f)
        : baseFieldW;
    const float saveButtonX = inlineActions ? fieldX + fieldW + 12.0f : fieldX;
    const float saveButtonY = inlineActions ? rowY + 1.0f : rowY + 50.0f;
    const float statusY = inlineActions ? rowY + 50.0f : rowY + 92.0f;
    const float separatorY = inlineActions ? rowY + 78.0f : rowY + 120.0f;

    drawSettingsRowLabel(ui, "settings.profile.name.label", x, rowY, labelW,
                         "用户名", "保存后立即更新本机显示名。");
    drawSettingsInput(ui, "settings.profile.name.input", fieldX, rowY, fieldW,
                      userName, "输入用户名");
    drawSettingsButton(ui,
                       "settings.profile.name.save",
                       saveButtonX,
                       saveButtonY,
                       saveButtonW,
                       38.0f,
                       "保存",
                       true,
                       [&runtime,
                        &userName,
                        &savedUserName,
                        &status,
                        &statusIsError] {
                           const std::string nextName =
                               trimAsciiWhitespace(userName);
                           if (nextName.empty()) {
                               status = "用户名不能为空";
                               statusIsError = true;
                               return;
                           }

                           try {
                               runtime.updateLocalDisplayName(nextName);
                               userName = localDisplayName(runtime);
                               savedUserName = userName;
                               status = "已保存";
                               statusIsError = false;
                           } catch (const std::exception& error) {
                               status = std::string("保存失败：") + error.what();
                               statusIsError = true;
                           }
                       });
    if (!status.empty()) {
        const Color statusColor =
            statusIsError ? kDanger : (status == "已保存" ? kTeal : kMutedText);
        text(ui, "settings.profile.name.status", fieldX, statusY, fieldAreaW,
             22.0f, status, 12.0f, statusColor);
    }
    rect(ui, "settings.profile.name.line", x, separatorY, width, 1.0f, kBorder);

    const float avatarY = separatorY + 26.0f;
    drawSettingsRowLabel(ui, "settings.profile.avatar.label", x, avatarY, labelW,
                         "头像", "头像修改暂未接入。");
    avatar(ui, "settings.profile.avatar", fieldX, avatarY - 2.0f, 58.0f,
           userName.empty() ? "RelayDesk" : userName);
    drawSettingsButton(ui,
                       "settings.profile.avatar.button",
                       fieldX + 76.0f,
                       avatarY + 8.0f,
                       116.0f,
                       36.0f,
                       "更换头像",
                       false,
                       [&status, &statusIsError] {
                           status = "头像修改暂未接入";
                           statusIsError = false;
                       });
}

void drawSettingsAppearancePage(eui::Ui& ui,
                                float x,
                                float y,
                                float width)
{
    int& fontSize = ui.state<int>("settings.appearance.font_size");
    if (fontSize < 12 || fontSize > 22) {
        fontSize = 14;
    }

    drawSettingsTitle(ui, "settings.appearance.header", x, y, width,
                      "外观", "字体大小控件先只影响这个设置页的预览。");
    const float rowY = y + 86.0f;
    const float labelW = std::min(180.0f, width * 0.34f);
    const float fieldX = x + labelW + 26.0f;
    const float controlW = std::min(300.0f, width - labelW - 26.0f);

    drawSettingsRowLabel(ui, "settings.appearance.font.label", x, rowY, labelW,
                         "字体大小", "范围先固定在 12 到 22。");
    drawSettingsButton(ui,
                       "settings.appearance.font.minus",
                       fieldX,
                       rowY,
                       38.0f,
                       36.0f,
                       "-",
                       false,
                       [&fontSize] {
                           fontSize = std::max(12, fontSize - 1);
                       });
    text(ui,
         "settings.appearance.font.value",
         fieldX + 48.0f,
         rowY + 4.0f,
         84.0f,
         28.0f,
         std::to_string(fontSize) + " px",
         15.0f,
         kText,
         eui::HorizontalAlign::Center);
    drawSettingsButton(ui,
                       "settings.appearance.font.plus",
                       fieldX + 142.0f,
                       rowY,
                       38.0f,
                       36.0f,
                       "+",
                       false,
                       [&fontSize] {
                           fontSize = std::min(22, fontSize + 1);
                       });

    const float trackX = fieldX;
    const float trackY = rowY + 62.0f;
    const float trackW = std::max(160.0f, controlW);
    const float progress =
        static_cast<float>(fontSize - 12) / static_cast<float>(22 - 12);
    rect(ui, "settings.appearance.font.track", trackX, trackY, trackW, 5.0f,
         {0.850f, 0.885f, 0.895f, 1.0f}, 3.0f);
    rect(ui, "settings.appearance.font.fill", trackX, trackY, trackW * progress,
         5.0f, kTeal, 3.0f);
    rect(ui, "settings.appearance.font.thumb",
         trackX + std::max(0.0f, trackW * progress - 6.0f), trackY - 5.0f,
         15.0f, 15.0f, kTeal, 8.0f);

    rect(ui, "settings.appearance.preview.bg", x, rowY + 106.0f, width, 92.0f,
         {1.0f, 1.0f, 1.0f, 1.0f}, 7.0f, kBorder);
    text(ui, "settings.appearance.preview.title", x + 18.0f, rowY + 120.0f,
         width - 36.0f, 24.0f, "预览文本", 13.0f, kMutedText);
    text(ui, "settings.appearance.preview.text", x + 18.0f, rowY + 150.0f,
         width - 36.0f, 30.0f, "RelayDesk 消息字体大小预览", static_cast<float>(fontSize),
         kText);
}

void drawSettingsSendPage(eui::Ui& ui, float x, float y, float width)
{
    int& sendMode = ui.state<int>("settings.send.mode");
    sendMode = std::clamp(sendMode, 0, 1);
    bool& dropdownOpen = ui.state<bool>("settings.send.dropdown.open");
    std::vector<std::string> modes{"Enter", "Ctrl + Enter"};

    drawSettingsTitle(ui, "settings.send.header", x, y, width,
                      "发送消息", "发送快捷键只提供 Enter 和 Ctrl + Enter 两种选择。");
    const float rowY = y + 86.0f;
    const float labelW = std::min(180.0f, width * 0.34f);
    const float fieldX = x + labelW + 26.0f;
    const float fieldW = std::min(260.0f, width - labelW - 26.0f);

    drawSettingsRowLabel(ui, "settings.send.mode.label", x, rowY, labelW,
                         "发送方式", "不可完全自定义。");
    ui.stack("settings.send.mode.dropdown.pos")
        .position(fieldX, rowY)
        .size(fieldW, 136.0f)
        .content([&] {
            components::dropdown(ui, "settings.send.mode.dropdown")
                .size(fieldW, 40.0f)
                .items(modes)
                .selected(sendMode)
                .open(dropdownOpen)
                .itemHeight(34.0f)
                .style(settingsDropdownStyle())
                .zIndex(90)
                .onChange([&sendMode](int next) {
                    sendMode = std::clamp(next, 0, 1);
                })
                .onOpenChange([&dropdownOpen](bool open) {
                    dropdownOpen = open;
                })
                .build();
        })
        .build();
    text(ui, "settings.send.mode.note", fieldX, rowY + 54.0f,
         std::max(180.0f, fieldW), 22.0f,
         sendMode == 0 ? "当前占位选择：Enter 发送"
                       : "当前占位选择：Ctrl + Enter 发送",
         12.0f, kMutedText);
}

void drawSettingsScreenshotPage(eui::Ui& ui,
                                float x,
                                float y,
                                float width)
{
    bool& initialized = ui.state<bool>("settings.shortcut.initialized");
    std::string& shortcut = ui.state<std::string>("settings.shortcut.screenshot");
    if (!initialized) {
        shortcut = "Ctrl + Shift + A";
        initialized = true;
    }

    drawSettingsTitle(ui, "settings.shortcut.header", x, y, width,
                      "截图", "截图快捷键保留完全自定义入口，包括组合键。");
    const float rowY = y + 86.0f;
    const float labelW = std::min(180.0f, width * 0.34f);
    const float fieldX = x + labelW + 26.0f;
    const float fieldW = std::min(320.0f, width - labelW - 26.0f);

    drawSettingsRowLabel(ui, "settings.shortcut.capture.label", x, rowY, labelW,
                         "快捷键", "这里先用文本框占位。");
    drawSettingsInput(ui, "settings.shortcut.capture.input", fieldX, rowY,
                      fieldW, shortcut, "例如 Ctrl + Shift + A");
    text(ui, "settings.shortcut.capture.note", fieldX, rowY + 54.0f,
         std::max(220.0f, fieldW), 22.0f,
         "后续需要接入真实按键录入和冲突检测。", 12.0f, kMutedText);
}

std::atomic_bool& notificationSoundEnabledFlag()
{
    static std::atomic_bool soundEnabled{true};
    return soundEnabled;
}

bool& notificationSoundEnabledState(eui::Ui& ui)
{
    bool& initialized = ui.state<bool>("settings.notification.initialized");
    bool& soundEnabled = ui.state<bool>("settings.notification.sound_enabled");
    if (!initialized) {
        soundEnabled = true;
        notificationSoundEnabledFlag().store(true, std::memory_order_relaxed);
        initialized = true;
    }
    return soundEnabled;
}

bool& launchAtStartupEnabledState(eui::Ui& ui)
{
    bool& initialized = ui.state<bool>("settings.startup.initialized");
    bool& launchAtStartup =
        ui.state<bool>("settings.startup.launch_at_startup");
    if (!initialized) {
        launchAtStartup = loadStoredLaunchAtStartupEnabled();
        initialized = true;
    }
    return launchAtStartup;
}

void drawSettingsNotificationPage(eui::Ui& ui,
                                  float x,
                                  float y,
                                  float width)
{
    bool& soundEnabled = notificationSoundEnabledState(ui);
    bool& launchAtStartup = launchAtStartupEnabledState(ui);
    std::string& startupStatus =
        ui.state<std::string>("settings.startup.status");

    drawSettingsTitle(ui, "settings.notification.header", x, y, width,
                      "通知", "管理提示音和系统启动行为。");
    const float rowY = y + 86.0f;
    const float labelW = std::min(180.0f, width * 0.34f);
    const float fieldX = x + labelW + 26.0f;
    const float fieldW = std::max(260.0f, width - labelW - 26.0f);

    drawSettingsRowLabel(ui, "settings.notification.sound.label", x, rowY,
                         labelW, "消息通知音", "收到消息时播放提示音。");
    ui.stack("settings.notification.sound.switch.pos")
        .position(fieldX, rowY + 5.0f)
        .size(240.0f, 34.0f)
        .content([&] {
            components::toggleSwitch(ui, "settings.notification.sound.switch")
                .size(240.0f, 34.0f)
                .checked(soundEnabled)
                .label(soundEnabled ? "已启用" : "已关闭")
                .fontSize(14.0f)
                .trackSize(48.0f, 24.0f)
                .style(settingsSwitchStyle())
                .onChange([&soundEnabled](bool next) {
                    soundEnabled = next;
                    notificationSoundEnabledFlag().store(
                        next,
                        std::memory_order_relaxed);
                })
                .build();
        })
        .build();

    const float startupRowY = rowY + 74.0f;
    drawSettingsRowLabel(ui,
                         "settings.startup.launch.label",
                         x,
                         startupRowY,
                         labelW,
                         "开机自启",
                         "登录 Windows 后自动启动 RelayDesk。");
    ui.stack("settings.startup.launch.switch.pos")
        .position(fieldX, startupRowY + 5.0f)
        .size(240.0f, 34.0f)
        .content([&] {
            components::toggleSwitch(ui, "settings.startup.launch.switch")
                .size(240.0f, 34.0f)
                .checked(launchAtStartup)
                .label(launchAtStartup ? "已启用" : "已关闭")
                .fontSize(14.0f)
                .trackSize(48.0f, 24.0f)
                .style(settingsSwitchStyle())
                .onChange([&launchAtStartup, &startupStatus](bool next) {
                    launchAtStartup = next;
                    const bool saved = saveStoredLaunchAtStartupEnabled(next);
                    const bool applied = applyLaunchAtStartupEnabled(next);
                    if (saved && applied) {
                        startupStatus.clear();
                    } else if (!saved && !applied) {
                        startupStatus = "保存设置和写入系统启动项失败";
                    } else if (!saved) {
                        startupStatus = "系统启动项已更新，但保存设置失败";
                    } else {
                        startupStatus = "保存设置成功，但写入系统启动项失败";
                    }
                })
                .build();
        })
        .build();
    if (!startupStatus.empty()) {
        text(ui,
             "settings.startup.launch.status",
             fieldX,
             startupRowY + 42.0f,
             fieldW,
             22.0f,
             startupStatus,
             12.0f,
             kDanger);
    }
}

void drawSettingsUpdatePage(eui::Ui& ui, float x, float y, float width)
{
    bool& initialized = ui.state<bool>("settings.update.initialized");
    std::string& status = ui.state<std::string>("settings.update.status");
    if (!initialized) {
        status = "尚未检查更新";
        initialized = true;
    }

    drawSettingsTitle(ui, "settings.update.header", x, y, width,
                      "检查更新", "先预留更新入口，不发起网络请求。");
    const float rowY = y + 86.0f;
    const float labelW = std::min(180.0f, width * 0.34f);
    const float fieldX = x + labelW + 26.0f;
    const float fieldW = std::max(260.0f, width - labelW - 26.0f);
    const std::string versionText =
        "当前版本号：" + std::to_string(relaydesk::core::kAppVersion);

    drawSettingsRowLabel(ui, "settings.update.check.label", x, rowY, labelW,
                         "软件更新", "后续接入版本检查。");
    drawSettingsButton(ui,
                       "settings.update.check.button",
                       fieldX,
                       rowY,
                       116.0f,
                       38.0f,
                       "检查更新",
                       true,
                       [&status] {
                           status = "检查更新功能占位，等待接入更新服务";
                       });
    text(ui, "settings.update.version", fieldX, rowY + 52.0f, fieldW, 22.0f,
         versionText, 12.0f, kMutedText);
    text(ui, "settings.update.status", fieldX, rowY + 76.0f, fieldW, 22.0f,
         status, 12.0f, kMutedText);
}

void drawSettingsContent(eui::Ui& ui,
                         float x,
                         float y,
                         float width,
                         float height,
                         int selectedCategory,
                         relaydesk::runtime::RelayDeskRuntime& runtime)
{
    rect(ui, "settings.content.bg", x, y, width, height,
         {1.0f, 1.0f, 1.0f, 1.0f});
    const float insetX = width < 620.0f ? 22.0f : 34.0f;
    const float contentX = x + insetX;
    const float contentY = y + 100.0f;
    const float contentW = std::max(1.0f, width - insetX * 2.0f);
    rect(ui, "settings.content.top.line", x, y + 78.0f, width, 1.0f, kBorder);

    switch (selectedCategory) {
    case 0:
        drawSettingsProfilePage(ui, contentX, contentY, contentW, runtime);
        break;
    case 1:
        drawSettingsAppearancePage(ui, contentX, contentY, contentW);
        break;
    case 2:
        drawSettingsSendPage(ui, contentX, contentY, contentW);
        break;
    case 3:
        drawSettingsScreenshotPage(ui, contentX, contentY, contentW);
        break;
    case 4:
        drawSettingsNotificationPage(ui, contentX, contentY, contentW);
        break;
    case 5:
        drawSettingsUpdatePage(ui, contentX, contentY, contentW);
        break;
    default:
        break;
    }
}

void drawSettingsPage(eui::Ui& ui,
                      float x,
                      float y,
                      float width,
                      float height,
                      relaydesk::runtime::RelayDeskRuntime& runtime)
{
    bool& settingsOpen = ui.state<bool>("settings.open");
    int& selectedCategory = ui.state<int>("settings.category");
    selectedCategory =
        std::clamp(selectedCategory, 0,
                   static_cast<int>(kSettingsCategories.size()) - 1);

    rect(ui, "settings.bg", x, y, width, height, {1.0f, 1.0f, 1.0f, 1.0f});
    const float railWidth = std::clamp(width * 0.27f, 184.0f, 238.0f);
    drawSettingsCategoryRail(ui, x, y, railWidth, height, selectedCategory);

    const float contentX = x + railWidth + 1.0f;
    const float contentW = std::max(1.0f, width - railWidth - 1.0f);
    drawSettingsContent(ui, contentX, y, contentW, height, selectedCategory,
                        runtime);

    text(ui, "settings.header.title", contentX + 34.0f, y + 20.0f,
         contentW - 96.0f, 28.0f, kSettingsCategories[
             static_cast<std::size_t>(selectedCategory)].title,
         20.0f, kText);
    text(ui, "settings.header.detail", contentX + 34.0f, y + 50.0f,
         contentW - 96.0f, 22.0f, "设置项会按当前页面的接入状态保存",
         13.0f, kMutedText);
    ui.rect("settings.close.hit")
        .position(contentX + contentW - 58.0f, y + 16.0f)
        .size(42.0f, 42.0f)
        .states(Color{0.0f, 0.0f, 0.0f, 0.0f},
                Color{0.930f, 0.960f, 0.960f, 1.0f},
                Color{0.850f, 0.925f, 0.920f, 1.0f})
        .radius(21.0f)
        .onClick([&settingsOpen] {
            settingsOpen = false;
        })
        .build();
    icon(ui, "settings.close.icon", contentX + contentW - 52.0f, y + 22.0f,
         32.0f, 0xE711, kText);
}

void drawAppUpdateDownloadBlock(
    eui::Ui& ui,
    const relaydesk::runtime::AppUpdatePrompt& prompt,
    float x,
    float y,
    float width)
{
    const std::string fileName =
        prompt.GetFileName().empty() ? "relaydesk.exe" : prompt.GetFileName();
    const float iconSize = 34.0f;
    const float textX = x + iconSize + 14.0f;
    const float textW = std::max(1.0f, width - iconSize - 14.0f);
    const float progress =
        prompt.GetExpectedSize() > 0
            ? std::clamp(
                static_cast<float>(
                    static_cast<double>(prompt.GetReceivedSize())
                    / static_cast<double>(prompt.GetExpectedSize())),
                0.0f,
                1.0f)
            : 0.0f;
    const bool failed =
        prompt.GetState() == relaydesk::runtime::AppUpdatePromptState::Failed;

    rect(ui, "app.update.file.icon.bg", x, y + 4.0f, iconSize, iconSize,
         {0.060f, 0.070f, 0.078f, 1.0f}, 7.0f);
    rect(ui, "app.update.file.icon.mark", x + iconSize - 12.0f, y + 19.0f,
         9.0f, 9.0f, failed ? kDanger : kGreen, 5.0f);
    icon(ui, "app.update.file.icon", x + 1.0f, y + 5.0f, iconSize - 2.0f,
         0xE895, {1.0f, 1.0f, 1.0f, 1.0f});
    text(ui, "app.update.file.name", textX, y, textW, 26.0f, fileName, 15.0f,
         kText);
    text(ui, "app.update.file.detail", textX, y + 30.0f, textW, 22.0f,
         appUpdateDownloadDetail(prompt), 12.0f, kMutedText);

    const float trackY = y + 61.0f;
    rect(ui, "app.update.file.progress.track", textX, trackY, textW, 4.0f,
         {0.900f, 0.910f, 0.920f, 1.0f}, 2.0f);
    rect(ui, "app.update.file.progress.fill", textX, trackY, textW * progress,
         4.0f, failed ? kDanger : kText, 2.0f);
}

void drawAppUpdatePromptOverlay(eui::Ui& ui,
                                float width,
                                float height,
                                relaydesk::runtime::RelayDeskRuntime& runtime)
{
    const std::optional<relaydesk::runtime::AppUpdatePrompt> prompt =
        runtime.GetAppUpdatePrompt();
    if (!prompt.has_value()) {
        return;
    }

    const auto state = prompt->GetState();
    const bool downloading =
        state == relaydesk::runtime::AppUpdatePromptState::Downloading;
    const bool failed = state == relaydesk::runtime::AppUpdatePromptState::Failed;
    const float panelWidth = std::min(width - 48.0f, 540.0f);
    const float panelHeight = failed ? 288.0f : (downloading ? 214.0f : 238.0f);
    const float panelX = std::max(24.0f, (width - panelWidth) * 0.5f);
    const float panelY = std::max(24.0f, (height - panelHeight) * 0.5f);
    const float contentX = panelX + 26.0f;
    const float contentW = panelWidth - 52.0f;
    const std::string sourceName =
        prompt->GetSourceDisplayName().empty()
            ? "附近设备"
            : prompt->GetSourceDisplayName();
    const std::string versionText =
        "当前版本 " + std::to_string(relaydesk::core::kAppVersion)
        + "，可更新到 " + std::to_string(prompt->GetAppVersion());

    ui.stack("app.update.prompt.overlay")
        .position(0.0f, 0.0f)
        .size(width, height)
        .zIndex(1300)
        .content([&] {
            ui.rect("app.update.prompt.backdrop")
                .position(0.0f, 0.0f)
                .size(width, height)
                .color({0.0f, 0.0f, 0.0f, 0.38f})
                .onClick([] {})
                .build();
            rect(ui, "app.update.prompt.panel", panelX, panelY, panelWidth,
                 panelHeight, kPanelBackground, 8.0f, kBorder);
            ui.rect("app.update.prompt.panel.hit")
                .position(panelX, panelY)
                .size(panelWidth, panelHeight)
                .color({0.0f, 0.0f, 0.0f, 0.0f})
                .onClick([] {})
                .build();

            if (downloading) {
                text(ui, "app.update.prompt.title", contentX, panelY + 22.0f,
                     contentW, 28.0f, "正在下载更新", 19.0f, kText);
                drawAppUpdateDownloadBlock(ui, prompt.value(), contentX,
                                           panelY + 72.0f, contentW);
                return;
            }

            if (failed) {
                text(ui, "app.update.prompt.title", contentX, panelY + 22.0f,
                     contentW, 28.0f, "更新失败", 19.0f, kText);
                drawAppUpdateDownloadBlock(ui, prompt.value(), contentX,
                                           panelY + 70.0f, contentW);
                paragraphText(ui,
                              "app.update.prompt.error",
                              contentX,
                              panelY + 150.0f,
                              contentW,
                              48.0f,
                              prompt->GetErrorMessage().empty()
                                  ? "更新没有完成，可以关闭这次提示后稍后重试。"
                                  : prompt->GetErrorMessage(),
                              13.0f,
                              20.0f,
                              kDanger);
                drawSettingsButton(ui,
                                   "app.update.prompt.close",
                                   panelX + panelWidth - 126.0f,
                                   panelY + panelHeight - 58.0f,
                                   100.0f,
                                   38.0f,
                                   "关闭",
                                   false,
                                   [&runtime] {
                                       runtime.dismissAppUpdatePrompt();
                                   });
                return;
            }

            text(ui, "app.update.prompt.title", contentX, panelY + 22.0f,
                 contentW, 28.0f, "发现新版本", 19.0f, kText);
            paragraphText(ui,
                          "app.update.prompt.message",
                          contentX,
                          panelY + 62.0f,
                          contentW,
                          60.0f,
                          sourceName + " 提供了更新后的 RelayDesk。",
                          14.0f,
                          22.0f,
                          kMutedText);
            text(ui, "app.update.prompt.version", contentX, panelY + 116.0f,
                 contentW, 24.0f, versionText, 13.0f, kText);
            drawSettingsButton(ui,
                               "app.update.prompt.defer",
                               panelX + panelWidth - 304.0f,
                               panelY + panelHeight - 58.0f,
                               132.0f,
                               38.0f,
                               "关闭后更新",
                               false,
                               [&runtime] {
                                   runtime.startAppUpdate(
                                       relaydesk::runtime::AppUpdateInstallMode::
                                           InstallOnExit);
                               });
            drawSettingsButton(ui,
                               "app.update.prompt.restart",
                               panelX + panelWidth - 158.0f,
                               panelY + panelHeight - 58.0f,
                               132.0f,
                               38.0f,
                               "更新并重启",
                               true,
                               [&runtime] {
                                   runtime.startAppUpdate(
                                       relaydesk::runtime::AppUpdateInstallMode::
                                           RestartNow);
                               });
        })
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

    bool& settingsOpen = ui.state<bool>("settings.open");
    if (settingsOpen) {
        drawSettingsPage(ui,
                         layout.chatX,
                         kContentTop,
                         layout.chatWidth,
                         layout.contentHeight,
                         runtime);
        drawImagePreviewOverlay(ui, layout.width, layout.height);
        drawAppUpdatePromptOverlay(ui, layout.width, layout.height, runtime);
        return;
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

    drawImagePreviewOverlay(ui, layout.width, layout.height);
    drawAppUpdatePromptOverlay(ui, layout.width, layout.height, runtime);
}

#if defined(_WIN32)

constexpr int kRelayDeskAppIconResourceId = 1;
constexpr int kRelayDeskNotificationSoundResourceId = 2;

struct RelayDeskIconWindowSearchContext {
    HWND window = nullptr;
};

BOOL CALLBACK relayDeskMainWindowIconEnumProc(HWND window, LPARAM contextAddress)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId() || !IsWindowVisible(window)) {
        return TRUE;
    }

    wchar_t title[256]{};
    GetWindowTextW(window,
                   title,
                   static_cast<int>(sizeof(title) / sizeof(title[0])));
    const std::wstring titleText(title);
    if (titleText.rfind(L"RelayDesk", 0) != 0) {
        return TRUE;
    }

    auto* context = reinterpret_cast<RelayDeskIconWindowSearchContext*>(contextAddress);
    context->window = window;
    return FALSE;
}

HWND findRelayDeskMainWindowForIcon()
{
    RelayDeskIconWindowSearchContext context;
    EnumWindows(relayDeskMainWindowIconEnumProc,
                reinterpret_cast<LPARAM>(&context));
    return context.window;
}

HWND relayDeskMainWindow()
{
    static std::mutex windowMutex;
    static HWND cachedWindow = nullptr;
    std::lock_guard lock(windowMutex);
    if (cachedWindow != nullptr && IsWindow(cachedWindow)) {
        return cachedWindow;
    }

    cachedWindow = findRelayDeskMainWindowForIcon();
    return cachedWindow;
}

bool isRelayDeskMainWindowActive(HWND window)
{
    if (window == nullptr) {
        return true;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground == window) {
        return true;
    }
    return foreground != nullptr && GetAncestor(foreground, GA_ROOT) == window;
}

void setRelayDeskTaskbarFlash(HWND window, bool enabled)
{
    if (window == nullptr) {
        return;
    }

    FLASHWINFO flashInfo{};
    flashInfo.cbSize = sizeof(flashInfo);
    flashInfo.hwnd = window;
    flashInfo.dwFlags = enabled
        ? static_cast<DWORD>(FLASHW_TRAY | FLASHW_TIMERNOFG)
        : static_cast<DWORD>(FLASHW_STOP);
    flashInfo.uCount = 0;
    flashInfo.dwTimeout = 0;
    (void)FlashWindowEx(&flashInfo);
}

void playRelayDeskNotificationSound()
{
    if (!notificationSoundEnabledFlag().load(std::memory_order_relaxed)) {
        return;
    }

    (void)PlaySoundW(MAKEINTRESOURCEW(kRelayDeskNotificationSoundResourceId),
                     GetModuleHandleW(nullptr),
                     SND_RESOURCE | SND_ASYNC | SND_NODEFAULT | SND_SYSTEM);
}

void setRelayDeskTrayAttention(bool enabled);

void handleIncomingUserNotification()
{
    playRelayDeskNotificationSound();

    HWND window = relayDeskMainWindow();
    if (window == nullptr || !IsWindowVisible(window)) {
        core::platform::requestTrayShowMinimized();
    }
    if (!isRelayDeskMainWindowActive(window)) {
        setRelayDeskTaskbarFlash(window, true);
        setRelayDeskTrayAttention(true);
    }
}

void ensureUserNotificationHandlerInstalled(
    relaydesk::runtime::RelayDeskRuntime& runtime)
{
    static bool installed = false;
    if (installed) {
        return;
    }

    runtime.SetUserNotificationHandler([] {
        handleIncomingUserNotification();
    });
    installed = true;
}

void processPendingUserNotifications(
    relaydesk::runtime::RelayDeskRuntime& runtime)
{
    HWND window = relayDeskMainWindow();
    if (isRelayDeskMainWindowActive(window)) {
        setRelayDeskTaskbarFlash(window, false);
        setRelayDeskTrayAttention(false);
    }

    (void)runtime.ConsumePendingUserNotificationCount();
}

HICON loadRelayDeskIcon(int width, int height)
{
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                         MAKEINTRESOURCEW(kRelayDeskAppIconResourceId),
                                         IMAGE_ICON,
                                         width,
                                         height,
                                         LR_DEFAULTCOLOR));
}

struct RelayDeskTrayWindowSearchContext {
    HWND window = nullptr;
};

BOOL CALLBACK relayDeskTrayWindowEnumProc(HWND window, LPARAM contextAddress)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) {
        return TRUE;
    }

    wchar_t className[64]{};
    const int classLength = GetClassNameW(
        window,
        className,
        static_cast<int>(sizeof(className) / sizeof(className[0])));
    if (classLength <= 0 || std::wstring(className) != L"RelayDeskTrayWindow") {
        return TRUE;
    }

    auto* context =
        reinterpret_cast<RelayDeskTrayWindowSearchContext*>(contextAddress);
    context->window = window;
    return FALSE;
}

HWND findRelayDeskTrayWindow()
{
    RelayDeskTrayWindowSearchContext context;
    EnumWindows(relayDeskTrayWindowEnumProc, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

HICON createTransparentIcon()
{
    constexpr int kIconSize = 16;
    std::array<BYTE, (kIconSize * kIconSize) / 8> andMask{};
    std::array<BYTE, (kIconSize * kIconSize) / 8> xorMask{};
    andMask.fill(0xFF);
    return CreateIcon(GetModuleHandleW(nullptr),
                      kIconSize,
                      kIconSize,
                      1,
                      1,
                      andMask.data(),
                      xorMask.data());
}

void updateRelayDeskTrayIcon(HICON icon)
{
    HWND trayWindow = findRelayDeskTrayWindow();
    if (trayWindow == nullptr || icon == nullptr) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = trayWindow;
    data.uID = 0;
    data.uFlags = NIF_ICON;
    data.hIcon = icon;
    (void)Shell_NotifyIconW(NIM_MODIFY, &data);
}

std::atomic_bool& relayDeskTrayAttentionEnabled()
{
    static std::atomic_bool enabled{false};
    return enabled;
}

std::atomic_bool& relayDeskTrayAttentionWorkerRunning()
{
    static std::atomic_bool running{false};
    return running;
}

void runRelayDeskTrayAttentionWorker()
{
    const HICON normalIcon = loadRelayDeskIcon(GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON));
    const HICON transparentIcon = createTransparentIcon();
    bool showTransparent = false;

    while (relayDeskTrayAttentionEnabled().load(std::memory_order_relaxed)) {
        updateRelayDeskTrayIcon(showTransparent ? transparentIcon : normalIcon);
        showTransparent = !showTransparent;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (normalIcon != nullptr) {
        updateRelayDeskTrayIcon(normalIcon);
        DestroyIcon(normalIcon);
    }
    if (transparentIcon != nullptr) {
        DestroyIcon(transparentIcon);
    }

    relayDeskTrayAttentionWorkerRunning().store(false, std::memory_order_relaxed);
}

void setRelayDeskTrayAttention(bool enabled)
{
    relayDeskTrayAttentionEnabled().store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        return;
    }

    bool expected = false;
    if (relayDeskTrayAttentionWorkerRunning().compare_exchange_strong(
            expected, true, std::memory_order_relaxed)) {
        std::thread(runRelayDeskTrayAttentionWorker).detach();
    }
}

void applyEmbeddedWindowIconOnce()
{
    static bool applied = false;
    static HICON largeIcon = nullptr;
    static HICON smallIcon = nullptr;
    if (applied) {
        return;
    }

    HWND window = relayDeskMainWindow();
    if (window == nullptr) {
        return;
    }

    if (largeIcon == nullptr) {
        largeIcon = loadRelayDeskIcon(GetSystemMetrics(SM_CXICON),
                                      GetSystemMetrics(SM_CYICON));
    }
    if (smallIcon == nullptr) {
        smallIcon = loadRelayDeskIcon(GetSystemMetrics(SM_CXSMICON),
                                      GetSystemMetrics(SM_CYSMICON));
    }

    if (largeIcon != nullptr) {
        SendMessageW(window,
                     WM_SETICON,
                     ICON_BIG,
                     reinterpret_cast<LPARAM>(largeIcon));
        SetClassLongPtrW(window,
                         GCLP_HICON,
                         reinterpret_cast<LONG_PTR>(largeIcon));
    }
    if (smallIcon != nullptr) {
        SendMessageW(window,
                     WM_SETICON,
                     ICON_SMALL,
                     reinterpret_cast<LPARAM>(smallIcon));
#if defined(ICON_SMALL2)
        SendMessageW(window,
                     WM_SETICON,
                     ICON_SMALL2,
                     reinterpret_cast<LPARAM>(smallIcon));
#endif
        SetClassLongPtrW(window,
                         GCLP_HICONSM,
                         reinterpret_cast<LONG_PTR>(smallIcon));
    }

    applied = true;
}

#else

void ensureUserNotificationHandlerInstalled(
    relaydesk::runtime::RelayDeskRuntime& runtime)
{
    runtime.SetUserNotificationHandler([] {});
}

void processPendingUserNotifications(
    relaydesk::runtime::RelayDeskRuntime& runtime)
{
    (void)runtime.ConsumePendingUserNotificationCount();
}

#endif

const char* relayDeskTrayIconPath()
{
    static const std::string iconPath = [] {
        try {
            const auto appPaths = relaydesk::storage::createAppPaths();
            return filesystemPathToUtf8String(appPaths.GetExecutablePath());
        } catch (const std::exception&) {
            return std::string{};
        }
    }();
    return iconPath.c_str();
}

} // namespace

namespace app {

const DslAppConfig& dslAppConfig()
{
    ensureAppStorage();
    syncLaunchAtStartupOnAppStart();
    static const DslAppConfig config = DslAppConfig{}
        .title("RelayDesk")
        .pageId("relaydesk")
        .clearColor(kWindowBackground)
        .iconPath("")
        .tray(true)
        .trayTitle("RelayDesk")
        .trayIcon(relayDeskTrayIconPath())
        .textFont("C:/Windows/Fonts/msyh.ttc")
        .iconFont("C:/Windows/Fonts/segmdl2.ttf")
        .windowSize(1940, 1224)
        .showDebugStatsInTitle(false)
        .fps(90.0);
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen)
{
#if defined(_WIN32)
    applyEmbeddedWindowIconOnce();
#endif

    auto& runtime = relaydesk::runtime::getRelayDeskRuntime();
    bool& notificationSoundEnabled = notificationSoundEnabledState(ui);
    notificationSoundEnabledFlag().store(notificationSoundEnabled,
                                         std::memory_order_relaxed);
    ensureUserNotificationHandlerInstalled(runtime);
    processPendingUserNotifications(runtime);

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
