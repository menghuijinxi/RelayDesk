#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace relaydesk::net {

constexpr const char* kDiscoveryAnnouncementTypeHello = "hello";
constexpr const char* kDiscoveryAnnouncementTypeReply = "reply";
constexpr const char* kDiscoveryAnnouncementTypeOffline = "offline";

class DiscoveryAnnouncement {
public:
    int GetVersion() const { return version_; }
    const std::string& GetType() const { return type_; }
    const std::string& GetDeviceId() const { return deviceId_; }
    const std::string& GetHostName() const { return hostName_; }
    const std::string& GetDisplayName() const { return displayName_; }
    std::uint16_t GetTcpPort() const { return tcpPort_; }
    int GetAppVersion() const { return appVersion_; }
    const std::vector<std::string>& GetCapabilities() const { return capabilities_; }
    const std::string& GetTimestamp() const { return timestamp_; }
    bool GetAvatarSha256Specified() const { return avatarSha256Specified_; }
    const std::string& GetAvatarSha256() const { return avatarSha256_; }

    void SetVersion(int version) { version_ = version; }
    void SetType(std::string type) { type_ = std::move(type); }
    void SetDeviceId(std::string deviceId) { deviceId_ = std::move(deviceId); }
    void SetHostName(std::string hostName) { hostName_ = std::move(hostName); }
    void SetDisplayName(std::string displayName)
    {
        displayName_ = std::move(displayName);
    }
    void SetTcpPort(std::uint16_t tcpPort) { tcpPort_ = tcpPort; }
    void SetAppVersion(int appVersion) { appVersion_ = appVersion; }
    void SetCapabilities(std::vector<std::string> capabilities)
    {
        capabilities_ = std::move(capabilities);
    }
    void SetTimestamp(std::string timestamp) { timestamp_ = std::move(timestamp); }
    void SetAvatarSha256(std::string avatarSha256)
    {
        avatarSha256Specified_ = true;
        avatarSha256_ = std::move(avatarSha256);
    }

protected:
    int version_ = 1;
    std::string type_ = kDiscoveryAnnouncementTypeHello;
    std::string deviceId_;
    std::string hostName_;
    std::string displayName_;
    std::uint16_t tcpPort_ = 0;
    int appVersion_ = 0;
    std::vector<std::string> capabilities_;
    std::string timestamp_;
    bool avatarSha256Specified_ = false;
    std::string avatarSha256_;
};

std::string serializeDiscoveryAnnouncement(const DiscoveryAnnouncement& announcement);
DiscoveryAnnouncement parseDiscoveryAnnouncement(const std::string& payload);

}
