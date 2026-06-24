#include "storage/history_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace relaydesk::storage {
namespace {

constexpr int kSchemaVersion = 2;
constexpr int kLegacySchemaVersion = 1;
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

std::string toJsonValue(MessagePartType partType)
{
    switch (partType) {
    case MessagePartType::Text:
        return "text";
    case MessagePartType::Emoji:
        return "emoji";
    case MessagePartType::Image:
        return "image";
    case MessagePartType::File:
        return "file";
    case MessagePartType::Folder:
        return "folder";
    }

    throw std::runtime_error("Unsupported message part type.");
}

MessagePartType messagePartTypeFromJsonValue(const std::string& value)
{
    if (value == "text") {
        return MessagePartType::Text;
    }
    if (value == "emoji") {
        return MessagePartType::Emoji;
    }
    if (value == "image") {
        return MessagePartType::Image;
    }
    if (value == "file") {
        return MessagePartType::File;
    }
    if (value == "folder") {
        return MessagePartType::Folder;
    }

    throw std::runtime_error("Unsupported message part type value.");
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

std::string toJsonValue(TransferState transferState)
{
    switch (transferState) {
    case TransferState::Pending:
        return "pending";
    case TransferState::Offered:
        return "offered";
    case TransferState::Transferring:
        return "transferring";
    case TransferState::Interrupted:
        return "interrupted";
    case TransferState::Completed:
        return "completed";
    case TransferState::Failed:
        return "failed";
    case TransferState::Cancelled:
        return "cancelled";
    case TransferState::Rejected:
        return "rejected";
    }

    throw std::runtime_error("Unsupported transfer state.");
}

TransferState transferStateFromJsonValue(const std::string& value)
{
    if (value == "pending") {
        return TransferState::Pending;
    }
    if (value == "offered") {
        return TransferState::Offered;
    }
    if (value == "transferring") {
        return TransferState::Transferring;
    }
    if (value == "interrupted") {
        return TransferState::Interrupted;
    }
    if (value == "completed") {
        return TransferState::Completed;
    }
    if (value == "failed") {
        return TransferState::Failed;
    }
    if (value == "cancelled") {
        return TransferState::Cancelled;
    }
    if (value == "rejected") {
        return TransferState::Rejected;
    }

    throw std::runtime_error("Unsupported transfer state value.");
}

TransferState transferStateFromLegacyDeliveryState(DeliveryState deliveryState)
{
    switch (deliveryState) {
    case DeliveryState::Pending:
        return TransferState::Pending;
    case DeliveryState::Sent:
    case DeliveryState::Delivered:
    case DeliveryState::Received:
        return TransferState::Offered;
    case DeliveryState::Completed:
        return TransferState::Completed;
    case DeliveryState::Failed:
        return TransferState::Failed;
    case DeliveryState::Cancelled:
        return TransferState::Cancelled;
    }

    throw std::runtime_error("Unsupported delivery state.");
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

std::optional<std::uintmax_t> readOptionalUnsigned(
    const nlohmann::json& value,
    const char* fieldName)
{
    if (!value.contains(fieldName)) {
        return std::nullopt;
    }
    if (!value[fieldName].is_number_unsigned()) {
        throw std::runtime_error("Chat history record has invalid unsigned field.");
    }

    return value[fieldName].get<std::uintmax_t>();
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

void addOptionalString(nlohmann::json& value,
                       const char* fieldName,
                       const std::optional<std::string>& fieldValue)
{
    if (fieldValue.has_value()) {
        value[fieldName] = *fieldValue;
    }
}

void addOptionalFileSize(nlohmann::json& value,
                         const std::optional<std::uintmax_t>& fieldValue)
{
    if (fieldValue.has_value()) {
        value["file_size"] = *fieldValue;
    }
}

void addOptionalUnsigned(nlohmann::json& value,
                         const char* fieldName,
                         const std::optional<std::uintmax_t>& fieldValue)
{
    if (fieldValue.has_value()) {
        value[fieldName] = *fieldValue;
    }
}

bool hasRequiredString(const std::optional<std::string>& value)
{
    return value.has_value() && !value->empty();
}

void validatePart(const ChatMessagePart& part)
{
    if (part.GetPartId().empty()) {
        throw std::runtime_error("Chat message part is missing part ID.");
    }

    switch (part.GetType()) {
    case MessagePartType::Text:
        if (!hasRequiredString(part.GetText())) {
            throw std::runtime_error("Text message part is missing text.");
        }
        return;
    case MessagePartType::Emoji:
        if (!hasRequiredString(part.GetEmoji())) {
            throw std::runtime_error("Emoji message part is missing emoji.");
        }
        return;
    case MessagePartType::Image:
    case MessagePartType::File:
        if (!hasRequiredString(part.GetTransferId())
            || !part.GetTransferState().has_value()
            || !hasRequiredString(part.GetFileName())
            || !part.GetFileSize().has_value()
            || !hasRequiredString(part.GetLocalPath())) {
            throw std::runtime_error("File-like message part is missing transfer fields.");
        }
        return;
    case MessagePartType::Folder:
        if (!hasRequiredString(part.GetTransferId())
            || !part.GetTransferState().has_value()
            || !hasRequiredString(part.GetFileName())
            || !hasRequiredString(part.GetLocalPath())) {
            throw std::runtime_error("Folder message part is missing transfer fields.");
        }
        return;
    }

    throw std::runtime_error("Unsupported chat history part type.");
}

void validateRecord(const ChatMessageRecord& record)
{
    validateCoreFields(record);
    if (record.GetParts().empty()) {
        throw std::runtime_error("Chat history record is missing message parts.");
    }

    for (const auto& part : record.GetParts()) {
        validatePart(part);
    }
}

nlohmann::json partToJson(const ChatMessagePart& part)
{
    validatePart(part);

    nlohmann::json value{
        {"part_id", part.GetPartId()},
        {"type", toJsonValue(part.GetType())},
    };

    addOptionalString(value, "text", part.GetText());
    addOptionalString(value, "emoji", part.GetEmoji());
    addOptionalString(value, "transfer_id", part.GetTransferId());
    if (part.GetTransferState().has_value()) {
        value["transfer_state"] = toJsonValue(*part.GetTransferState());
    }
    addOptionalString(value, "file_name", part.GetFileName());
    addOptionalFileSize(value, part.GetFileSize());
    addOptionalUnsigned(value, "transferred_size", part.GetTransferredSize());
    addOptionalString(value, "sha256", part.GetSha256());
    addOptionalString(value, "local_path", part.GetLocalPath());
    addOptionalString(value, "manifest_path", part.GetManifestPath());

    return value;
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
        {"delivery_state", toJsonValue(record.GetDeliveryState())},
    };

    nlohmann::json parts = nlohmann::json::array();
    for (const auto& part : record.GetParts()) {
        parts.push_back(partToJson(part));
    }
    value["parts"] = std::move(parts);

    return value;
}

void readCommonRecordFields(const nlohmann::json& value, ChatMessageRecord& record)
{
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
    record.SetDeliveryState(
        deliveryStateFromJsonValue(readRequiredString(value, "delivery_state")));
}

ChatMessagePart partFromJson(const nlohmann::json& value)
{
    if (!value.is_object()) {
        throw std::runtime_error("Chat message part must be an object.");
    }

    ChatMessagePart part;
    part.SetPartId(readRequiredString(value, "part_id"));
    part.SetType(messagePartTypeFromJsonValue(readRequiredString(value, "type")));

    switch (part.GetType()) {
    case MessagePartType::Text:
        part.SetText(readRequiredString(value, "text"));
        break;
    case MessagePartType::Emoji:
        part.SetEmoji(readRequiredString(value, "emoji"));
        break;
    case MessagePartType::Image:
    case MessagePartType::File:
        part.SetTransferId(readRequiredString(value, "transfer_id"));
        part.SetTransferState(
            transferStateFromJsonValue(readRequiredString(value, "transfer_state")));
        part.SetFileName(readRequiredString(value, "file_name"));
        part.SetFileSize(readRequiredFileSize(value));
        if (const auto transferredSize =
                readOptionalUnsigned(value, "transferred_size")) {
            part.SetTransferredSize(*transferredSize);
        }
        part.SetLocalPath(readRequiredString(value, "local_path"));
        if (value.contains("sha256") && value["sha256"].is_string()) {
            part.SetSha256(value["sha256"].get<std::string>());
        }
        break;
    case MessagePartType::Folder:
        part.SetTransferId(readRequiredString(value, "transfer_id"));
        part.SetTransferState(
            transferStateFromJsonValue(readRequiredString(value, "transfer_state")));
        part.SetFileName(readRequiredString(value, "file_name"));
        if (const auto fileSize = readOptionalUnsigned(value, "file_size")) {
            part.SetFileSize(*fileSize);
        }
        part.SetLocalPath(readRequiredString(value, "local_path"));
        if (value.contains("manifest_path") && value["manifest_path"].is_string()) {
            part.SetManifestPath(value["manifest_path"].get<std::string>());
        }
        break;
    }

    validatePart(part);
    return part;
}

ChatMessagePart legacyPartFromJson(const nlohmann::json& value,
                                   DeliveryState deliveryState)
{
    ChatMessagePart part;
    part.SetPartId("legacy-1");
    part.SetType(messagePartTypeFromJsonValue(readRequiredString(value, "content_type")));

    switch (part.GetType()) {
    case MessagePartType::Text:
        part.SetText(readRequiredString(value, "text"));
        break;
    case MessagePartType::Emoji:
        part.SetEmoji(readRequiredString(value, "emoji"));
        break;
    case MessagePartType::Image:
    case MessagePartType::File:
        part.SetTransferId(readRequiredString(value, "transfer_id"));
        part.SetTransferState(transferStateFromLegacyDeliveryState(deliveryState));
        part.SetFileName(readRequiredString(value, "file_name"));
        part.SetFileSize(readRequiredFileSize(value));
        part.SetLocalPath(readRequiredString(value, "local_path"));
        if (value.contains("sha256") && value["sha256"].is_string()) {
            part.SetSha256(value["sha256"].get<std::string>());
        }
        break;
    case MessagePartType::Folder:
        part.SetTransferId(readRequiredString(value, "transfer_id"));
        part.SetTransferState(transferStateFromLegacyDeliveryState(deliveryState));
        part.SetFileName(readRequiredString(value, "file_name"));
        if (const auto fileSize = readOptionalUnsigned(value, "file_size")) {
            part.SetFileSize(*fileSize);
        }
        part.SetLocalPath(readRequiredString(value, "local_path"));
        if (value.contains("manifest_path") && value["manifest_path"].is_string()) {
            part.SetManifestPath(value["manifest_path"].get<std::string>());
        }
        break;
    }

    validatePart(part);
    return part;
}

ChatMessageRecord fromJsonV2(const nlohmann::json& value)
{
    ChatMessageRecord record;
    readCommonRecordFields(value, record);

    if (!value.contains("parts") || !value["parts"].is_array()
        || value["parts"].empty()) {
        throw std::runtime_error("Chat history record is missing message parts.");
    }

    for (const auto& partValue : value["parts"]) {
        record.AddPart(partFromJson(partValue));
    }

    validateRecord(record);
    return record;
}

ChatMessageRecord fromJsonV1(const nlohmann::json& value)
{
    ChatMessageRecord record;
    readCommonRecordFields(value, record);
    record.AddPart(legacyPartFromJson(value, record.GetDeliveryState()));

    validateRecord(record);
    return record;
}

ChatMessageRecord fromJson(const nlohmann::json& value)
{
    if (!value.is_object() || value.value("record_type", "") != kRecordType) {
        throw std::runtime_error("Chat history record has unsupported schema.");
    }

    const int schemaVersion = value.value("schema_version", 0);
    if (schemaVersion == kSchemaVersion) {
        return fromJsonV2(value);
    }
    if (schemaVersion == kLegacySchemaVersion) {
        return fromJsonV1(value);
    }

    throw std::runtime_error("Chat history record has unsupported schema.");
}

} // namespace

ChatHistoryLoadResult::ChatHistoryLoadResult(
    std::vector<ChatMessageRecord> records,
    int skippedLineCount,
    bool hasMoreRecords)
    : records_(std::move(records)),
      skippedLineCount_(skippedLineCount),
      hasMoreRecords_(hasMoreRecords)
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

std::string serializeChatMessageRecord(const ChatMessageRecord& record)
{
    return toJson(record).dump();
}

ChatMessageRecord parseChatMessageRecord(const std::string& payload)
{
    try {
        return fromJson(nlohmann::json::parse(payload));
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Chat history payload is invalid JSON.");
    }
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

    output << serializeChatMessageRecord(record) << '\n';
    if (!output) {
        throw std::runtime_error("Failed to append chat history record.");
    }
}

bool replaceChatMessage(const AppPaths& appPaths,
                        const std::string& peerDeviceId,
                        const ChatMessageRecord& record)
{
    const std::filesystem::path messagesFilePath =
        getPeerMessagesFilePath(appPaths, peerDeviceId);
    if (!std::filesystem::exists(messagesFilePath)) {
        return false;
    }

    const ChatHistoryLoadResult history = loadChatHistory(appPaths, peerDeviceId);
    std::vector<ChatMessageRecord> records = history.GetRecords();
    const auto message = std::find_if(
        records.begin(),
        records.end(),
        [&record](const ChatMessageRecord& existing) {
            return existing.GetMessageId() == record.GetMessageId();
        });
    if (message == records.end()) {
        return false;
    }

    *message = record;
    const std::filesystem::path temporaryFilePath =
        messagesFilePath.string() + ".tmp";
    {
        std::ofstream output(temporaryFilePath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Failed to open chat history file for rewriting.");
        }

        for (const auto& item : records) {
            output << serializeChatMessageRecord(item) << '\n';
            if (!output) {
                throw std::runtime_error("Failed to rewrite chat history record.");
            }
        }
    }

    std::error_code error;
    std::filesystem::remove(messagesFilePath, error);
    error.clear();
    std::filesystem::rename(temporaryFilePath, messagesFilePath, error);
    if (error) {
        std::filesystem::remove(temporaryFilePath);
        throw std::runtime_error("Failed to replace chat history file.");
    }

    return true;
}

std::optional<ChatMessageRecord> loadChatMessage(const AppPaths& appPaths,
                                                 const std::string& peerDeviceId,
                                                 const std::string& messageId)
{
    if (messageId.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path messagesFilePath =
        getPeerMessagesFilePath(appPaths, peerDeviceId);
    if (!std::filesystem::exists(messagesFilePath)) {
        return std::nullopt;
    }

    std::ifstream input(messagesFilePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open chat history file for reading.");
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        try {
            ChatMessageRecord record = fromJson(nlohmann::json::parse(line));
            if (record.GetMessageId() == messageId) {
                return record;
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    return std::nullopt;
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

ChatHistoryLoadResult loadRecentChatHistory(const AppPaths& appPaths,
                                            const std::string& peerDeviceId,
                                            std::size_t maxRecordCount)
{
    if (maxRecordCount == 0) {
        return ChatHistoryLoadResult({}, 0);
    }

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
    std::size_t validRecordCount = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        try {
            records.push_back(fromJson(nlohmann::json::parse(line)));
            ++validRecordCount;
            if (records.size() > maxRecordCount) {
                records.erase(records.begin());
            }
        } catch (const std::exception&) {
            ++skippedLineCount;
        }
    }

    return ChatHistoryLoadResult(std::move(records),
                                 skippedLineCount,
                                 validRecordCount > maxRecordCount);
}

ChatHistoryLoadResult loadChatHistoryBefore(const AppPaths& appPaths,
                                            const std::string& peerDeviceId,
                                            const std::string& beforeMessageId,
                                            std::size_t maxRecordCount)
{
    if (beforeMessageId.empty() || maxRecordCount == 0) {
        return ChatHistoryLoadResult({}, 0);
    }

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
    std::size_t validRecordCount = 0;
    bool foundBoundary = false;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        try {
            ChatMessageRecord record = fromJson(nlohmann::json::parse(line));
            if (record.GetMessageId() == beforeMessageId) {
                foundBoundary = true;
                break;
            }

            records.push_back(std::move(record));
            ++validRecordCount;
            if (records.size() > maxRecordCount) {
                records.erase(records.begin());
            }
        } catch (const std::exception&) {
            ++skippedLineCount;
        }
    }

    if (!foundBoundary) {
        return ChatHistoryLoadResult({}, skippedLineCount);
    }

    return ChatHistoryLoadResult(std::move(records),
                                 skippedLineCount,
                                 validRecordCount > maxRecordCount);
}

std::string loadLatestChatMessageCreatedAt(const AppPaths& appPaths,
                                           const std::string& peerDeviceId)
{
    const std::filesystem::path messagesFilePath =
        getPeerMessagesFilePath(appPaths, peerDeviceId);
    if (!std::filesystem::exists(messagesFilePath)) {
        return {};
    }

    std::ifstream input(messagesFilePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open chat history file for reading.");
    }

    std::string latestCreatedAt;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        try {
            const ChatMessageRecord record = fromJson(nlohmann::json::parse(line));
            if (record.GetCreatedAt() > latestCreatedAt) {
                latestCreatedAt = record.GetCreatedAt();
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    return latestCreatedAt;
}

}
