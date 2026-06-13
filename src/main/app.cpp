#include "eui_neo.h"

#include "main/app_runtime.h"
#include "storage/app_paths.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
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
constexpr Color kGreen{0.250f, 0.660f, 0.160f, 1.0f};
constexpr Color kOffline{0.630f, 0.650f, 0.670f, 1.0f};
constexpr Color kAvatarGreen{0.080f, 0.600f, 0.440f, 1.0f};
constexpr float kContentTop = 0.0f;
constexpr float kChatHeaderHeight = 118.0f;
constexpr float kChatTimelineContentHeight = 650.0f;
constexpr float kComposerHeight = 68.0f;

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

void drawRuntimeChatTimeline(
    eui::Ui& ui,
    float x,
    float y,
    float width,
    float height,
    const std::optional<relaydesk::runtime::PeerListItem>& selectedPeer)
{
    rect(ui, "chat.bg", x, y, width, height, {1.0f, 1.0f, 1.0f, 1.0f});

    const std::string title = selectedPeer.has_value()
        ? "暂无本地消息"
        : "选择一个已发现设备";
    const std::string detail = selectedPeer.has_value()
        ? "TCP 会话实现后会在这里显示聊天记录"
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
    const std::optional<relaydesk::runtime::PeerListItem>& selectedPeer)
{
    const float horizontalPadding = width < 560.0f ? 12.0f : 16.0f;
    const float innerPadding = width < 560.0f ? 8.0f : 12.0f;
    const float sendWidth = width < 560.0f ? 58.0f : 70.0f;
    const float sendX = x + width - horizontalPadding - innerPadding - sendWidth;
    const float iconSize = width < 560.0f ? 34.0f : 38.0f;
    const float firstActionX = width < 560.0f ? sendX - 48.0f : x + width - 272.0f;
    const float inputWidth = std::max(
        140.0f,
        firstActionX - (x + 28.0f) - innerPadding);
    const std::string placeholder = selectedPeer.has_value()
        ? std::string("发给 ") + getPeerDisplayName(selectedPeer.value())
        : "请先选择设备";

    rect(ui, "composer.bg", x + horizontalPadding, y, width - horizontalPadding * 2.0f,
         kComposerHeight, kPanelBackground, 8.0f, kBorder);
    ui.stack("composer.input.pos")
        .position(x + 28.0f, y + 13.0f)
        .size(inputWidth, 42.0f)
        .content([&] {
            components::input(ui, "composer.input")
                .size(inputWidth, 42.0f)
                .placeholder(placeholder)
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
    rect(ui, "composer.send.bg", sendX, y + 13.0f, sendWidth, 42.0f,
         selectedPeer.has_value() ? kTeal : kOffline, 6.0f);
    text(ui, "composer.send.text", sendX, y + 21.0f, sendWidth, 24.0f, "发送",
         14.0f, {1.0f, 1.0f, 1.0f, 1.0f}, eui::HorizontalAlign::Center);
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
        "最后发现",
        "设备 ID",
    };
    const std::array values{
        selectedPeer.has_value() ? getPeerDisplayName(selectedPeer.value()) : "-",
        selectedPeer.has_value() ? selectedPeer->GetHostName() : "-",
        selectedPeer.has_value() ? selectedPeer->GetAddress() : "-",
        selectedPeer.has_value() ? selectedPeer->GetLastSeenAt() : "-",
        selectedPeer.has_value() ? selectedPeer->GetDeviceId() : "-",
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

    rect(ui, "details.sep.2", x, kContentTop + 417.0f, width, 1.0f, kBorder);
    text(ui, "details.transfers.title", x + 22.0f, kContentTop + 441.0f,
         width - 44.0f, 26.0f, "传输", 15.0f);
    text(ui, "details.transfers.empty", x + 22.0f, kContentTop + 485.0f,
         width - 44.0f, 24.0f, "暂无活动传输", 13.0f, kMutedText);
}

void drawRelayDesk(eui::Ui& ui,
                   const eui::Screen& screen,
                   relaydesk::runtime::RelayDeskRuntime& runtime)
{
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
                            selectedPeer);
    drawRuntimeComposer(ui, layout.chatX, composerY, layout.chatWidth, selectedPeer);

    if (layout.showDetails) {
        drawRuntimeDetails(ui,
                           layout.detailX,
                           layout.detailWidth,
                           layout.contentHeight,
                           selectedPeer);
    }
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
