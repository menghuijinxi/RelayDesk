#pragma once

#include "net/peer_frame.h"
#include "storage/history_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace relaydesk::net {

constexpr const char* kPeerMessageTypeChatMessage = "chat_message";
constexpr const char* kPeerMessageTypeTransferOffer = "transfer_offer";
constexpr const char* kPeerMessageTypeTransferAccept = "transfer_accept";
constexpr const char* kPeerMessageTypeTransferReject = "transfer_reject";
constexpr const char* kPeerMessageTypeTransferCancel = "transfer_cancel";
constexpr const char* kPeerMessageTypeTransferChunk = "transfer_chunk";
constexpr const char* kPeerMessageTypeTransferComplete = "transfer_complete";
constexpr const char* kPeerMessageTypeAppUpdateRequest = "app_update_request";
constexpr const char* kPeerMessageTypeAppUpdateChunk = "app_update_chunk";
constexpr const char* kPeerMessageTypeAppUpdateComplete = "app_update_complete";
constexpr const char* kPeerMessageTypeAvatarRequest = "avatar_request";
constexpr const char* kPeerMessageTypeAvatar = "avatar";
constexpr const char* kScreenShakeEventMarker =
    "relaydesk-event:screen-shake:v1";
constexpr const char* kScreenShakeEventMessageIdPrefix =
    "relaydesk-event:screen-shake:v1:";

enum class TransferSaveStrategy {
    Unique,
    Overwrite,
};

class TransferOfferMessage {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    const std::string& GetSenderDeviceId() const { return senderDeviceId_; }
    const std::string& GetFileName() const { return fileName_; }
    std::uintmax_t GetFileSize() const { return fileSize_; }
    const std::optional<std::string>& GetSha256() const { return sha256_; }
    bool GetImageTransfer() const { return imageTransfer_; }
    bool GetFolderTransfer() const { return folderTransfer_; }
    bool GetResumeRequest() const { return resumeRequest_; }

    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId) { transferId_ = std::move(transferId); }
    void SetSenderDeviceId(std::string senderDeviceId)
    {
        senderDeviceId_ = std::move(senderDeviceId);
    }
    void SetFileName(std::string fileName) { fileName_ = std::move(fileName); }
    void SetFileSize(std::uintmax_t fileSize) { fileSize_ = fileSize; }
    void SetSha256(std::string sha256) { sha256_ = std::move(sha256); }
    void SetImageTransfer(bool imageTransfer) { imageTransfer_ = imageTransfer; }
    void SetFolderTransfer(bool folderTransfer) { folderTransfer_ = folderTransfer; }
    void SetResumeRequest(bool resumeRequest) { resumeRequest_ = resumeRequest; }

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string senderDeviceId_;
    std::string fileName_;
    std::uintmax_t fileSize_ = 0;
    std::optional<std::string> sha256_;
    bool imageTransfer_ = false;
    bool folderTransfer_ = false;
    bool resumeRequest_ = false;
};

class TransferAcceptMessage {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    const std::string& GetReceiverDeviceId() const { return receiverDeviceId_; }
    TransferSaveStrategy GetSaveStrategy() const { return saveStrategy_; }
    std::uintmax_t GetResumeOffset() const { return resumeOffset_; }

    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId) { transferId_ = std::move(transferId); }
    void SetReceiverDeviceId(std::string receiverDeviceId)
    {
        receiverDeviceId_ = std::move(receiverDeviceId);
    }
    void SetSaveStrategy(TransferSaveStrategy saveStrategy)
    {
        saveStrategy_ = saveStrategy;
    }
    void SetResumeOffset(std::uintmax_t resumeOffset)
    {
        resumeOffset_ = resumeOffset;
    }

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string receiverDeviceId_;
    TransferSaveStrategy saveStrategy_ = TransferSaveStrategy::Unique;
    std::uintmax_t resumeOffset_ = 0;
};

class TransferRejectMessage {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    const std::string& GetReceiverDeviceId() const { return receiverDeviceId_; }
    const std::string& GetReason() const { return reason_; }

    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId) { transferId_ = std::move(transferId); }
    void SetReceiverDeviceId(std::string receiverDeviceId)
    {
        receiverDeviceId_ = std::move(receiverDeviceId);
    }
    void SetReason(std::string reason) { reason_ = std::move(reason); }

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string receiverDeviceId_;
    std::string reason_ = "user_rejected";
};

class TransferCancelMessage {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    const std::string& GetCancellerDeviceId() const { return cancellerDeviceId_; }
    const std::string& GetReason() const { return reason_; }

    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId) { transferId_ = std::move(transferId); }
    void SetCancellerDeviceId(std::string cancellerDeviceId)
    {
        cancellerDeviceId_ = std::move(cancellerDeviceId);
    }
    void SetReason(std::string reason) { reason_ = std::move(reason); }

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string cancellerDeviceId_;
    std::string reason_ = "user_cancelled";
};

class TransferChunkMessage {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    std::uintmax_t GetOffset() const { return offset_; }
    const std::optional<std::string>& GetFolderRelativePath() const
    {
        return folderRelativePath_;
    }
    std::uintmax_t GetFolderFileOffset() const { return folderFileOffset_; }
    bool GetFolderDirectory() const { return folderDirectory_; }

    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId) { transferId_ = std::move(transferId); }
    void SetOffset(std::uintmax_t offset) { offset_ = offset; }
    void SetFolderRelativePath(std::string folderRelativePath)
    {
        folderRelativePath_ = std::move(folderRelativePath);
    }
    void SetFolderFileOffset(std::uintmax_t folderFileOffset)
    {
        folderFileOffset_ = folderFileOffset;
    }
    void SetFolderDirectory(bool folderDirectory)
    {
        folderDirectory_ = folderDirectory;
    }

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::uintmax_t offset_ = 0;
    std::optional<std::string> folderRelativePath_;
    std::uintmax_t folderFileOffset_ = 0;
    bool folderDirectory_ = false;
};

class TransferCompleteMessage {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    std::uintmax_t GetFileSize() const { return fileSize_; }
    const std::optional<std::string>& GetSha256() const { return sha256_; }

    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId) { transferId_ = std::move(transferId); }
    void SetFileSize(std::uintmax_t fileSize) { fileSize_ = fileSize; }
    void SetSha256(std::string sha256) { sha256_ = std::move(sha256); }

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::uintmax_t fileSize_ = 0;
    std::optional<std::string> sha256_;
};

class AppUpdateRequestMessage {
public:
    const std::string& GetRequestId() const { return requestId_; }
    const std::string& GetRequesterDeviceId() const { return requesterDeviceId_; }
    int GetCurrentAppVersion() const { return currentAppVersion_; }
    int GetRequestedAppVersion() const { return requestedAppVersion_; }

    void SetRequestId(std::string requestId) { requestId_ = std::move(requestId); }
    void SetRequesterDeviceId(std::string requesterDeviceId)
    {
        requesterDeviceId_ = std::move(requesterDeviceId);
    }
    void SetCurrentAppVersion(int currentAppVersion)
    {
        currentAppVersion_ = currentAppVersion;
    }
    void SetRequestedAppVersion(int requestedAppVersion)
    {
        requestedAppVersion_ = requestedAppVersion;
    }

protected:
    std::string requestId_;
    std::string requesterDeviceId_;
    int currentAppVersion_ = 0;
    int requestedAppVersion_ = 0;
};

class AppUpdateChunkMessage {
public:
    const std::string& GetRequestId() const { return requestId_; }
    std::uintmax_t GetOffset() const { return offset_; }
    std::uintmax_t GetFileSize() const { return fileSize_; }

    void SetRequestId(std::string requestId) { requestId_ = std::move(requestId); }
    void SetOffset(std::uintmax_t offset) { offset_ = offset; }
    void SetFileSize(std::uintmax_t fileSize) { fileSize_ = fileSize; }

protected:
    std::string requestId_;
    std::uintmax_t offset_ = 0;
    std::uintmax_t fileSize_ = 0;
};

class AppUpdateCompleteMessage {
public:
    const std::string& GetRequestId() const { return requestId_; }
    int GetAppVersion() const { return appVersion_; }
    const std::string& GetFileName() const { return fileName_; }
    std::uintmax_t GetFileSize() const { return fileSize_; }

    void SetRequestId(std::string requestId) { requestId_ = std::move(requestId); }
    void SetAppVersion(int appVersion) { appVersion_ = appVersion; }
    void SetFileName(std::string fileName) { fileName_ = std::move(fileName); }
    void SetFileSize(std::uintmax_t fileSize) { fileSize_ = fileSize; }

protected:
    std::string requestId_;
    int appVersion_ = 0;
    std::string fileName_;
    std::uintmax_t fileSize_ = 0;
};

std::string serializePeerChatMessageHeader(
    const relaydesk::storage::ChatMessageRecord& record);
relaydesk::storage::ChatMessageRecord parsePeerChatMessageHeader(
    const std::string& payload);
PeerFrame makeChatMessageFrame(
    const relaydesk::storage::ChatMessageRecord& record);
relaydesk::storage::ChatMessageRecord parseChatMessageFrame(
    const PeerFrame& frame);
bool isScreenShakeChatMessage(
    const relaydesk::storage::ChatMessageRecord& record);
std::string serializeTransferOfferHeader(const TransferOfferMessage& message);
TransferOfferMessage parseTransferOfferHeader(const std::string& payload);
PeerFrame makeTransferOfferFrame(const TransferOfferMessage& message);
TransferOfferMessage parseTransferOfferFrame(const PeerFrame& frame);
std::string serializeTransferAcceptHeader(const TransferAcceptMessage& message);
TransferAcceptMessage parseTransferAcceptHeader(const std::string& payload);
PeerFrame makeTransferAcceptFrame(const TransferAcceptMessage& message);
TransferAcceptMessage parseTransferAcceptFrame(const PeerFrame& frame);
std::string serializeTransferRejectHeader(const TransferRejectMessage& message);
TransferRejectMessage parseTransferRejectHeader(const std::string& payload);
PeerFrame makeTransferRejectFrame(const TransferRejectMessage& message);
TransferRejectMessage parseTransferRejectFrame(const PeerFrame& frame);
std::string serializeTransferCancelHeader(const TransferCancelMessage& message);
TransferCancelMessage parseTransferCancelHeader(const std::string& payload);
PeerFrame makeTransferCancelFrame(const TransferCancelMessage& message);
TransferCancelMessage parseTransferCancelFrame(const PeerFrame& frame);
std::string serializeTransferChunkHeader(const TransferChunkMessage& message);
TransferChunkMessage parseTransferChunkHeader(const std::string& payload);
PeerFrame makeTransferChunkFrame(TransferChunkMessage message,
                                 std::vector<std::uint8_t> body);
TransferChunkMessage parseTransferChunkFrame(const PeerFrame& frame);
std::string serializeTransferCompleteHeader(const TransferCompleteMessage& message);
TransferCompleteMessage parseTransferCompleteHeader(const std::string& payload);
PeerFrame makeTransferCompleteFrame(const TransferCompleteMessage& message);
TransferCompleteMessage parseTransferCompleteFrame(const PeerFrame& frame);
std::string serializeAppUpdateRequestHeader(const AppUpdateRequestMessage& message);
AppUpdateRequestMessage parseAppUpdateRequestHeader(const std::string& payload);
PeerFrame makeAppUpdateRequestFrame(const AppUpdateRequestMessage& message);
AppUpdateRequestMessage parseAppUpdateRequestFrame(const PeerFrame& frame);
std::string serializeAppUpdateChunkHeader(const AppUpdateChunkMessage& message);
AppUpdateChunkMessage parseAppUpdateChunkHeader(const std::string& payload);
PeerFrame makeAppUpdateChunkFrame(AppUpdateChunkMessage message,
                                  std::vector<std::uint8_t> body);
AppUpdateChunkMessage parseAppUpdateChunkFrame(const PeerFrame& frame);
class AvatarRequestMessage {
public:
    const std::string& GetRequesterDeviceId() const { return requesterDeviceId_; }
    const std::string& GetDeviceId() const { return deviceId_; }
    const std::string& GetAvatarSha256() const { return avatarSha256_; }

    void SetRequesterDeviceId(std::string requesterDeviceId)
    {
        requesterDeviceId_ = std::move(requesterDeviceId);
    }
    void SetDeviceId(std::string deviceId) { deviceId_ = std::move(deviceId); }
    void SetAvatarSha256(std::string avatarSha256)
    {
        avatarSha256_ = std::move(avatarSha256);
    }

protected:
    std::string requesterDeviceId_;
    std::string deviceId_;
    std::string avatarSha256_;
};

class AvatarMessage {
public:
    const std::string& GetDeviceId() const { return deviceId_; }
    const std::string& GetAvatarSha256() const { return avatarSha256_; }

    void SetDeviceId(std::string deviceId) { deviceId_ = std::move(deviceId); }
    void SetAvatarSha256(std::string avatarSha256)
    {
        avatarSha256_ = std::move(avatarSha256);
    }

protected:
    std::string deviceId_;
    std::string avatarSha256_;
};

std::string serializeAppUpdateCompleteHeader(const AppUpdateCompleteMessage& message);
AppUpdateCompleteMessage parseAppUpdateCompleteHeader(const std::string& payload);
PeerFrame makeAppUpdateCompleteFrame(const AppUpdateCompleteMessage& message);
AppUpdateCompleteMessage parseAppUpdateCompleteFrame(const PeerFrame& frame);
std::string serializeAvatarRequestHeader(const AvatarRequestMessage& message);
AvatarRequestMessage parseAvatarRequestHeader(const std::string& payload);
PeerFrame makeAvatarRequestFrame(const AvatarRequestMessage& message);
AvatarRequestMessage parseAvatarRequestFrame(const PeerFrame& frame);
std::string serializeAvatarHeader(const AvatarMessage& message);
AvatarMessage parseAvatarHeader(const std::string& payload);
PeerFrame makeAvatarFrame(AvatarMessage message, std::vector<std::uint8_t> body);
AvatarMessage parseAvatarFrame(const PeerFrame& frame);

}
