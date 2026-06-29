#pragma once

#include "storage/history_store.h"
#include "storage/peer_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace relaydesk::net {
class AppUpdateRequestMessage;
class PeerFrame;
}

namespace relaydesk::runtime {

class DiscoveryWorkerHandle;
class TcpPeerTransportHandle;

class AppUpdateApplyOptions {
public:
    const std::filesystem::path& GetTargetPath() const { return targetPath_; }
    const std::filesystem::path& GetPayloadPath() const { return payloadPath_; }
    const std::filesystem::path& GetHelperPath() const { return helperPath_; }
    const std::filesystem::path& GetStartDirectory() const
    {
        return startDirectory_;
    }
    const std::filesystem::path& GetLogPath() const { return logPath_; }
    unsigned long GetTargetProcessId() const { return targetProcessId_; }
    bool GetRestartAfterApply() const { return restartAfterApply_; }

    void SetTargetPath(std::filesystem::path targetPath)
    {
        targetPath_ = std::move(targetPath);
    }
    void SetPayloadPath(std::filesystem::path payloadPath)
    {
        payloadPath_ = std::move(payloadPath);
    }
    void SetHelperPath(std::filesystem::path helperPath)
    {
        helperPath_ = std::move(helperPath);
    }
    void SetStartDirectory(std::filesystem::path startDirectory)
    {
        startDirectory_ = std::move(startDirectory);
    }
    void SetLogPath(std::filesystem::path logPath)
    {
        logPath_ = std::move(logPath);
    }
    void SetTargetProcessId(unsigned long targetProcessId)
    {
        targetProcessId_ = targetProcessId;
    }
    void SetRestartAfterApply(bool restartAfterApply)
    {
        restartAfterApply_ = restartAfterApply;
    }

protected:
    std::filesystem::path targetPath_;
    std::filesystem::path payloadPath_;
    std::filesystem::path helperPath_;
    std::filesystem::path startDirectory_;
    std::filesystem::path logPath_;
    unsigned long targetProcessId_ = 0;
    bool restartAfterApply_ = false;
};

std::optional<AppUpdateApplyOptions> parseAppUpdateApplyOptions(
    const std::vector<std::wstring>& arguments);
int runAppUpdateApplyMode(const AppUpdateApplyOptions& options) noexcept;

enum class AppUpdateInstallMode {
    RestartNow,
    InstallOnExit,
};

enum class AppUpdatePromptState {
    Available,
    Downloading,
    Failed,
};

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
    int GetUnreadMessageCount() const { return unreadMessageCount_; }
    std::uint16_t GetTcpPort() const { return tcpPort_; }
    int GetAppVersion() const { return appVersion_; }
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
    void SetUnreadMessageCount(int unreadMessageCount);
    void SetTcpPort(std::uint16_t tcpPort);
    void SetAppVersion(int appVersion);
    void SetLastOnlineSignalAt(std::chrono::steady_clock::time_point signalAt);
    void SetOnline(bool online);

protected:
    std::string deviceId_;
    std::string displayName_;
    std::string hostName_;
    std::string address_;
    std::string lastSeenAt_;
    std::string lastConversationAt_;
    int unreadMessageCount_ = 0;
    std::uint16_t tcpPort_ = 0;
    int appVersion_ = 0;
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

class PendingChatMessage {
public:
    PendingChatMessage(relaydesk::storage::ChatMessageRecord record,
                       bool persisted,
                       bool unreadCounted);

    relaydesk::storage::ChatMessageRecord& GetRecord() { return record_; }
    const relaydesk::storage::ChatMessageRecord& GetRecord() const
    {
        return record_;
    }
    bool GetPersisted() const { return persisted_; }
    bool GetUnreadCounted() const { return unreadCounted_; }

protected:
    relaydesk::storage::ChatMessageRecord record_;
    bool persisted_ = false;
    bool unreadCounted_ = false;
};

class AppUpdatePrompt {
public:
    const std::string& GetSourceDeviceId() const { return sourceDeviceId_; }
    const std::string& GetSourceDisplayName() const { return sourceDisplayName_; }
    const std::string& GetFileName() const { return fileName_; }
    int GetAppVersion() const { return appVersion_; }
    AppUpdatePromptState GetState() const { return state_; }
    AppUpdateInstallMode GetInstallMode() const { return installMode_; }
    std::uintmax_t GetExpectedSize() const { return expectedSize_; }
    std::uintmax_t GetReceivedSize() const { return receivedSize_; }
    double GetBytesPerSecond() const { return bytesPerSecond_; }
    const std::string& GetErrorMessage() const { return errorMessage_; }

    void SetSourceDeviceId(std::string sourceDeviceId)
    {
        sourceDeviceId_ = std::move(sourceDeviceId);
    }
    void SetSourceDisplayName(std::string sourceDisplayName)
    {
        sourceDisplayName_ = std::move(sourceDisplayName);
    }
    void SetFileName(std::string fileName) { fileName_ = std::move(fileName); }
    void SetAppVersion(int appVersion) { appVersion_ = appVersion; }
    void SetState(AppUpdatePromptState state) { state_ = state; }
    void SetInstallMode(AppUpdateInstallMode installMode)
    {
        installMode_ = installMode;
    }
    void SetExpectedSize(std::uintmax_t expectedSize) { expectedSize_ = expectedSize; }
    void SetReceivedSize(std::uintmax_t receivedSize) { receivedSize_ = receivedSize; }
    void SetBytesPerSecond(double bytesPerSecond)
    {
        bytesPerSecond_ = bytesPerSecond;
    }
    void SetErrorMessage(std::string errorMessage)
    {
        errorMessage_ = std::move(errorMessage);
    }

protected:
    std::string sourceDeviceId_;
    std::string sourceDisplayName_;
    std::string fileName_;
    int appVersion_ = 0;
    AppUpdatePromptState state_ = AppUpdatePromptState::Available;
    AppUpdateInstallMode installMode_ = AppUpdateInstallMode::RestartNow;
    std::uintmax_t expectedSize_ = 0;
    std::uintmax_t receivedSize_ = 0;
    double bytesPerSecond_ = 0.0;
    std::string errorMessage_;
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
    const std::filesystem::path& GetFinalFilePath() const { return finalFilePath_; }
    bool GetImageTransfer() const { return imageTransfer_; }
    bool GetFolderTransfer() const { return folderTransfer_; }

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
    void SetFinalFilePath(std::filesystem::path finalFilePath)
    {
        finalFilePath_ = std::move(finalFilePath);
    }
    void SetImageTransfer(bool imageTransfer) { imageTransfer_ = imageTransfer; }
    void SetFolderTransfer(bool folderTransfer) { folderTransfer_ = folderTransfer; }

protected:
    std::string senderDeviceId_;
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string fileName_;
    std::uintmax_t expectedSize_ = 0;
    std::uintmax_t receivedSize_ = 0;
    std::filesystem::path tempFilePath_;
    std::filesystem::path finalFilePath_;
    bool imageTransfer_ = false;
    bool folderTransfer_ = false;
};

class PendingIncomingAppUpdate {
public:
    const std::string& GetRequestId() const { return requestId_; }
    const std::string& GetSourceDeviceId() const { return sourceDeviceId_; }
    int GetAppVersion() const { return appVersion_; }
    const std::string& GetFileName() const { return fileName_; }
    std::uintmax_t GetExpectedSize() const { return expectedSize_; }
    std::uintmax_t GetReceivedSize() const { return receivedSize_; }
    const std::filesystem::path& GetTempFilePath() const { return tempFilePath_; }
    AppUpdateInstallMode GetInstallMode() const { return installMode_; }
    std::chrono::steady_clock::time_point GetStartedAt() const { return startedAt_; }

    void SetRequestId(std::string requestId) { requestId_ = std::move(requestId); }
    void SetSourceDeviceId(std::string sourceDeviceId)
    {
        sourceDeviceId_ = std::move(sourceDeviceId);
    }
    void SetAppVersion(int appVersion) { appVersion_ = appVersion; }
    void SetFileName(std::string fileName) { fileName_ = std::move(fileName); }
    void SetExpectedSize(std::uintmax_t expectedSize) { expectedSize_ = expectedSize; }
    void SetReceivedSize(std::uintmax_t receivedSize) { receivedSize_ = receivedSize; }
    void SetTempFilePath(std::filesystem::path tempFilePath)
    {
        tempFilePath_ = std::move(tempFilePath);
    }
    void SetInstallMode(AppUpdateInstallMode installMode)
    {
        installMode_ = installMode;
    }
    void SetStartedAt(std::chrono::steady_clock::time_point startedAt)
    {
        startedAt_ = startedAt;
    }

protected:
    std::string requestId_;
    std::string sourceDeviceId_;
    int appVersion_ = 0;
    std::string fileName_;
    std::uintmax_t expectedSize_ = 0;
    std::uintmax_t receivedSize_ = 0;
    std::filesystem::path tempFilePath_;
    AppUpdateInstallMode installMode_ = AppUpdateInstallMode::RestartNow;
    std::chrono::steady_clock::time_point startedAt_{};
};

class PendingOutgoingTransferRequest {
public:
    const std::string& GetReceiverDeviceId() const { return receiverDeviceId_; }
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    std::uintmax_t GetResumeOffset() const { return resumeOffset_; }
    bool GetResumeRequestOnly() const { return resumeRequestOnly_; }

    void SetReceiverDeviceId(std::string receiverDeviceId)
    {
        receiverDeviceId_ = std::move(receiverDeviceId);
    }
    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId)
    {
        transferId_ = std::move(transferId);
    }
    void SetResumeOffset(std::uintmax_t resumeOffset)
    {
        resumeOffset_ = resumeOffset;
    }
    void SetResumeRequestOnly(bool resumeRequestOnly)
    {
        resumeRequestOnly_ = resumeRequestOnly;
    }

protected:
    std::string receiverDeviceId_;
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::uintmax_t resumeOffset_ = 0;
    bool resumeRequestOnly_ = false;
};

class PendingTransferStateUpdate {
public:
    const std::string& GetPeerDeviceId() const { return peerDeviceId_; }
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
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
    void SetTransferState(relaydesk::storage::TransferState transferState)
    {
        transferState_ = transferState;
    }

protected:
    std::string peerDeviceId_;
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    relaydesk::storage::TransferState transferState_ =
        relaydesk::storage::TransferState::Pending;
};

class PendingTransferProgressUpdate {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    std::uintmax_t GetTransferredSize() const { return transferredSize_; }

    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId)
    {
        transferId_ = std::move(transferId);
    }
    void SetTransferredSize(std::uintmax_t transferredSize)
    {
        transferredSize_ = transferredSize;
    }

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::uintmax_t transferredSize_ = 0;
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

class RelayDeskRuntimeOptions {
public:
    RelayDeskRuntimeOptions();

    std::uint16_t GetTcpListenPort() const { return tcpListenPort_; }
    std::uint16_t GetDiscoveryUdpPort() const { return discoveryUdpPort_; }
    bool GetDiscoveryBroadcastEnabled() const
    {
        return discoveryBroadcastEnabled_;
    }
    bool GetDiscoveryAnnounceOnStart() const
    {
        return discoveryAnnounceOnStart_;
    }
    bool GetNetworkEnabled() const { return networkEnabled_; }
    bool GetAsyncRefreshEnabled() const { return asyncRefreshEnabled_; }

    void SetTcpListenPort(std::uint16_t tcpListenPort)
    {
        tcpListenPort_ = tcpListenPort;
    }
    void SetDiscoveryUdpPort(std::uint16_t discoveryUdpPort)
    {
        discoveryUdpPort_ = discoveryUdpPort;
    }
    void SetDiscoveryBroadcastEnabled(bool discoveryBroadcastEnabled)
    {
        discoveryBroadcastEnabled_ = discoveryBroadcastEnabled;
    }
    void SetDiscoveryAnnounceOnStart(bool discoveryAnnounceOnStart)
    {
        discoveryAnnounceOnStart_ = discoveryAnnounceOnStart;
    }
    void SetNetworkEnabled(bool networkEnabled)
    {
        networkEnabled_ = networkEnabled;
    }
    void SetAsyncRefreshEnabled(bool asyncRefreshEnabled)
    {
        asyncRefreshEnabled_ = asyncRefreshEnabled;
    }

protected:
    std::uint16_t tcpListenPort_ = 0;
    std::uint16_t discoveryUdpPort_ = 0;
    bool discoveryBroadcastEnabled_ = true;
    bool discoveryAnnounceOnStart_ = true;
    bool networkEnabled_ = true;
    bool asyncRefreshEnabled_ = true;
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
    bool GetSelectedPeerHasMoreMessages() const
    {
        return selectedPeerHasMoreMessages_;
    }
    std::optional<PeerListItem> GetSelectedPeer() const;
    const std::string& GetSelectedPeerDeviceId() const { return selectedPeerDeviceId_; }
    const std::string& GetStartupErrorMessage() const { return startupErrorMessage_; }
    bool GetStorageAvailable() const { return storageAvailable_; }
    bool GetDiscoveryStarted() const { return discoveryStarted_; }
    std::uint16_t GetDiscoveryUdpPort() const { return discoveryUdpPort_; }
    std::optional<AppUpdatePrompt> GetAppUpdatePrompt();
    bool GetAppUpdateExitRequested() const
    {
        return appUpdateExitRequested_.load(std::memory_order_relaxed);
    }
    void SetAppUpdateHelperLauncherForTest(
        std::function<bool(const AppUpdateApplyOptions&)> launcher)
    {
        appUpdateHelperLauncherForTest_ = std::move(launcher);
    }
    std::uint64_t ConsumePendingUserNotificationCount();
    void SetUserNotificationHandler(std::function<void()> handler);

    void updateLocalDisplayName(std::string displayName);
    void refreshPeersIfNeeded();
    void selectPeer(std::string deviceId);
    void loadMoreSelectedPeerMessages();
    bool loadSelectedPeerMessagesAround(const std::string& messageId);
    void sendMessagePartsToSelectedPeer(
        std::vector<relaydesk::storage::ChatMessagePart> parts);
    void resendSelectedPeerMessage(const std::string& messageId);
    void acceptSelectedPeerFileTransfer(const std::string& messageId,
                                        const std::string& partId,
                                        bool overwriteExisting);
    void acceptSelectedPeerFileTransferAs(const std::string& messageId,
                                          const std::string& partId,
                                          std::filesystem::path finalPath);
    void sendSelectedPeerFileTransfer(const std::string& messageId,
                                      const std::string& partId);
    void rejectSelectedPeerFileTransfer(const std::string& messageId,
                                        const std::string& partId);
    void cancelSelectedPeerFileTransfer(const std::string& messageId,
                                        const std::string& partId);
    void sendTextMessageToSelectedPeer(std::string text);
    void startAppUpdate(AppUpdateInstallMode installMode);
    void dismissAppUpdatePrompt();

protected:
    explicit RelayDeskRuntime(RelayDeskRuntimeOptions options);

    void initialize();
    void refreshPeers();
    void enqueuePeerProfile(relaydesk::storage::PeerProfile profile,
                            bool online);
    void drainPendingPeerProfiles();
    void applyPeerProfile(const relaydesk::storage::PeerProfile& profile,
                          bool online,
                          std::chrono::steady_clock::time_point now);
    void maybeOfferAppUpdateFromPeer(const PeerListItem& peer);
    void requestAppUpdateFromPeer(const PeerListItem& peer,
                                  AppUpdateInstallMode installMode);
    void sendAppUpdatePackageToPeer(
        const PeerListItem& peer,
        const relaydesk::net::AppUpdateRequestMessage& request);
    void completeDownloadedAppUpdate(const PendingIncomingAppUpdate& update);
    AppUpdateApplyOptions prepareDownloadedAppUpdate(
        const PendingIncomingAppUpdate& update,
        bool restartAfterApply);
    bool launchAppUpdateHelper(const AppUpdateApplyOptions& options);
    void applyDownloadedAppUpdate(const PendingIncomingAppUpdate& update,
                                  bool restartAfterApply);
    void launchScheduledAppUpdateOnExit() noexcept;
    void markAppUpdateFailed(const std::string& sourceDeviceId,
                             int appVersion,
                             std::string errorMessage);
    std::optional<PeerListItem> findPeerByDeviceId(
        const std::string& peerDeviceId) const;
    void refreshPeerOnlineStates();
    std::string loadPeerLastConversationAtOrEmpty(
        const relaydesk::storage::AppPaths& appPaths,
        const std::string& peerDeviceId) const;
    void sortPeers();
    void updatePeerLastConversationAt(const std::string& peerDeviceId,
                                      const std::string& lastConversationAt);
    void incrementPeerUnreadMessageCount(const std::string& peerDeviceId);
    void incrementPeerUnreadMessageCountInMemory(
        const std::string& peerDeviceId);
    void clearPeerUnreadMessageCount(const std::string& peerDeviceId);
    void savePeerUnreadMessageCount(const std::string& peerDeviceId,
                                    int unreadMessageCount);
    bool incrementPersistedPeerUnreadMessageCount(
        const std::string& peerDeviceId);
    void syncSelectedPeer();
    void setSelectedPeerDeviceId(std::string deviceId);
    void loadSelectedPeerMessages();
    void enqueueIncomingChatMessage(
        relaydesk::storage::ChatMessageRecord record);
    void handleIncomingPeerFrame(relaydesk::net::PeerFrame frame);
    void failIncomingTransferFromFrame(const relaydesk::net::PeerFrame& frame);
    void interruptPendingIncomingTransfers();
    void notifyIncomingTransferFailed(
        const PendingIncomingTransfer& transfer);
    void acceptSelectedPeerFileTransferToPath(
        const std::string& messageId,
        const std::string& partId,
        std::filesystem::path finalPath,
        bool overwriteExisting);
    void enqueueTransferUpdate(PendingTransferUpdate update);
    void drainPendingTransferUpdates();
    bool updateChatMessageTransferPart(
        const PendingTransferUpdate& update);
    void enqueueOutgoingTransferRequest(PendingOutgoingTransferRequest request);
    void drainPendingOutgoingTransferRequests();
    void sendOutgoingTransferRequest(
        const PendingOutgoingTransferRequest& request);
    void enqueueTransferStateUpdate(PendingTransferStateUpdate update);
    void drainPendingTransferStateUpdates();
    bool updateChatMessageTransferState(
        const PendingTransferStateUpdate& update);
    void enqueueTransferProgressUpdate(PendingTransferProgressUpdate update);
    void drainPendingTransferProgressUpdates();
    bool updateChatMessageTransferProgress(
        const PendingTransferProgressUpdate& update);
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
    void notifyUserNotification();
    void setStartupError(std::string errorMessage);
    void requestUiRefresh();
    void requestPeerStatusRefresh();
    void logDiagnostic(const std::string& message) const noexcept;

    LocalUserSummary localUser_;
    RelayDeskRuntimeOptions runtimeOptions_;
    std::vector<PeerListItem> peers_;
    std::vector<PendingPeerProfile> pendingPeerProfiles_;
    std::vector<PendingChatMessage> pendingChatMessages_;
    std::vector<PendingTransferUpdate> pendingTransferUpdates_;
    std::vector<PendingOutgoingTransferRequest> pendingOutgoingTransferRequests_;
    std::vector<PendingTransferStateUpdate> pendingTransferStateUpdates_;
    std::vector<PendingTransferProgressUpdate> pendingTransferProgressUpdates_;
    std::unordered_map<std::string, PendingIncomingTransfer>
        pendingIncomingTransfers_;
    std::unordered_map<std::string, PendingIncomingAppUpdate>
        pendingIncomingAppUpdates_;
    std::unordered_map<std::string, int> requestedAppUpdateVersions_;
    std::unordered_map<std::string, int> dismissedAppUpdateVersions_;
    std::optional<AppUpdatePrompt> appUpdatePrompt_;
    std::optional<PendingIncomingAppUpdate> scheduledAppUpdate_;
    std::vector<relaydesk::storage::ChatMessageRecord> selectedPeerMessages_;
    std::string selectedPeerDeviceId_;
    std::string startupErrorMessage_;
    std::chrono::steady_clock::time_point nextPeerStatusRefreshAt_{};
    std::filesystem::path diagnosticLogFilePath_;
    std::function<void()> userNotificationHandler_;
    std::function<bool(const AppUpdateApplyOptions&)>
        appUpdateHelperLauncherForTest_;
    std::mutex pendingPeerMutex_;
    std::mutex pendingChatMutex_;
    std::mutex pendingTransferUpdateMutex_;
    std::mutex pendingOutgoingTransferRequestMutex_;
    std::mutex pendingTransferStateUpdateMutex_;
    std::mutex pendingTransferProgressUpdateMutex_;
    std::mutex pendingTransferMutex_;
    std::mutex pendingAppUpdateMutex_;
    std::mutex userNotificationMutex_;
    mutable std::mutex chatHistoryStorageMutex_;
    std::atomic_bool uiRefreshPending_ = false;
    std::atomic_bool peerStatusRefreshPending_ = false;
    std::atomic_bool appUpdateExitRequested_ = false;
    std::atomic<std::uint64_t> pendingUserNotificationCount_ = 0;
    bool storageAvailable_ = false;
    bool discoveryStarted_ = false;
    bool selectedPeerHasMoreMessages_ = false;
    std::uint16_t discoveryUdpPort_ = 0;
    std::unique_ptr<DiscoveryWorkerHandle> discoveryWorker_;
    std::unique_ptr<TcpPeerTransportHandle> tcpPeerTransport_;
};

RelayDeskRuntime& getRelayDeskRuntime();

}
