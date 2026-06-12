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

enum class MessageContentType {
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
    MessageContentType GetContentType() const { return contentType_; }
    DeliveryState GetDeliveryState() const { return deliveryState_; }
    const std::optional<std::string>& GetText() const { return text_; }
    const std::optional<std::string>& GetEmoji() const { return emoji_; }
    const std::optional<std::string>& GetTransferId() const { return transferId_; }
    const std::optional<std::string>& GetFileName() const { return fileName_; }
    const std::optional<std::uintmax_t>& GetFileSize() const { return fileSize_; }
    const std::optional<std::string>& GetSha256() const { return sha256_; }
    const std::optional<std::string>& GetLocalPath() const { return localPath_; }
    const std::optional<std::string>& GetManifestPath() const { return manifestPath_; }

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
    void SetContentType(MessageContentType contentType) { contentType_ = contentType; }
    void SetDeliveryState(DeliveryState deliveryState) { deliveryState_ = deliveryState; }
    void SetText(std::string text) { text_ = std::move(text); }
    void SetEmoji(std::string emoji) { emoji_ = std::move(emoji); }
    void SetTransferId(std::string transferId) { transferId_ = std::move(transferId); }
    void SetFileName(std::string fileName) { fileName_ = std::move(fileName); }
    void SetFileSize(std::uintmax_t fileSize) { fileSize_ = fileSize; }
    void SetSha256(std::string sha256) { sha256_ = std::move(sha256); }
    void SetLocalPath(std::string localPath) { localPath_ = std::move(localPath); }
    void SetManifestPath(std::string manifestPath)
    {
        manifestPath_ = std::move(manifestPath);
    }

protected:
    int schemaVersion_ = 1;
    std::string messageId_;
    std::string conversationId_;
    MessageDirection direction_ = MessageDirection::Outgoing;
    std::string senderDeviceId_;
    std::string receiverDeviceId_;
    std::string senderDisplayNameSnapshot_;
    std::string receiverDisplayNameSnapshot_;
    std::string createdAt_;
    MessageContentType contentType_ = MessageContentType::Text;
    DeliveryState deliveryState_ = DeliveryState::Pending;
    std::optional<std::string> text_;
    std::optional<std::string> emoji_;
    std::optional<std::string> transferId_;
    std::optional<std::string> fileName_;
    std::optional<std::uintmax_t> fileSize_;
    std::optional<std::string> sha256_;
    std::optional<std::string> localPath_;
    std::optional<std::string> manifestPath_;
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
void appendChatMessage(const AppPaths& appPaths,
                       const std::string& peerDeviceId,
                       const ChatMessageRecord& record);
ChatHistoryLoadResult loadChatHistory(const AppPaths& appPaths,
                                      const std::string& peerDeviceId);

}
