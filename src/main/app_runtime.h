#pragma once

#include "storage/peer_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
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
    std::chrono::steady_clock::time_point GetLastOnlineSignalAt() const
    {
        return lastOnlineSignalAt_;
    }
    bool GetOnline() const { return online_; }

    void SetDeviceId(std::string deviceId);
    void SetDisplayName(std::string displayName);
    void SetHostName(std::string hostName);
    void SetAddress(std::string address);
    void SetLastSeenAt(std::string lastSeenAt);
    void SetLastOnlineSignalAt(std::chrono::steady_clock::time_point signalAt);
    void SetOnline(bool online);

protected:
    std::string deviceId_;
    std::string displayName_;
    std::string hostName_;
    std::string address_;
    std::string lastSeenAt_;
    std::chrono::steady_clock::time_point lastOnlineSignalAt_{};
    bool online_ = false;
};

class PendingPeerProfile {
public:
    PendingPeerProfile(relaydesk::storage::PeerProfile profile, bool online);

    const relaydesk::storage::PeerProfile& GetProfile() const { return profile_; }
    bool GetOnline() const { return online_; }

protected:
    relaydesk::storage::PeerProfile profile_;
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
    std::optional<PeerListItem> GetSelectedPeer() const;
    const std::string& GetSelectedPeerDeviceId() const { return selectedPeerDeviceId_; }
    const std::string& GetStartupErrorMessage() const { return startupErrorMessage_; }
    bool GetStorageAvailable() const { return storageAvailable_; }
    bool GetDiscoveryStarted() const { return discoveryStarted_; }
    std::uint16_t GetDiscoveryUdpPort() const { return discoveryUdpPort_; }

    void refreshPeersIfNeeded();
    void selectPeer(std::string deviceId);

protected:
    void initialize();
    void refreshPeers();
    void enqueuePeerProfile(relaydesk::storage::PeerProfile profile,
                            bool online);
    void drainPendingPeerProfiles();
    void applyPeerProfile(const relaydesk::storage::PeerProfile& profile,
                          bool online,
                          std::chrono::steady_clock::time_point now);
    void refreshPeerOnlineStates();
    void syncSelectedPeer();
    void setStartupError(std::string errorMessage);
    void requestUiRefresh();
    void requestPeerStatusRefresh();
    void logDiagnostic(const std::string& message) const noexcept;

    LocalUserSummary localUser_;
    std::vector<PeerListItem> peers_;
    std::vector<PendingPeerProfile> pendingPeerProfiles_;
    std::string selectedPeerDeviceId_;
    std::string startupErrorMessage_;
    std::chrono::steady_clock::time_point nextPeerStatusRefreshAt_{};
    std::filesystem::path diagnosticLogFilePath_;
    std::mutex pendingPeerMutex_;
    std::atomic_bool uiRefreshPending_ = false;
    std::atomic_bool peerStatusRefreshPending_ = false;
    bool storageAvailable_ = false;
    bool discoveryStarted_ = false;
    std::uint16_t discoveryUdpPort_ = 0;
    std::unique_ptr<DiscoveryWorkerHandle> discoveryWorker_;
};

RelayDeskRuntime& getRelayDeskRuntime();

}
