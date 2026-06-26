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

int readRequiredAppVersion(const nlohmann::json& value, const char* fieldName)
{
    if (!value.contains(fieldName) || !value[fieldName].is_number_integer()) {
        throw std::runtime_error("Peer message header is missing app version.");
    }

    const int appVersion = value[fieldName].get<int>();
    if (appVersion < 0) {
        throw std::runtime_error("Peer message app version is invalid.");
    }
    return appVersion;
}

std::string toJsonValue(TransferSaveStrategy saveStrategy)
{
    switch (saveStrategy) {
    case TransferSaveStrategy::Unique:
        return "unique";
    case TransferSaveStrategy::Overwrite:
        return "overwrite";
    }

    throw std::runtime_error("Unsupported transfer save strategy.");
}

TransferSaveStrategy transferSaveStrategyFromJsonValue(const std::string& value)
{
    if (value == "unique") {
        return TransferSaveStrategy::Unique;
    }
    if (value == "overwrite") {
        return TransferSaveStrategy::Overwrite;
    }

    throw std::runtime_error("Unsupported transfer save strategy value.");
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
        {"folder_transfer", message.GetFolderTransfer()},
        {"resume_request", message.GetResumeRequest()},
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
    message.SetFolderTransfer(value.value("folder_transfer", false));
    message.SetResumeRequest(value.value("resume_request", false));
    if (value.contains("sha256") && value["sha256"].is_string()) {
        message.SetSha256(value["sha256"].get<std::string>());
    }
    return message;
}

nlohmann::json transferAcceptToJson(const TransferAcceptMessage& message)
{
    if (message.GetMessageId().empty()
        || message.GetPartId().empty()
        || message.GetTransferId().empty()
        || message.GetReceiverDeviceId().empty()) {
        throw std::runtime_error("Transfer accept header is missing required fields.");
    }

    return nlohmann::json{
        {"message_id", message.GetMessageId()},
        {"part_id", message.GetPartId()},
        {"transfer_id", message.GetTransferId()},
        {"receiver_device_id", message.GetReceiverDeviceId()},
        {"save_strategy", toJsonValue(message.GetSaveStrategy())},
        {"resume_offset", message.GetResumeOffset()},
    };
}

TransferAcceptMessage transferAcceptFromJson(const nlohmann::json& value)
{
    TransferAcceptMessage message;
    message.SetMessageId(readRequiredString(value, "message_id"));
    message.SetPartId(readRequiredString(value, "part_id"));
    message.SetTransferId(readRequiredString(value, "transfer_id"));
    message.SetReceiverDeviceId(readRequiredString(value, "receiver_device_id"));
    if (value.contains("save_strategy")) {
        message.SetSaveStrategy(
            transferSaveStrategyFromJsonValue(
                readRequiredString(value, "save_strategy")));
    }
    if (value.contains("resume_offset")) {
        if (!value["resume_offset"].is_number_unsigned()) {
            throw std::runtime_error("Transfer accept resume offset is invalid.");
        }
        message.SetResumeOffset(value["resume_offset"].get<std::uintmax_t>());
    }
    return message;
}

nlohmann::json transferRejectToJson(const TransferRejectMessage& message)
{
    if (message.GetMessageId().empty()
        || message.GetPartId().empty()
        || message.GetTransferId().empty()
        || message.GetReceiverDeviceId().empty()
        || message.GetReason().empty()) {
        throw std::runtime_error("Transfer reject header is missing required fields.");
    }

    return nlohmann::json{
        {"message_id", message.GetMessageId()},
        {"part_id", message.GetPartId()},
        {"transfer_id", message.GetTransferId()},
        {"receiver_device_id", message.GetReceiverDeviceId()},
        {"reason", message.GetReason()},
    };
}

TransferRejectMessage transferRejectFromJson(const nlohmann::json& value)
{
    TransferRejectMessage message;
    message.SetMessageId(readRequiredString(value, "message_id"));
    message.SetPartId(readRequiredString(value, "part_id"));
    message.SetTransferId(readRequiredString(value, "transfer_id"));
    message.SetReceiverDeviceId(readRequiredString(value, "receiver_device_id"));
    message.SetReason(readRequiredString(value, "reason"));
    return message;
}

nlohmann::json transferCancelToJson(const TransferCancelMessage& message)
{
    if (message.GetMessageId().empty()
        || message.GetPartId().empty()
        || message.GetTransferId().empty()
        || message.GetCancellerDeviceId().empty()
        || message.GetReason().empty()) {
        throw std::runtime_error("Transfer cancel header is missing required fields.");
    }

    return nlohmann::json{
        {"message_id", message.GetMessageId()},
        {"part_id", message.GetPartId()},
        {"transfer_id", message.GetTransferId()},
        {"canceller_device_id", message.GetCancellerDeviceId()},
        {"reason", message.GetReason()},
    };
}

TransferCancelMessage transferCancelFromJson(const nlohmann::json& value)
{
    TransferCancelMessage message;
    message.SetMessageId(readRequiredString(value, "message_id"));
    message.SetPartId(readRequiredString(value, "part_id"));
    message.SetTransferId(readRequiredString(value, "transfer_id"));
    message.SetCancellerDeviceId(readRequiredString(value, "canceller_device_id"));
    message.SetReason(readRequiredString(value, "reason"));
    return message;
}

nlohmann::json transferChunkToJson(const TransferChunkMessage& message)
{
    if (message.GetMessageId().empty()
        || message.GetPartId().empty()
        || message.GetTransferId().empty()) {
        throw std::runtime_error("Transfer chunk header is missing required fields.");
    }

    nlohmann::json value{
        {"message_id", message.GetMessageId()},
        {"part_id", message.GetPartId()},
        {"transfer_id", message.GetTransferId()},
        {"offset", message.GetOffset()},
    };
    if (message.GetFolderRelativePath().has_value()) {
        value["folder_relative_path"] = message.GetFolderRelativePath().value();
        value["folder_file_offset"] = message.GetFolderFileOffset();
        value["folder_directory"] = message.GetFolderDirectory();
    }
    return value;
}

TransferChunkMessage transferChunkFromJson(const nlohmann::json& value)
{
    TransferChunkMessage message;
    message.SetMessageId(readRequiredString(value, "message_id"));
    message.SetPartId(readRequiredString(value, "part_id"));
    message.SetTransferId(readRequiredString(value, "transfer_id"));
    message.SetOffset(readRequiredOffset(value));
    if (value.contains("folder_relative_path")) {
        if (!value["folder_relative_path"].is_string()
            || value["folder_relative_path"].get<std::string>().empty()
            || !value.contains("folder_file_offset")
            || !value["folder_file_offset"].is_number_unsigned()
            || !value.contains("folder_directory")
            || !value["folder_directory"].is_boolean()) {
            throw std::runtime_error("Transfer chunk folder metadata is invalid.");
        }
        message.SetFolderRelativePath(
            value["folder_relative_path"].get<std::string>());
        message.SetFolderFileOffset(
            value["folder_file_offset"].get<std::uintmax_t>());
        message.SetFolderDirectory(value["folder_directory"].get<bool>());
    }
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

nlohmann::json appUpdateRequestToJson(const AppUpdateRequestMessage& message)
{
    if (message.GetRequestId().empty()
        || message.GetRequesterDeviceId().empty()
        || message.GetCurrentAppVersion() < 0
        || message.GetRequestedAppVersion() < 0) {
        throw std::runtime_error(
            "App update request header is missing required fields.");
    }

    return nlohmann::json{
        {"request_id", message.GetRequestId()},
        {"requester_device_id", message.GetRequesterDeviceId()},
        {"current_app_version", message.GetCurrentAppVersion()},
        {"requested_app_version", message.GetRequestedAppVersion()},
    };
}

AppUpdateRequestMessage appUpdateRequestFromJson(const nlohmann::json& value)
{
    AppUpdateRequestMessage message;
    message.SetRequestId(readRequiredString(value, "request_id"));
    message.SetRequesterDeviceId(readRequiredString(value, "requester_device_id"));
    message.SetCurrentAppVersion(
        readRequiredAppVersion(value, "current_app_version"));
    message.SetRequestedAppVersion(
        readRequiredAppVersion(value, "requested_app_version"));
    return message;
}

nlohmann::json appUpdateChunkToJson(const AppUpdateChunkMessage& message)
{
    if (message.GetRequestId().empty() || message.GetFileSize() == 0) {
        throw std::runtime_error(
            "App update chunk header is missing required fields.");
    }

    return nlohmann::json{
        {"request_id", message.GetRequestId()},
        {"offset", message.GetOffset()},
        {"file_size", message.GetFileSize()},
    };
}

AppUpdateChunkMessage appUpdateChunkFromJson(const nlohmann::json& value)
{
    AppUpdateChunkMessage message;
    message.SetRequestId(readRequiredString(value, "request_id"));
    message.SetOffset(readRequiredOffset(value));
    message.SetFileSize(readRequiredFileSize(value));
    return message;
}

nlohmann::json appUpdateCompleteToJson(const AppUpdateCompleteMessage& message)
{
    if (message.GetRequestId().empty()
        || message.GetAppVersion() < 0
        || message.GetFileName().empty()) {
        throw std::runtime_error(
            "App update complete header is missing required fields.");
    }

    return nlohmann::json{
        {"request_id", message.GetRequestId()},
        {"app_version", message.GetAppVersion()},
        {"file_name", message.GetFileName()},
        {"file_size", message.GetFileSize()},
    };
}

AppUpdateCompleteMessage appUpdateCompleteFromJson(const nlohmann::json& value)
{
    AppUpdateCompleteMessage message;
    message.SetRequestId(readRequiredString(value, "request_id"));
    message.SetAppVersion(readRequiredAppVersion(value, "app_version"));
    message.SetFileName(readRequiredString(value, "file_name"));
    message.SetFileSize(readRequiredFileSize(value));
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

std::string serializeTransferAcceptHeader(const TransferAcceptMessage& message)
{
    return makeEnvelope(
        kPeerMessageTypeTransferAccept,
        transferAcceptToJson(message)).dump();
}

TransferAcceptMessage parseTransferAcceptHeader(const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeTransferAccept);
    return transferAcceptFromJson(value["message"]);
}

PeerFrame makeTransferAcceptFrame(const TransferAcceptMessage& message)
{
    return PeerFrame(PeerFrameType::TransferAccept,
                     serializeTransferAcceptHeader(message));
}

TransferAcceptMessage parseTransferAcceptFrame(const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::TransferAccept) {
        throw std::runtime_error("Peer frame is not a transfer_accept frame.");
    }
    requireEmptyBody(frame, "Transfer accept");
    return parseTransferAcceptHeader(frame.GetHeader());
}

std::string serializeTransferRejectHeader(const TransferRejectMessage& message)
{
    return makeEnvelope(
        kPeerMessageTypeTransferReject,
        transferRejectToJson(message)).dump();
}

TransferRejectMessage parseTransferRejectHeader(const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeTransferReject);
    return transferRejectFromJson(value["message"]);
}

PeerFrame makeTransferRejectFrame(const TransferRejectMessage& message)
{
    return PeerFrame(PeerFrameType::TransferReject,
                     serializeTransferRejectHeader(message));
}

TransferRejectMessage parseTransferRejectFrame(const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::TransferReject) {
        throw std::runtime_error("Peer frame is not a transfer_reject frame.");
    }
    requireEmptyBody(frame, "Transfer reject");
    return parseTransferRejectHeader(frame.GetHeader());
}

std::string serializeTransferCancelHeader(const TransferCancelMessage& message)
{
    return makeEnvelope(
        kPeerMessageTypeTransferCancel,
        transferCancelToJson(message)).dump();
}

TransferCancelMessage parseTransferCancelHeader(const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeTransferCancel);
    return transferCancelFromJson(value["message"]);
}

PeerFrame makeTransferCancelFrame(const TransferCancelMessage& message)
{
    return PeerFrame(PeerFrameType::TransferCancel,
                     serializeTransferCancelHeader(message));
}

TransferCancelMessage parseTransferCancelFrame(const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::TransferCancel) {
        throw std::runtime_error("Peer frame is not a transfer_cancel frame.");
    }
    requireEmptyBody(frame, "Transfer cancel");
    return parseTransferCancelHeader(frame.GetHeader());
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

std::string serializeAppUpdateRequestHeader(
    const AppUpdateRequestMessage& message)
{
    return makeEnvelope(
        kPeerMessageTypeAppUpdateRequest,
        appUpdateRequestToJson(message)).dump();
}

AppUpdateRequestMessage parseAppUpdateRequestHeader(const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeAppUpdateRequest);
    return appUpdateRequestFromJson(value["message"]);
}

PeerFrame makeAppUpdateRequestFrame(const AppUpdateRequestMessage& message)
{
    return PeerFrame(PeerFrameType::AppUpdateRequest,
                     serializeAppUpdateRequestHeader(message));
}

AppUpdateRequestMessage parseAppUpdateRequestFrame(const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::AppUpdateRequest) {
        throw std::runtime_error("Peer frame is not an app_update_request frame.");
    }
    requireEmptyBody(frame, "App update request");
    return parseAppUpdateRequestHeader(frame.GetHeader());
}

std::string serializeAppUpdateChunkHeader(const AppUpdateChunkMessage& message)
{
    return makeEnvelope(
        kPeerMessageTypeAppUpdateChunk,
        appUpdateChunkToJson(message)).dump();
}

AppUpdateChunkMessage parseAppUpdateChunkHeader(const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeAppUpdateChunk);
    return appUpdateChunkFromJson(value["message"]);
}

PeerFrame makeAppUpdateChunkFrame(AppUpdateChunkMessage message,
                                  std::vector<std::uint8_t> body)
{
    return PeerFrame(PeerFrameType::AppUpdateChunk,
                     serializeAppUpdateChunkHeader(message),
                     std::move(body));
}

AppUpdateChunkMessage parseAppUpdateChunkFrame(const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::AppUpdateChunk) {
        throw std::runtime_error("Peer frame is not an app_update_chunk frame.");
    }
    return parseAppUpdateChunkHeader(frame.GetHeader());
}

std::string serializeAppUpdateCompleteHeader(
    const AppUpdateCompleteMessage& message)
{
    return makeEnvelope(
        kPeerMessageTypeAppUpdateComplete,
        appUpdateCompleteToJson(message)).dump();
}

AppUpdateCompleteMessage parseAppUpdateCompleteHeader(const std::string& payload)
{
    const nlohmann::json value = parseEnvelope(payload);
    requireEnvelope(value, kPeerMessageTypeAppUpdateComplete);
    return appUpdateCompleteFromJson(value["message"]);
}

PeerFrame makeAppUpdateCompleteFrame(const AppUpdateCompleteMessage& message)
{
    return PeerFrame(PeerFrameType::AppUpdateComplete,
                     serializeAppUpdateCompleteHeader(message));
}

AppUpdateCompleteMessage parseAppUpdateCompleteFrame(const PeerFrame& frame)
{
    if (frame.GetType() != PeerFrameType::AppUpdateComplete) {
        throw std::runtime_error("Peer frame is not an app_update_complete frame.");
    }
    requireEmptyBody(frame, "App update complete");
    return parseAppUpdateCompleteHeader(frame.GetHeader());
}

}
