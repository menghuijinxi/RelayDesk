#pragma once

#include "storage/history_store.h"
#include "storage/peer_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace relaydesk::net {
class PeerFrame;
}

namespace relaydesk::runtime {

class DiscoveryWorkerHandle;
class TcpPeerTransportHandle;

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
    const std::string& GetLastConversationAt() const
    {
        return lastConversationAt_;
    }
    std::uint16_t GetTcpPort() const { return tcpPort_; }
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
    void SetLastConversationAt(std::string lastConversationAt);
    void SetTcpPort(std::uint16_t tcpPort);
    void SetLastOnlineSignalAt(std::chrono::steady_clock::time_point signalAt);
    void SetOnline(bool online);

protected:
    std::string deviceId_;
    std::string displayName_;
    std::string hostName_;
    std::string address_;
    std::string lastSeenAt_;
    std::string lastConversationAt_;
    std::uint16_t tcpPort_ = 0;
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

class PendingIncomingTransfer {
public:
    const std::string& GetSenderDeviceId() const { return senderDeviceId_; }
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    const std::string& GetFileName() const { return fileName_; }
    std::uintmax_t GetExpectedSize() const { return expectedSize_; }
    std::uintmax_t GetReceivedSize() const { return receivedSize_; }
    const std::filesystem::path& GetTempFilePath() const { return tempFilePath_; }
    bool GetImageTransfer() const { return imageTransfer_; }

    void SetSenderDeviceId(std::string senderDeviceId)
    {
        senderDeviceId_ = std::move(senderDeviceId);
    }
    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId)
    {
        transferId_ = std::move(transferId);
    }
    void SetFileName(std::string fileName) { fileName_ = std::move(fileName); }
    void SetExpectedSize(std::uintmax_t expectedSize) { expectedSize_ = expectedSize; }
    void SetReceivedSize(std::uintmax_t receivedSize) { receivedSize_ = receivedSize; }
    void SetTempFilePath(std::filesystem::path tempFilePath)
    {
        tempFilePath_ = std::move(tempFilePath);
    }
    void SetImageTransfer(bool imageTransfer) { imageTransfer_ = imageTransfer; }

protected:
    std::string senderDeviceId_;
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string fileName_;
    std::uintmax_t expectedSize_ = 0;
    std::uintmax_t receivedSize_ = 0;
    std::filesystem::path tempFilePath_;
    bool imageTransfer_ = false;
};

class PendingTransferUpdate {
public:
    const std::string& GetPeerDeviceId() const { return peerDeviceId_; }
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    const std::string& GetFileName() const { return fileName_; }
    std::uintmax_t GetFileSize() const { return fileSize_; }
    const std::string& GetLocalPath() const { return localPath_; }
    const std::optional<std::string>& GetSha256() const { return sha256_; }
    relaydesk::storage::TransferState GetTransferState() const
    {
        return transferState_;
    }

    void SetPeerDeviceId(std::string peerDeviceId)
    {
        peerDeviceId_ = std::move(peerDeviceId);
    }
    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId)
    {
        transferId_ = std::move(transferId);
    }
    void SetFileName(std::string fileName) { fileName_ = std::move(fileName); }
    void SetFileSize(std::uintmax_t fileSize) { fileSize_ = fileSize; }
    void SetLocalPath(std::string localPath) { localPath_ = std::move(localPath); }
    void SetSha256(std::string sha256) { sha256_ = std::move(sha256); }
    void SetTransferState(relaydesk::storage::TransferState transferState)
    {
        transferState_ = transferState;
    }

protected:
    std::string peerDeviceId_;
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string fileName_;
    std::uintmax_t fileSize_ = 0;
    std::string localPath_;
    std::optional<std::string> sha256_;
    relaydesk::storage::TransferState transferState_ =
        relaydesk::storage::TransferState::Pending;
};

class RelayDeskRuntime {
public:
    RelayDeskRuntime();
    ~RelayDeskRuntime();

    RelayDeskRuntime(const RelayDeskRuntime&) = delete;
    RelayDeskRuntime& operator=(const RelayDeskRuntime&) = delete;

    const LocalUserSummary& GetLocalUser() const { return localUser_; }
    const std::vector<PeerListItem>& GetPeers() const { return peers_; }
    const std::vector<relaydesk::storage::ChatMessageRecord>&
    GetSelectedPeerMessages() const
    {
        return selectedPeerMessages_;
    }
    std::optional<PeerListItem> GetSelectedPeer() const;
    const std::string& GetSelectedPeerDeviceId() const { return selectedPeerDeviceId_; }
    const std::string& GetStartupErrorMessage() const { return startupErrorMessage_; }
    bool GetStorageAvailable() const { return storageAvailable_; }
    bool GetDiscoveryStarted() const { return discoveryStarted_; }
    std::uint16_t GetDiscoveryUdpPort() const { return discoveryUdpPort_; }

    void refreshPeersIfNeeded();
    void selectPeer(std::string deviceId);
    void sendMessagePartsToSelectedPeer(
        std::vector<relaydesk::storage::ChatMessagePart> parts);
    void resendSelectedPeerMessage(const std::string& messageId);
    void sendTextMessageToSelectedPeer(std::string text);

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
    std::string loadPeerLastConversationAtOrEmpty(
        const relaydesk::storage::AppPaths& appPaths,
        const std::string& peerDeviceId) const;
    void sortPeers();
    void updatePeerLastConversationAt(const std::string& peerDeviceId,
                                      const std::string& lastConversationAt);
    void syncSelectedPeer();
    void setSelectedPeerDeviceId(std::string deviceId);
    void loadSelectedPeerMessages();
    void enqueueIncomingChatMessage(
        relaydesk::storage::ChatMessageRecord record);
    void handleIncomingPeerFrame(relaydesk::net::PeerFrame frame);
    void enqueueTransferUpdate(PendingTransferUpdate update);
    void drainPendingTransferUpdates();
    bool updateChatMessageTransferPart(
        const PendingTransferUpdate& update);
    void drainPendingChatMessages();
    void appendSelectedPeerMessage(
        const std::string& peerDeviceId,
        const relaydesk::storage::ChatMessageRecord& record);
    void sendOutgoingMessageRecordToPeer(
        relaydesk::storage::ChatMessageRecord record,
        const PeerListItem& peer,
        bool replaceExistingRecord);
    void persistChatMessageRecord(
        const std::string& peerDeviceId,
        const relaydesk::storage::ChatMessageRecord& record,
        bool replaceExistingRecord);
    void updateSelectedPeerMessageRecord(
        const relaydesk::storage::ChatMessageRecord& record);
    void setStartupError(std::string errorMessage);
    void requestUiRefresh();
    void requestPeerStatusRefresh();
    void logDiagnostic(const std::string& message) const noexcept;

    LocalUserSummary localUser_;
    std::vector<PeerListItem> peers_;
    std::vector<PendingPeerProfile> pendingPeerProfiles_;
    std::vector<relaydesk::storage::ChatMessageRecord> pendingChatMessages_;
    std::vector<PendingTransferUpdate> pendingTransferUpdates_;
    std::unordered_map<std::string, PendingIncomingTransfer>
        pendingIncomingTransfers_;
    std::vector<relaydesk::storage::ChatMessageRecord> selectedPeerMessages_;
    std::string selectedPeerDeviceId_;
    std::string startupErrorMessage_;
    std::chrono::steady_clock::time_point nextPeerStatusRefreshAt_{};
    std::filesystem::path diagnosticLogFilePath_;
    std::mutex pendingPeerMutex_;
    std::mutex pendingChatMutex_;
    std::mutex pendingTransferUpdateMutex_;
    std::mutex pendingTransferMutex_;
    std::atomic_bool uiRefreshPending_ = false;
    std::atomic_bool peerStatusRefreshPending_ = false;
    bool storageAvailable_ = false;
    bool discoveryStarted_ = false;
    std::uint16_t discoveryUdpPort_ = 0;
    std::unique_ptr<DiscoveryWorkerHandle> discoveryWorker_;
    std::unique_ptr<TcpPeerTransportHandle> tcpPeerTransport_;
};

RelayDeskRuntime& getRelayDeskRuntime();

}
