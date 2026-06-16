#include "net/peer_message.h"

#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace relaydesk::net {
namespace {

constexpr const char* kProtocol = "relaydesk.peer";
constexpr int kPeerMessageVersion = 1;

nlohmann::json makeEnvelope(std::string type, nlohmann::json message)
{
    return nlohmann::json{
        {"protocol", kProtocol},
        {"version", kPeerMessageVersion},
        {"type", std::move(type)},
        {"message", std::move(message)},
    };
}

nlohmann::json parseEnvelope(const std::string& payload)
{
    try {
        return nlohmann::json::parse(payload);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Peer message header is invalid JSON.");
    }
}

void requireEnvelope(const nlohmann::json& value, const char* expectedType)
{
    if (!value.is_object()
        || value.value("protocol", "") != kProtocol
        || value.value("version", 0) != kPeerMessageVersion
        || value.value("type", "") != expectedType
        || !value.contains("message")
        || !value["message"].is_object()) {
        throw std::runtime_error("Peer message header has unsupported schema.");
    }
}

std::string readRequiredString(const nlohmann::json& value, const char* fieldName)
{
    if (!value.contains(fieldName)
        || !value[fieldName].is_string()
        || value[fieldName].get<std::string>().empty()) {
        throw std::runtime_error("Peer message header is missing a required field.");
    }

    return value[fieldName].get<std::string>();
}

std::uintmax_t readRequiredFileSize(const nlohmann::json& value)
{
    if (!value.contains("file_size") || !value["file_size"].is_number_unsigned()) {
        throw std::runtime_error("Peer message header is missing file size.");
    }

    return value["file_size"].get<std::uintmax_t>();
}

std::uintmax_t readRequiredOffset(const nlohmann::json& value)
{
    if (!value.contains("offset") || !value["offset"].is_number_unsigned()) {
        throw std::runtime_error("Peer message header is missing chunk offset.");
    }

    return value["offset"].get<std::uintmax_t>();
}

void requireEmptyBody(const PeerFrame& frame, const char* frameName)
{
    if (!frame.GetBody().empty()) {
        throw std::runtime_error(std::string(frameName)
                                 + " frame must not contain a body.");
    }
}

nlohmann::json chatRecordToJson(
    const relaydesk::storage::ChatMessageRecord& record)
{
    try {
        return nlohmann::json::parse(
            relaydesk::storage::serializeChatMessageRecord(record));
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Chat message record serialization failed.");
    }
}

nlohmann::json transferOfferToJson(const TransferOfferMessage& message)
{
    if (message.GetMessageId().empty()
        || message.GetPartId().empty()
        || message.GetTransferId().empty()
        || message.GetSenderDeviceId().empty()
        || message.GetFileName().empty()) {
        throw std::runtime_error("Transfer offer header is missing required fields.");
    }

    nlohmann::json value{
        {"message_id", message.GetMessageId()},
        {"part_id", message.GetPartId()},
        {"transfer_id", message.GetTransferId()},
        {"sender_device_id", message.GetSenderDeviceId()},
        {"file_name", message.GetFileName()},
        {"file_size", message.GetFileSize()},
        {"image_transfer", message.GetImageTransfer()},
    };
    if (message.GetSha256().has_value()) {
        value["sha256"] = message.GetSha256().value();
    }
    return value;
}

TransferOfferMessage transferOfferFromJson(const nlohmann::json& value)
{
    TransferOfferMessage message;
    message.SetMessageId(readRequiredString(value, "message_id"));
    message.SetPartId(readRequiredString(value, "part_id"));
    message.SetTransferId(readRequiredString(value, "transfer_id"));
    message.SetSenderDeviceId(readRequiredString(value, "sender_device_id"));
    message.SetFileName(readRequiredString(value, "file_name"));
    message.SetFileSize(readRequiredFileSize(value));
    message.SetImageTransfer(value.value("image_transfer", false));
    if (value.contains("sha256") && value["sha256"].is_string()) {
        message.SetSha256(value["sha256"].get<std::string>());
    }
    return message;
}

nlohmann::json transferChunkToJson(const TransferChunkMessage& message)
{
    if (message.GetMessageId().empty()
        || message.GetPartId().empty()
        || message.GetTransferId().empty()) {
        throw std::runtime_error("Transfer chunk header is missing required fields.");
    }

    return nlohmann::json{
        {"message_id", message.GetMessageId()},
        {"part_id", message.GetPartId()},
        {"transfer_id", message.GetTransferId()},
        {"offset", message.GetOffset()},
    };
}

TransferChunkMessage transferChunkFromJson(const nlohmann::json& value)
{
    TransferChunkMessage message;
    message.SetMessageId(readRequiredString(value, "message_id"));
    message.SetPartId(readRequiredString(value, "part_id"));
    message.SetTransferId(readRequiredString(value, "transfer_id"));
    message.SetOffset(readRequiredOffset(value));
    return message;
}

nlohmann::json transferCompleteToJson(const TransferCompleteMessage& message)
{
    if (message.GetMessageId().empty()
        || message.GetPartId().empty()
        || message.GetTransferId().empty()) {
        throw std::runtime_error("Transfer complete header is missing required fields.");
    }

    nlohmann::json value{
        {"message_id", message.GetMessageId()},
        {"part_id", message.GetPartId()},
        {"transfer_id", message.GetTransferId()},
        {"file_size", message.GetFileSize()},
    };
    if (message.GetSha256().has_value()) {
        value["sha256"] = message.GetSha256().value();
    }
    return value;
}

TransferCompleteMessage transferCompleteFromJson(const nlohmann::json& value)
{
    TransferCompleteMessage message;
    message.SetMessageId(readRequiredString(value, "message_id"));
    message.SetPartId(readRequiredString(value, "part_id"));
    message.SetTransferId(readRequiredString(value, "transfer_id"));
    message.SetFileSize(readRequiredFileSize(value));
    if (value.contains("sha256") && value["sha256"].is_string()) {
        message.SetSha256(value["sha256"].get<std::string>());
    }
    return message;
}

} // namespace

std::string serializePeerChatMessageHeader(
    const relaydesk::storage::ChatMessageRecord& record)
{
    return makeEnvelope(kPeerMessageTypeChatMessage, chatRecordToJson(record)).dump();
}

relaydesk::storage::ChatMessageRecord parsePeerChatMessageHeader(
    const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeChatMessage);
    return relaydesk::storage::parseChatMessageRecord(value["message"].dump());
}

PeerFrame makeChatMessageFrame(
    const relaydesk::storage::ChatMessageRecord& record)
{
    return PeerFrame(PeerFrameType::ChatMessage,
                     serializePeerChatMessageHeader(record));
}

relaydesk::storage::ChatMessageRecord parseChatMessageFrame(
    const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::ChatMessage) {
        throw std::runtime_error("Peer frame is not a chat_message frame.");
    }
    requireEmptyBody(frame, "Chat message");

    return parsePeerChatMessageHeader(frame.GetHeader());
}

std::string serializeTransferOfferHeader(const TransferOfferMessage& message)
{
    return makeEnvelope(
        kPeerMessageTypeTransferOffer,
        transferOfferToJson(message)).dump();
}

TransferOfferMessage parseTransferOfferHeader(const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeTransferOffer);
    return transferOfferFromJson(value["message"]);
}

PeerFrame makeTransferOfferFrame(const TransferOfferMessage& message)
{
    return PeerFrame(PeerFrameType::TransferOffer,
                     serializeTransferOfferHeader(message));
}

TransferOfferMessage parseTransferOfferFrame(const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::TransferOffer) {
        throw std::runtime_error("Peer frame is not a transfer_offer frame.");
    }
    requireEmptyBody(frame, "Transfer offer");
    return parseTransferOfferHeader(frame.GetHeader());
}

std::string serializeTransferChunkHeader(const TransferChunkMessage& message)
{
    return makeEnvelope(
        kPeerMessageTypeTransferChunk,
        transferChunkToJson(message)).dump();
}

TransferChunkMessage parseTransferChunkHeader(const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeTransferChunk);
    return transferChunkFromJson(value["message"]);
}

PeerFrame makeTransferChunkFrame(TransferChunkMessage message,
                                 std::vector<std::uint8_t> body)
{
    return PeerFrame(PeerFrameType::TransferChunk,
                     serializeTransferChunkHeader(message),
                     std::move(body));
}

TransferChunkMessage parseTransferChunkFrame(const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::TransferChunk) {
        throw std::runtime_error("Peer frame is not a transfer_chunk frame.");
    }
    return parseTransferChunkHeader(frame.GetHeader());
}

std::string serializeTransferCompleteHeader(const TransferCompleteMessage& message)
{
    return makeEnvelope(
        kPeerMessageTypeTransferComplete,
        transferCompleteToJson(message)).dump();
}

TransferCompleteMessage parseTransferCompleteHeader(const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeTransferComplete);
    return transferCompleteFromJson(value["message"]);
}

PeerFrame makeTransferCompleteFrame(const TransferCompleteMessage& message)
{
    return PeerFrame(PeerFrameType::TransferComplete,
                     serializeTransferCompleteHeader(message));
}

TransferCompleteMessage parseTransferCompleteFrame(const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::TransferComplete) {
        throw std::runtime_error("Peer frame is not a transfer_complete frame.");
    }
    requireEmptyBody(frame, "Transfer complete");
    return parseTransferCompleteHeader(frame.GetHeader());
}

}
