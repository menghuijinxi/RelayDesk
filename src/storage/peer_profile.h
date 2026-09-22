#pragma once

#include "storage/app_paths.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace relaydesk::storage {

class PeerProfile {
public:
    int GetSchemaVersion() const { return schemaVersion_; }
    const std::string& GetDeviceId() const { return deviceId_; }
    const std::string& GetHostName() const { return hostName_; }
    const std::string& GetDisplayName() const { return displayName_; }
    const std::vector<std::string>& GetLastAddresses() const { return lastAddresses_; }
    std::uint16_t GetTcpPort() const { return tcpPort_; }
    int GetAppVersion() const { return appVersion_; }
    int GetUnreadMessageCount() const { return unreadMessageCount_; }
    const std::vector<std::string>& GetCapabilities() const { return capabilities_; }
    const std::string& GetFirstSeenAt() const { return firstSeenAt_; }
    const std::string& GetLastSeenAt() const { return lastSeenAt_; }
    const std::string& GetAvatarSha256() const { return avatarSha256_; }

    void SetDeviceId(std::string deviceId) { deviceId_ = std::move(deviceId); }
    void SetHostName(std::string hostName) { hostName_ = std::move(hostName); }
    void SetDisplayName(std::string displayName)
    {
        displayName_ = std::move(displayName);
    }
    void SetLastAddresses(std::vector<std::string> lastAddresses)
    {
        lastAddresses_ = std::move(lastAddresses);
    }
    void SetTcpPort(std::uint16_t tcpPort) { tcpPort_ = tcpPort; }
    void SetAppVersion(int appVersion) { appVersion_ = appVersion; }
    void SetUnreadMessageCount(int unreadMessageCount)
    {
        unreadMessageCount_ = unreadMessageCount;
    }
    void SetCapabilities(std::vector<std::string> capabilities)
    {
        capabilities_ = std::move(capabilities);
    }
    void SetFirstSeenAt(std::string firstSeenAt)
    {
        firstSeenAt_ = std::move(firstSeenAt);
    }
    void SetLastSeenAt(std::string lastSeenAt)
    {
        lastSeenAt_ = std::move(lastSeenAt);
    }
    void SetAvatarSha256(std::string avatarSha256)
    {
        avatarSha256_ = std::move(avatarSha256);
    }

protected:
    int schemaVersion_ = 1;
    std::string deviceId_;
    std::string hostName_;
    std::string displayName_;
    std::vector<std::string> lastAddresses_;
    std::uint16_t tcpPort_ = 0;
    int appVersion_ = 0;
    int unreadMessageCount_ = 0;
    std::vector<std::string> capabilities_;
    std::string firstSeenAt_;
    std::string lastSeenAt_;
    std::string avatarSha256_;
};

std::filesystem::path getPeerProfileFilePath(const AppPaths& appPaths,
                                             const std::string& peerDeviceId);
void savePeerProfile(const AppPaths& appPaths, const PeerProfile& profile);
PeerProfile loadPeerProfile(const AppPaths& appPaths, const std::string& peerDeviceId);
std::vector<PeerProfile> loadPeerProfiles(const AppPaths& appPaths);

}
