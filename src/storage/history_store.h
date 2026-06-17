#pragma once

#include "storage/app_paths.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace relaydesk::storage {

enum class MessageDirection {
    Incoming,
    Outgoing,
};

enum class MessagePartType {
    Text,
    Emoji,
    Image,
    File,
    Folder,
};

enum class DeliveryState {
    Pending,
    Sent,
    Delivered,
    Received,
    Completed,
    Failed,
    Cancelled,
};

enum class TransferState {
    Pending,
    Offered,
    Transferring,
    Completed,
    Failed,
    Cancelled,
    Rejected,
};

class ChatMessagePart {
public:
    const std::string& GetPartId() const { return partId_; }
    MessagePartType GetType() const { return type_; }
    const std::optional<std::string>& GetText() const { return text_; }
    const std::optional<std::string>& GetEmoji() const { return emoji_; }
    const std::optional<std::string>& GetTransferId() const { return transferId_; }
    const std::optional<TransferState>& GetTransferState() const
    {
        return transferState_;
    }
    const std::optional<std::string>& GetFileName() const { return fileName_; }
    const std::optional<std::uintmax_t>& GetFileSize() const { return fileSize_; }
    const std::optional<std::uintmax_t>& GetTransferredSize() const
    {
        return transferredSize_;
    }
    const std::optional<std::string>& GetSha256() const { return sha256_; }
    const std::optional<std::string>& GetLocalPath() const { return localPath_; }
    const std::optional<std::string>& GetManifestPath() const { return manifestPath_; }

    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetType(MessagePartType type) { type_ = type; }
    void SetText(std::string text) { text_ = std::move(text); }
    void SetEmoji(std::string emoji) { emoji_ = std::move(emoji); }
    void SetTransferId(std::string transferId) { transferId_ = std::move(transferId); }
    void SetTransferState(TransferState transferState)
    {
        transferState_ = transferState;
    }
    void SetFileName(std::string fileName) { fileName_ = std::move(fileName); }
    void SetFileSize(std::uintmax_t fileSize) { fileSize_ = fileSize; }
    void SetTransferredSize(std::uintmax_t transferredSize)
    {
        transferredSize_ = transferredSize;
    }
    void SetSha256(std::string sha256) { sha256_ = std::move(sha256); }
    void SetLocalPath(std::string localPath) { localPath_ = std::move(localPath); }
    void SetManifestPath(std::string manifestPath)
    {
        manifestPath_ = std::move(manifestPath);
    }

protected:
    std::string partId_;
    MessagePartType type_ = MessagePartType::Text;
    std::optional<std::string> text_;
    std::optional<std::string> emoji_;
    std::optional<std::string> transferId_;
    std::optional<TransferState> transferState_;
    std::optional<std::string> fileName_;
    std::optional<std::uintmax_t> fileSize_;
    std::optional<std::uintmax_t> transferredSize_;
    std::optional<std::string> sha256_;
    std::optional<std::string> localPath_;
    std::optional<std::string> manifestPath_;
};

class ChatMessageRecord {
public:
    int GetSchemaVersion() const { return schemaVersion_; }
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetConversationId() const { return conversationId_; }
    MessageDirection GetDirection() const { return direction_; }
    const std::string& GetSenderDeviceId() const { return senderDeviceId_; }
    const std::string& GetReceiverDeviceId() const { return receiverDeviceId_; }
    const std::string& GetSenderDisplayNameSnapshot() const
    {
        return senderDisplayNameSnapshot_;
    }
    const std::string& GetReceiverDisplayNameSnapshot() const
    {
        return receiverDisplayNameSnapshot_;
    }
    const std::string& GetCreatedAt() const { return createdAt_; }
    DeliveryState GetDeliveryState() const { return deliveryState_; }
    const std::vector<ChatMessagePart>& GetParts() const { return parts_; }

    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetConversationId(std::string conversationId)
    {
        conversationId_ = std::move(conversationId);
    }
    void SetDirection(MessageDirection direction) { direction_ = direction; }
    void SetSenderDeviceId(std::string senderDeviceId)
    {
        senderDeviceId_ = std::move(senderDeviceId);
    }
    void SetReceiverDeviceId(std::string receiverDeviceId)
    {
        receiverDeviceId_ = std::move(receiverDeviceId);
    }
    void SetSenderDisplayNameSnapshot(std::string displayName)
    {
        senderDisplayNameSnapshot_ = std::move(displayName);
    }
    void SetReceiverDisplayNameSnapshot(std::string displayName)
    {
        receiverDisplayNameSnapshot_ = std::move(displayName);
    }
    void SetCreatedAt(std::string createdAt) { createdAt_ = std::move(createdAt); }
    void SetDeliveryState(DeliveryState deliveryState) { deliveryState_ = deliveryState; }
    void AddPart(ChatMessagePart part) { parts_.push_back(std::move(part)); }
    void SetParts(std::vector<ChatMessagePart> parts)
    {
        parts_ = std::move(parts);
    }

protected:
    int schemaVersion_ = 2;
    std::string messageId_;
    std::string conversationId_;
    MessageDirection direction_ = MessageDirection::Outgoing;
    std::string senderDeviceId_;
    std::string receiverDeviceId_;
    std::string senderDisplayNameSnapshot_;
    std::string receiverDisplayNameSnapshot_;
    std::string createdAt_;
    DeliveryState deliveryState_ = DeliveryState::Pending;
    std::vector<ChatMessagePart> parts_;
};

class ChatHistoryLoadResult {
public:
    explicit ChatHistoryLoadResult(std::vector<ChatMessageRecord> records,
                                   int skippedLineCount);

    const std::vector<ChatMessageRecord>& GetRecords() const { return records_; }
    int GetSkippedLineCount() const { return skippedLineCount_; }

protected:
    std::vector<ChatMessageRecord> records_;
    int skippedLineCount_ = 0;
};

std::string makeDirectConversationId(const std::string& localDeviceId,
                                     const std::string& peerDeviceId);
std::filesystem::path getPeerMessagesFilePath(const AppPaths& appPaths,
                                              const std::string& peerDeviceId);
std::string serializeChatMessageRecord(const ChatMessageRecord& record);
ChatMessageRecord parseChatMessageRecord(const std::string& payload);
void appendChatMessage(const AppPaths& appPaths,
                       const std::string& peerDeviceId,
                       const ChatMessageRecord& record);
bool replaceChatMessage(const AppPaths& appPaths,
                        const std::string& peerDeviceId,
                        const ChatMessageRecord& record);
ChatHistoryLoadResult loadChatHistory(const AppPaths& appPaths,
                                      const std::string& peerDeviceId);

}
