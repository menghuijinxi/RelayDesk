#include "storage/app_paths.h"
#include "storage/history_store.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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
    return record;
}

relaydesk::storage::ChatMessageRecord makeTextRecord(const std::string& messageId)
{
    auto record = makeBaseRecord(messageId);
    record.SetContentType(relaydesk::storage::MessageContentType::Text);
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Sent);
    record.SetText("hello");
    return record;
}

relaydesk::storage::ChatMessageRecord makeEmojiRecord(const std::string& messageId)
{
    auto record = makeBaseRecord(messageId);
    record.SetContentType(relaydesk::storage::MessageContentType::Emoji);
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Delivered);
    record.SetEmoji(":thumbs_up:");
    return record;
}

relaydesk::storage::ChatMessageRecord makeFileLikeRecord(
    const std::string& messageId,
    relaydesk::storage::MessageContentType contentType,
    const std::string& fileName)
{
    auto record = makeBaseRecord(messageId);
    record.SetContentType(contentType);
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Completed);
    record.SetTransferId("transfer-" + messageId);
    record.SetFileName(fileName);
    record.SetFileSize(1024);
    record.SetSha256("hash-" + messageId);
    record.SetLocalPath("data/transfers/outbox/peer-device/transfer-" + messageId + "/"
                        + fileName);
    return record;
}

relaydesk::storage::ChatMessageRecord makeFolderRecord(const std::string& messageId)
{
    auto record = makeBaseRecord(messageId);
    record.SetContentType(relaydesk::storage::MessageContentType::Folder);
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Completed);
    record.SetTransferId("transfer-" + messageId);
    record.SetFileName("ProjectDocs");
    record.SetLocalPath("data/transfers/outbox/peer-device/transfer-" + messageId
                        + "/ProjectDocs");
    record.SetManifestPath("data/transfers/outbox/peer-device/transfer-" + messageId
                           + "/manifest.json");
    return record;
}

int appendsAndLoadsSupportedMessageTypes()
{
    const auto appPaths = makeAppPaths("supported-types");

    relaydesk::storage::appendChatMessage(appPaths, "peer-device", makeTextRecord("m1"));
    relaydesk::storage::appendChatMessage(appPaths, "peer-device", makeEmojiRecord("m2"));
    relaydesk::storage::appendChatMessage(
        appPaths,
        "peer-device",
        makeFileLikeRecord("m3", relaydesk::storage::MessageContentType::Image, "photo.png"));
    relaydesk::storage::appendChatMessage(
        appPaths,
        "peer-device",
        makeFileLikeRecord("m4", relaydesk::storage::MessageContentType::File, "report.pdf"));
    relaydesk::storage::appendChatMessage(appPaths, "peer-device", makeFolderRecord("m5"));

    const auto result = relaydesk::storage::loadChatHistory(appPaths, "peer-device");
    if (const int check = expect(result.GetSkippedLineCount() == 0,
                                 "Valid history lines were skipped.");
        check != 0) {
        return check;
    }

    const auto& records = result.GetRecords();
    if (const int check = expect(records.size() == 5,
                                 "Loaded history record count mismatch.");
        check != 0) {
        return check;
    }

    if (const int check = expect(records[0].GetText().value() == "hello",
                                 "Text message did not round-trip.");
        check != 0) {
        return check;
    }

    if (const int check = expect(records[1].GetEmoji().value() == ":thumbs_up:",
                                 "Emoji message did not round-trip.");
        check != 0) {
        return check;
    }

    if (const int check = expect(records[2].GetContentType()
                                     == relaydesk::storage::MessageContentType::Image,
                                 "Image message content type mismatch.");
        check != 0) {
        return check;
    }

    if (const int check = expect(records[3].GetFileName().value() == "report.pdf",
                                 "File message file name mismatch.");
        check != 0) {
        return check;
    }

    return expect(records[4].GetManifestPath().has_value(),
                  "Folder message manifest path was not loaded.");
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

int skipsBrokenJsonLines()
{
    const auto appPaths = makeAppPaths("broken-lines");
    relaydesk::storage::appendChatMessage(appPaths, "peer-device", makeTextRecord("m1"));

    const auto messagesFilePath =
        relaydesk::storage::getPeerMessagesFilePath(appPaths, "peer-device");
    {
        std::ofstream output(messagesFilePath, std::ios::binary | std::ios::app);
        if (!output) {
            throw std::runtime_error("Failed to append broken test line.");
        }
        output << "{not-json}\n";
    }

    relaydesk::storage::appendChatMessage(appPaths, "peer-device", makeEmojiRecord("m2"));
    const auto result = relaydesk::storage::loadChatHistory(appPaths, "peer-device");
    if (const int check = expect(result.GetSkippedLineCount() == 1,
                                 "Broken history line was not counted.");
        check != 0) {
        return check;
    }

    return expect(result.GetRecords().size() == 2,
                  "Broken history line prevented valid records from loading.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = appendsAndLoadsSupportedMessageTypes(); result != 0) {
        return result;
    }

    if (const int result = keepsConversationIdStableAcrossDeviceOrder(); result != 0) {
        return result;
    }

    if (const int result = skipsBrokenJsonLines(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
