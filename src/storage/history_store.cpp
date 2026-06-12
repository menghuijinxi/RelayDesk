#include "storage/history_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace relaydesk::storage {
namespace {

constexpr int kSchemaVersion = 1;
constexpr const char* kRecordType = "message";

std::string toJsonValue(MessageDirection direction)
{
    switch (direction) {
    case MessageDirection::Incoming:
        return "in";
    case MessageDirection::Outgoing:
        return "out";
    }

    throw std::runtime_error("Unsupported message direction.");
}

MessageDirection messageDirectionFromJsonValue(const std::string& value)
{
    if (value == "in") {
        return MessageDirection::Incoming;
    }
    if (value == "out") {
        return MessageDirection::Outgoing;
    }

    throw std::runtime_error("Unsupported message direction value.");
}

std::string toJsonValue(MessageContentType contentType)
{
    switch (contentType) {
    case MessageContentType::Text:
        return "text";
    case MessageContentType::Emoji:
        return "emoji";
    case MessageContentType::Image:
        return "image";
    case MessageContentType::File:
        return "file";
    case MessageContentType::Folder:
        return "folder";
    }

    throw std::runtime_error("Unsupported message content type.");
}

MessageContentType messageContentTypeFromJsonValue(const std::string& value)
{
    if (value == "text") {
        return MessageContentType::Text;
    }
    if (value == "emoji") {
        return MessageContentType::Emoji;
    }
    if (value == "image") {
        return MessageContentType::Image;
    }
    if (value == "file") {
        return MessageContentType::File;
    }
    if (value == "folder") {
        return MessageContentType::Folder;
    }

    throw std::runtime_error("Unsupported message content type value.");
}

std::string toJsonValue(DeliveryState deliveryState)
{
    switch (deliveryState) {
    case DeliveryState::Pending:
        return "pending";
    case DeliveryState::Sent:
        return "sent";
    case DeliveryState::Delivered:
        return "delivered";
    case DeliveryState::Received:
        return "received";
    case DeliveryState::Completed:
        return "completed";
    case DeliveryState::Failed:
        return "failed";
    case DeliveryState::Cancelled:
        return "cancelled";
    }

    throw std::runtime_error("Unsupported delivery state.");
}

DeliveryState deliveryStateFromJsonValue(const std::string& value)
{
    if (value == "pending") {
        return DeliveryState::Pending;
    }
    if (value == "sent") {
        return DeliveryState::Sent;
    }
    if (value == "delivered") {
        return DeliveryState::Delivered;
    }
    if (value == "received") {
        return DeliveryState::Received;
    }
    if (value == "completed") {
        return DeliveryState::Completed;
    }
    if (value == "failed") {
        return DeliveryState::Failed;
    }
    if (value == "cancelled") {
        return DeliveryState::Cancelled;
    }

    throw std::runtime_error("Unsupported delivery state value.");
}

void requireStringField(const nlohmann::json& value, const char* fieldName)
{
    if (!value.contains(fieldName) || !value[fieldName].is_string()
        || value[fieldName].get<std::string>().empty()) {
        throw std::runtime_error("Chat history record is missing a required field.");
    }
}

std::string readRequiredString(const nlohmann::json& value, const char* fieldName)
{
    requireStringField(value, fieldName);
    return value[fieldName].get<std::string>();
}

std::uintmax_t readRequiredFileSize(const nlohmann::json& value)
{
    if (!value.contains("file_size") || !value["file_size"].is_number_unsigned()) {
        throw std::runtime_error("Chat history record is missing file size.");
    }

    return value["file_size"].get<std::uintmax_t>();
}

void validateCoreFields(const ChatMessageRecord& record)
{
    if (record.GetSchemaVersion() != kSchemaVersion
        || record.GetMessageId().empty()
        || record.GetConversationId().empty()
        || record.GetSenderDeviceId().empty()
        || record.GetReceiverDeviceId().empty()
        || record.GetSenderDisplayNameSnapshot().empty()
        || record.GetReceiverDisplayNameSnapshot().empty()
        || record.GetCreatedAt().empty()) {
        throw std::runtime_error("Chat history record is missing required core fields.");
    }
}

void validateContentFields(const ChatMessageRecord& record)
{
    switch (record.GetContentType()) {
    case MessageContentType::Text:
        if (!record.GetText().has_value() || record.GetText()->empty()) {
            throw std::runtime_error("Text message record is missing text.");
        }
        return;
    case MessageContentType::Emoji:
        if (!record.GetEmoji().has_value() || record.GetEmoji()->empty()) {
            throw std::runtime_error("Emoji message record is missing emoji.");
        }
        return;
    case MessageContentType::Image:
    case MessageContentType::File:
        if (!record.GetTransferId().has_value() || record.GetTransferId()->empty()
            || !record.GetFileName().has_value() || record.GetFileName()->empty()
            || !record.GetFileSize().has_value()
            || !record.GetLocalPath().has_value() || record.GetLocalPath()->empty()) {
            throw std::runtime_error("File-like message record is missing transfer fields.");
        }
        return;
    case MessageContentType::Folder:
        if (!record.GetTransferId().has_value() || record.GetTransferId()->empty()
            || !record.GetFileName().has_value() || record.GetFileName()->empty()
            || !record.GetLocalPath().has_value() || record.GetLocalPath()->empty()
            || !record.GetManifestPath().has_value()
            || record.GetManifestPath()->empty()) {
            throw std::runtime_error("Folder message record is missing transfer fields.");
        }
        return;
    }

    throw std::runtime_error("Unsupported chat history content type.");
}

void validateRecord(const ChatMessageRecord& record)
{
    validateCoreFields(record);
    validateContentFields(record);
}

void addOptionalString(nlohmann::json& value,
                       const char* fieldName,
                       const std::optional<std::string>& fieldValue)
{
    if (fieldValue.has_value()) {
        value[fieldName] = *fieldValue;
    }
}

nlohmann::json toJson(const ChatMessageRecord& record)
{
    validateRecord(record);

    nlohmann::json value{
        {"schema_version", kSchemaVersion},
        {"record_type", kRecordType},
        {"message_id", record.GetMessageId()},
        {"conversation_id", record.GetConversationId()},
        {"direction", toJsonValue(record.GetDirection())},
        {"sender_device_id", record.GetSenderDeviceId()},
        {"receiver_device_id", record.GetReceiverDeviceId()},
        {"sender_display_name_snapshot", record.GetSenderDisplayNameSnapshot()},
        {"receiver_display_name_snapshot", record.GetReceiverDisplayNameSnapshot()},
        {"created_at", record.GetCreatedAt()},
        {"content_type", toJsonValue(record.GetContentType())},
        {"delivery_state", toJsonValue(record.GetDeliveryState())},
    };

    addOptionalString(value, "text", record.GetText());
    addOptionalString(value, "emoji", record.GetEmoji());
    addOptionalString(value, "transfer_id", record.GetTransferId());
    addOptionalString(value, "file_name", record.GetFileName());
    if (record.GetFileSize().has_value()) {
        value["file_size"] = *record.GetFileSize();
    }
    addOptionalString(value, "sha256", record.GetSha256());
    addOptionalString(value, "local_path", record.GetLocalPath());
    addOptionalString(value, "manifest_path", record.GetManifestPath());

    return value;
}

ChatMessageRecord fromJson(const nlohmann::json& value)
{
    if (!value.is_object()
        || value.value("schema_version", 0) != kSchemaVersion
        || value.value("record_type", "") != kRecordType) {
        throw std::runtime_error("Chat history record has unsupported schema.");
    }

    ChatMessageRecord record;
    record.SetMessageId(readRequiredString(value, "message_id"));
    record.SetConversationId(readRequiredString(value, "conversation_id"));
    record.SetDirection(
        messageDirectionFromJsonValue(readRequiredString(value, "direction")));
    record.SetSenderDeviceId(readRequiredString(value, "sender_device_id"));
    record.SetReceiverDeviceId(readRequiredString(value, "receiver_device_id"));
    record.SetSenderDisplayNameSnapshot(
        readRequiredString(value, "sender_display_name_snapshot"));
    record.SetReceiverDisplayNameSnapshot(
        readRequiredString(value, "receiver_display_name_snapshot"));
    record.SetCreatedAt(readRequiredString(value, "created_at"));
    record.SetContentType(
        messageContentTypeFromJsonValue(readRequiredString(value, "content_type")));
    record.SetDeliveryState(
        deliveryStateFromJsonValue(readRequiredString(value, "delivery_state")));

    switch (record.GetContentType()) {
    case MessageContentType::Text:
        record.SetText(readRequiredString(value, "text"));
        break;
    case MessageContentType::Emoji:
        record.SetEmoji(readRequiredString(value, "emoji"));
        break;
    case MessageContentType::Image:
    case MessageContentType::File:
        record.SetTransferId(readRequiredString(value, "transfer_id"));
        record.SetFileName(readRequiredString(value, "file_name"));
        record.SetFileSize(readRequiredFileSize(value));
        record.SetLocalPath(readRequiredString(value, "local_path"));
        if (value.contains("sha256") && value["sha256"].is_string()) {
            record.SetSha256(value["sha256"].get<std::string>());
        }
        break;
    case MessageContentType::Folder:
        record.SetTransferId(readRequiredString(value, "transfer_id"));
        record.SetFileName(readRequiredString(value, "file_name"));
        record.SetLocalPath(readRequiredString(value, "local_path"));
        record.SetManifestPath(readRequiredString(value, "manifest_path"));
        break;
    }

    validateRecord(record);
    return record;
}

} // namespace

ChatHistoryLoadResult::ChatHistoryLoadResult(std::vector<ChatMessageRecord> records,
                                             int skippedLineCount)
    : records_(std::move(records)),
      skippedLineCount_(skippedLineCount)
{
}

std::string makeDirectConversationId(const std::string& localDeviceId,
                                     const std::string& peerDeviceId)
{
    if (localDeviceId.empty() || peerDeviceId.empty()) {
        throw std::invalid_argument("Conversation device IDs cannot be empty.");
    }

    const auto first = std::min(localDeviceId, peerDeviceId);
    const auto second = std::max(localDeviceId, peerDeviceId);
    return "dm_" + first + "_" + second;
}

std::filesystem::path getPeerMessagesFilePath(const AppPaths& appPaths,
                                              const std::string& peerDeviceId)
{
    if (peerDeviceId.empty()) {
        throw std::invalid_argument("Peer device ID cannot be empty.");
    }

    return appPaths.GetPeersDirectory() / peerDeviceId / "messages.jsonl";
}

void appendChatMessage(const AppPaths& appPaths,
                       const std::string& peerDeviceId,
                       const ChatMessageRecord& record)
{
    const std::filesystem::path messagesFilePath =
        getPeerMessagesFilePath(appPaths, peerDeviceId);
    std::filesystem::create_directories(messagesFilePath.parent_path());

    std::ofstream output(messagesFilePath, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("Failed to open chat history file for appending.");
    }

    output << toJson(record).dump() << '\n';
    if (!output) {
        throw std::runtime_error("Failed to append chat history record.");
    }
}

ChatHistoryLoadResult loadChatHistory(const AppPaths& appPaths,
                                      const std::string& peerDeviceId)
{
    const std::filesystem::path messagesFilePath =
        getPeerMessagesFilePath(appPaths, peerDeviceId);
    if (!std::filesystem::exists(messagesFilePath)) {
        return ChatHistoryLoadResult({}, 0);
    }

    std::ifstream input(messagesFilePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open chat history file for reading.");
    }

    std::vector<ChatMessageRecord> records;
    int skippedLineCount = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        try {
            records.push_back(fromJson(nlohmann::json::parse(line)));
        } catch (const std::exception&) {
            ++skippedLineCount;
        }
    }

    return ChatHistoryLoadResult(std::move(records), skippedLineCount);
}

}
