#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace relaydesk::runtime {

class DiscoveryWorkerHandle;

class LocalUserSummary {
public:
    const std::string& GetDisplayName() const { return displayName_; }
    const std::string& GetHostName() const { return hostName_; }
    const std::string& GetDeviceId() const { return deviceId_; }

    void SetDisplayName(std::string displayName);
    void SetHostName(std::string hostName);
    void SetDeviceId(std::string deviceId);

protected:
    std::string displayName_ = "RelayDesk";
    std::string hostName_ = "Local";
    std::string deviceId_;
};

class PeerListItem {
public:
    const std::string& GetDeviceId() const { return deviceId_; }
    const std::string& GetDisplayName() const { return displayName_; }
    const std::string& GetHostName() const { return hostName_; }
    const std::string& GetAddress() const { return address_; }
    const std::string& GetLastSeenAt() const { return lastSeenAt_; }
    bool GetOnline() const { return online_; }

    void SetDeviceId(std::string deviceId);
    void SetDisplayName(std::string displayName);
    void SetHostName(std::string hostName);
    void SetAddress(std::string address);
    void SetLastSeenAt(std::string lastSeenAt);
    void SetOnline(bool online);

protected:
    std::string deviceId_;
    std::string displayName_;
    std::string hostName_;
    std::string address_;
    std::string lastSeenAt_;
    bool online_ = false;
};

class RelayDeskRuntime {
public:
    RelayDeskRuntime();
    ~RelayDeskRuntime();

    RelayDeskRuntime(const RelayDeskRuntime&) = delete;
    RelayDeskRuntime& operator=(const RelayDeskRuntime&) = delete;

    const LocalUserSummary& GetLocalUser() const { return localUser_; }
    const std::vector<PeerListItem>& GetPeers() const { return peers_; }
    const std::string& GetStartupErrorMessage() const { return startupErrorMessage_; }
    bool GetStorageAvailable() const { return storageAvailable_; }
    bool GetDiscoveryStarted() const { return discoveryStarted_; }
    std::uint16_t GetDiscoveryUdpPort() const { return discoveryUdpPort_; }

    void refreshPeersIfNeeded();

protected:
    void initialize();
    void refreshPeers();
    void setStartupError(std::string errorMessage);

    LocalUserSummary localUser_;
    std::vector<PeerListItem> peers_;
    std::string startupErrorMessage_;
    std::chrono::steady_clock::time_point nextPeerRefreshAt_{};
    bool storageAvailable_ = false;
    bool discoveryStarted_ = false;
    std::uint16_t discoveryUdpPort_ = 0;
    std::unique_ptr<DiscoveryWorkerHandle> discoveryWorker_;
};

RelayDeskRuntime& getRelayDeskRuntime();

}
