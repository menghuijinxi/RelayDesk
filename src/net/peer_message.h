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
constexpr const char* kPeerMessageTypeTransferChunk = "transfer_chunk";
constexpr const char* kPeerMessageTypeTransferComplete = "transfer_complete";

class TransferOfferMessage {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    const std::string& GetSenderDeviceId() const { return senderDeviceId_; }
    const std::string& GetFileName() const { return fileName_; }
    std::uintmax_t GetFileSize() const { return fileSize_; }
    const std::optional<std::string>& GetSha256() const { return sha256_; }

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

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string senderDeviceId_;
    std::string fileName_;
    std::uintmax_t fileSize_ = 0;
    std::optional<std::string> sha256_;
};

class TransferChunkMessage {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    std::uintmax_t GetOffset() const { return offset_; }

    void SetMessageId(std::string messageId) { messageId_ = std::move(messageId); }
    void SetPartId(std::string partId) { partId_ = std::move(partId); }
    void SetTransferId(std::string transferId) { transferId_ = std::move(transferId); }
    void SetOffset(std::uintmax_t offset) { offset_ = offset; }

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::uintmax_t offset_ = 0;
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

std::string serializePeerChatMessageHeader(
    const relaydesk::storage::ChatMessageRecord& record);
relaydesk::storage::ChatMessageRecord parsePeerChatMessageHeader(
    const std::string& payload);
PeerFrame makeChatMessageFrame(
    const relaydesk::storage::ChatMessageRecord& record);
relaydesk::storage::ChatMessageRecord parseChatMessageFrame(
    const PeerFrame& frame);
std::string serializeTransferOfferHeader(const TransferOfferMessage& message);
TransferOfferMessage parseTransferOfferHeader(const std::string& payload);
PeerFrame makeTransferOfferFrame(const TransferOfferMessage& message);
TransferOfferMessage parseTransferOfferFrame(const PeerFrame& frame);
std::string serializeTransferChunkHeader(const TransferChunkMessage& message);
TransferChunkMessage parseTransferChunkHeader(const std::string& payload);
PeerFrame makeTransferChunkFrame(TransferChunkMessage message,
                                 std::vector<std::uint8_t> body);
TransferChunkMessage parseTransferChunkFrame(const PeerFrame& frame);
std::string serializeTransferCompleteHeader(const TransferCompleteMessage& message);
TransferCompleteMessage parseTransferCompleteHeader(const std::string& payload);
PeerFrame makeTransferCompleteFrame(const TransferCompleteMessage& message);
TransferCompleteMessage parseTransferCompleteFrame(const PeerFrame& frame);

}
