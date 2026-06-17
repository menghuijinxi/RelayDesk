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

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string senderDeviceId_;
    std::string fileName_;
    std::uintmax_t fileSize_ = 0;
    std::optional<std::string> sha256_;
    bool imageTransfer_ = false;
};

class TransferAcceptMessage {
public:
    const std::string& GetMessageId() const { return messageId_; }
    const std::string& GetPartId() const { return partId_; }
    const std::string& GetTransferId() const { return transferId_; }
    const std::string& GetReceiverDeviceId() const { return receiverDeviceId_; }
    TransferSaveStrategy GetSaveStrategy() const { return saveStrategy_; }

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

protected:
    std::string messageId_;
    std::string partId_;
    std::string transferId_;
    std::string receiverDeviceId_;
    TransferSaveStrategy saveStrategy_ = TransferSaveStrategy::Unique;
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

}
