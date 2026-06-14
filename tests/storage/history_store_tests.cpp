#include "storage/app_paths.h"
#include "storage/history_store.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int expect(bool condition, const char* message)
{
    if (!condition) {
        return fail(message);
    }
    return 0;
}

std::filesystem::path testRoot()
{
    return std::filesystem::path(RELAYDESK_HISTORY_TEST_WORK_DIR);
}

relaydesk::storage::AppPaths makeAppPaths(const std::filesystem::path& caseName)
{
    const std::filesystem::path workDirectory = testRoot() / caseName / "bin";
    std::filesystem::create_directories(workDirectory);
    return relaydesk::storage::AppPaths(workDirectory / "relaydesk.exe");
}

relaydesk::storage::ChatMessagePart makeTextPart(const std::string& partId,
                                                 const std::string& text)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::Text);
    part.SetText(text);
    return part;
}

relaydesk::storage::ChatMessagePart makeEmojiPart(const std::string& partId,
                                                  const std::string& emoji)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::Emoji);
    part.SetEmoji(emoji);
    return part;
}

relaydesk::storage::ChatMessagePart makeFileLikePart(
    const std::string& partId,
    relaydesk::storage::MessagePartType partType,
    const std::string& transferId,
    const std::string& fileName)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(partType);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Completed);
    part.SetFileName(fileName);
    part.SetFileSize(1024);
    part.SetSha256("hash-" + transferId);
    part.SetLocalPath("data/transfers/outbox/peer-device/" + transferId + "/"
                      + fileName);
    return part;
}

relaydesk::storage::ChatMessagePart makeFolderPart(const std::string& partId,
                                                   const std::string& transferId)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::Folder);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Completed);
    part.SetFileName("ProjectDocs");
    part.SetLocalPath("data/transfers/outbox/peer-device/" + transferId
                      + "/ProjectDocs");
    part.SetManifestPath("data/transfers/outbox/peer-device/" + transferId
                         + "/manifest.json");
    return part;
}

relaydesk::storage::ChatMessageRecord makeBaseRecord(const std::string& messageId)
{
    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId("local-device", "peer-device"));
    record.SetDirection(relaydesk::storage::MessageDirection::Outgoing);
    record.SetSenderDeviceId("local-device");
    record.SetReceiverDeviceId("peer-device");
    record.SetSenderDisplayNameSnapshot("Local User");
    record.SetReceiverDisplayNameSnapshot("Peer User");
    record.SetCreatedAt("2026-06-12T00:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Sent);
    return record;
}

relaydesk::storage::ChatMessageRecord makeTextRecord(const std::string& messageId)
{
    auto record = makeBaseRecord(messageId);
    record.AddPart(makeTextPart("p1", "hello"));
    return record;
}

relaydesk::storage::ChatMessageRecord makeMixedRecord(const std::string& messageId)
{
    auto record = makeBaseRecord(messageId);
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Delivered);
    record.AddPart(makeTextPart("p1", "please review this"));
    record.AddPart(makeEmojiPart("p2", "thumbs_up"));
    record.AddPart(makeFileLikePart("p3",
                                    relaydesk::storage::MessagePartType::Image,
                                    "transfer-image",
                                    "screenshot.png"));
    record.AddPart(makeFileLikePart("p4",
                                    relaydesk::storage::MessagePartType::File,
                                    "transfer-file",
                                    "report.pdf"));
    record.AddPart(makeFolderPart("p5", "transfer-folder"));
    return record;
}

nlohmann::json readFirstHistoryJsonLine(
    const relaydesk::storage::AppPaths& appPaths)
{
    const auto messagesFilePath =
        relaydesk::storage::getPeerMessagesFilePath(appPaths, "peer-device");
    std::ifstream input(messagesFilePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to read test history file.");
    }

    std::string line;
    std::getline(input, line);
    return nlohmann::json::parse(line);
}

void appendRawHistoryLine(const relaydesk::storage::AppPaths& appPaths,
                          const nlohmann::json& value)
{
    const auto messagesFilePath =
        relaydesk::storage::getPeerMessagesFilePath(appPaths, "peer-device");
    std::filesystem::create_directories(messagesFilePath.parent_path());
    std::ofstream output(messagesFilePath, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("Failed to append raw test history line.");
    }

    output << value.dump() << '\n';
}

nlohmann::json makeLegacyBaseRecord(const std::string& messageId)
{
    return nlohmann::json{
        {"schema_version", 1},
        {"record_type", "message"},
        {"message_id", messageId},
        {"conversation_id",
         relaydesk::storage::makeDirectConversationId("local-device", "peer-device")},
        {"direction", "in"},
        {"sender_device_id", "peer-device"},
        {"receiver_device_id", "local-device"},
        {"sender_display_name_snapshot", "Peer User"},
        {"receiver_display_name_snapshot", "Local User"},
        {"created_at", "2026-06-12T00:00:00Z"},
        {"delivery_state", "received"},
    };
}

int appendsAndLoadsMixedMessageParts()
{
    const auto appPaths = makeAppPaths("mixed-parts");
    relaydesk::storage::appendChatMessage(appPaths, "peer-device",
                                          makeMixedRecord("m1"));

    const auto rawValue = readFirstHistoryJsonLine(appPaths);
    if (const int check = expect(rawValue.value("schema_version", 0) == 2,
                                 "New history record did not use schema v2.");
        check != 0) {
        return check;
    }
    if (const int check = expect(!rawValue.contains("content_type"),
                                 "New history record still writes content_type.");
        check != 0) {
        return check;
    }
    if (const int check = expect(rawValue.contains("parts")
                                     && rawValue["parts"].is_array()
                                     && rawValue["parts"].size() == 5,
                                 "New history record did not write all parts.");
        check != 0) {
        return check;
    }

    const auto result = relaydesk::storage::loadChatHistory(appPaths, "peer-device");
    if (const int check = expect(result.GetSkippedLineCount() == 0,
                                 "Valid history lines were skipped.");
        check != 0) {
        return check;
    }

    const auto& records = result.GetRecords();
    if (const int check = expect(records.size() == 1,
                                 "Loaded history record count mismatch.");
        check != 0) {
        return check;
    }

    const auto& parts = records[0].GetParts();
    if (const int check = expect(parts.size() == 5,
                                 "Loaded mixed message part count mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(parts[0].GetText().value() == "please review this",
                                 "Text part did not round-trip.");
        check != 0) {
        return check;
    }
    if (const int check = expect(parts[1].GetEmoji().value() == "thumbs_up",
                                 "Emoji part did not round-trip.");
        check != 0) {
        return check;
    }
    if (const int check = expect(parts[2].GetType()
                                     == relaydesk::storage::MessagePartType::Image,
                                 "Image part type mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(parts[3].GetFileName().value() == "report.pdf",
                                 "File part name mismatch.");
        check != 0) {
        return check;
    }

    return expect(parts[4].GetManifestPath().has_value(),
                  "Folder part manifest path was not loaded.");
}

int keepsConversationIdStableAcrossDeviceOrder()
{
    const std::string first =
        relaydesk::storage::makeDirectConversationId("device-b", "device-a");
    const std::string second =
        relaydesk::storage::makeDirectConversationId("device-a", "device-b");

    if (const int check = expect(first == second,
                                 "Conversation ID changed with device order.");
        check != 0) {
        return check;
    }

    return expect(first == "dm_device-a_device-b",
                  "Conversation ID format mismatch.");
}

int loadsLegacySingleContentRecordsAsParts()
{
    const auto appPaths = makeAppPaths("legacy-v1");

    nlohmann::json textRecord = makeLegacyBaseRecord("legacy-text");
    textRecord["content_type"] = "text";
    textRecord["text"] = "legacy hello";
    appendRawHistoryLine(appPaths, textRecord);

    nlohmann::json fileRecord = makeLegacyBaseRecord("legacy-file");
    fileRecord["content_type"] = "file";
    fileRecord["transfer_id"] = "legacy-transfer";
    fileRecord["file_name"] = "legacy.pdf";
    fileRecord["file_size"] = 2048;
    fileRecord["sha256"] = "legacy-hash";
    fileRecord["local_path"] =
        "data/transfers/outbox/peer-device/legacy-transfer/legacy.pdf";
    appendRawHistoryLine(appPaths, fileRecord);

    const auto result = relaydesk::storage::loadChatHistory(appPaths, "peer-device");
    if (const int check = expect(result.GetSkippedLineCount() == 0,
                                 "Legacy history lines were skipped.");
        check != 0) {
        return check;
    }
    if (const int check = expect(result.GetRecords().size() == 2,
                                 "Legacy history record count mismatch.");
        check != 0) {
        return check;
    }

    const auto& textParts = result.GetRecords()[0].GetParts();
    if (const int check = expect(textParts.size() == 1,
                                 "Legacy text record was not loaded as one part.");
        check != 0) {
        return check;
    }
    if (const int check = expect(textParts[0].GetText().value() == "legacy hello",
                                 "Legacy text part mismatch.");
        check != 0) {
        return check;
    }

    const auto& fileParts = result.GetRecords()[1].GetParts();
    if (const int check = expect(fileParts.size() == 1,
                                 "Legacy file record was not loaded as one part.");
        check != 0) {
        return check;
    }
    if (const int check = expect(fileParts[0].GetType()
                                     == relaydesk::storage::MessagePartType::File,
                                 "Legacy file part type mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(fileParts[0].GetTransferState().value()
                                     == relaydesk::storage::TransferState::Offered,
                                 "Legacy file transfer state mapping mismatch.");
        check != 0) {
        return check;
    }

    return expect(fileParts[0].GetFileName().value() == "legacy.pdf",
                  "Legacy file name mismatch.");
}

int skipsBrokenJsonLines()
{
    const auto appPaths = makeAppPaths("broken-lines");
    relaydesk::storage::appendChatMessage(appPaths, "peer-device",
                                          makeTextRecord("m1"));

    const auto messagesFilePath =
        relaydesk::storage::getPeerMessagesFilePath(appPaths, "peer-device");
    {
        std::ofstream output(messagesFilePath, std::ios::binary | std::ios::app);
        if (!output) {
            throw std::runtime_error("Failed to append broken test line.");
        }
        output << "{not-json}\n";
    }

    relaydesk::storage::appendChatMessage(appPaths, "peer-device",
                                          makeTextRecord("m2"));
    const auto result = relaydesk::storage::loadChatHistory(appPaths, "peer-device");
    if (const int check = expect(result.GetSkippedLineCount() == 1,
                                 "Broken history line was not counted.");
        check != 0) {
        return check;
    }

    return expect(result.GetRecords().size() == 2,
                  "Broken history line prevented valid records from loading.");
}

int replacesExistingMessageRecord()
{
    const auto appPaths = makeAppPaths("replace-record");
    relaydesk::storage::appendChatMessage(appPaths, "peer-device",
                                          makeTextRecord("m1"));
    relaydesk::storage::appendChatMessage(appPaths, "peer-device",
                                          makeTextRecord("m2"));

    auto replacement = makeTextRecord("m1");
    replacement.SetDeliveryState(relaydesk::storage::DeliveryState::Completed);
    replacement.SetParts({makeTextPart("p1", "updated")});

    const bool replaced =
        relaydesk::storage::replaceChatMessage(appPaths, "peer-device", replacement);
    if (const int check = expect(replaced,
                                 "Existing history record was not replaced.");
        check != 0) {
        return check;
    }

    const auto result = relaydesk::storage::loadChatHistory(appPaths, "peer-device");
    if (const int check = expect(result.GetRecords().size() == 2,
                                 "Replacing a history record changed record count.");
        check != 0) {
        return check;
    }
    if (const int check = expect(result.GetRecords()[0].GetDeliveryState()
                                     == relaydesk::storage::DeliveryState::Completed,
                                 "Replaced record delivery state was not saved.");
        check != 0) {
        return check;
    }
    if (const int check = expect(result.GetRecords()[0].GetParts()[0]
                                     .GetText().value() == "updated",
                                 "Replaced record content was not saved.");
        check != 0) {
        return check;
    }

    return expect(result.GetRecords()[1].GetMessageId() == "m2",
                  "Replacing a history record reordered unrelated records.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = appendsAndLoadsMixedMessageParts(); result != 0) {
        return result;
    }

    if (const int result = keepsConversationIdStableAcrossDeviceOrder(); result != 0) {
        return result;
    }

    if (const int result = loadsLegacySingleContentRecordsAsParts(); result != 0) {
        return result;
    }

    if (const int result = skipsBrokenJsonLines(); result != 0) {
        return result;
    }

    if (const int result = replacesExistingMessageRecord(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
