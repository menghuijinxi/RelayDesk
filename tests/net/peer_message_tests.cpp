#include "net/peer_frame.h"
#include "net/peer_message.h"
#include "storage/history_store.h"

#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

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

int expectThrows(const std::function<void()>& action, const char* message)
{
    try {
        action();
    } catch (const std::exception&) {
        return 0;
    }

    return fail(message);
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

relaydesk::storage::ChatMessageRecord makeMixedTextEmojiRecord()
{
    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId("message-1");
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId("local-device", "peer-device"));
    record.SetDirection(relaydesk::storage::MessageDirection::Outgoing);
    record.SetSenderDeviceId("local-device");
    record.SetReceiverDeviceId("peer-device");
    record.SetSenderDisplayNameSnapshot("Local");
    record.SetReceiverDisplayNameSnapshot("Peer");
    record.SetCreatedAt("2026-06-14T00:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Pending);
    record.AddPart(makeTextPart("p1", "hello"));
    record.AddPart(makeEmojiPart("p2", "thumbs_up"));
    return record;
}

int roundTripsChatMessageFrame()
{
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeChatMessageFrame(makeMixedTextEmojiRecord());
    if (const int check = expect(frame.GetType()
                                     == relaydesk::net::PeerFrameType::ChatMessage,
                                 "Chat message frame type mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(frame.GetBody().empty(),
                                 "Chat message frame should not contain a body.");
        check != 0) {
        return check;
    }

    const auto encoded = relaydesk::net::encodePeerFrame(frame);
    const relaydesk::net::PeerFrame decodedFrame =
        relaydesk::net::decodePeerFrame(encoded);
    const relaydesk::storage::ChatMessageRecord decoded =
        relaydesk::net::parseChatMessageFrame(decodedFrame);

    if (const int check = expect(decoded.GetMessageId() == "message-1",
                                 "Decoded chat message ID mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetDeliveryState()
                                     == relaydesk::storage::DeliveryState::Pending,
                                 "Decoded chat message state mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetParts().size() == 2,
                                 "Decoded chat message part count mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetParts()[0].GetText().value() == "hello",
                                 "Decoded text part mismatch.");
        check != 0) {
        return check;
    }

    return expect(decoded.GetParts()[1].GetEmoji().value() == "thumbs_up",
                  "Decoded emoji part mismatch.");
}

int rejectsNonChatFrame()
{
    relaydesk::net::PeerFrame frame(relaydesk::net::PeerFrameType::Heartbeat,
                                    "{}");
    return expectThrows(
        [&] {
            (void)relaydesk::net::parseChatMessageFrame(frame);
        },
        "Non-chat peer frame was accepted as chat_message.");
}

int rejectsChatFrameBody()
{
    relaydesk::net::PeerFrame frame =
        relaydesk::net::makeChatMessageFrame(makeMixedTextEmojiRecord());
    frame.SetBody({0x01});
    return expectThrows(
        [&] {
            (void)relaydesk::net::parseChatMessageFrame(frame);
        },
        "Chat message frame body was accepted.");
}

int rejectsInvalidEnvelope()
{
    return expectThrows(
        [] {
            (void)relaydesk::net::parsePeerChatMessageHeader(
                R"({"protocol":"relaydesk.peer","version":1,"type":"heartbeat"})");
        },
        "Invalid peer message envelope was accepted.");
}

relaydesk::net::TransferOfferMessage makeTransferOffer()
{
    relaydesk::net::TransferOfferMessage message;
    message.SetMessageId("message-1");
    message.SetPartId("p3");
    message.SetTransferId("transfer-1");
    message.SetSenderDeviceId("local-device");
    message.SetFileName("photo.png");
    message.SetFileSize(4);
    message.SetSha256("hash-transfer-1");
    message.SetImageTransfer(true);
    return message;
}

relaydesk::net::TransferOfferMessage makeFolderTransferOffer()
{
    relaydesk::net::TransferOfferMessage message;
    message.SetMessageId("message-folder");
    message.SetPartId("p4");
    message.SetTransferId("transfer-folder");
    message.SetSenderDeviceId("local-device");
    message.SetFileName("Project");
    message.SetFileSize(128);
    message.SetFolderTransfer(true);
    return message;
}

relaydesk::net::TransferChunkMessage makeTransferChunk()
{
    relaydesk::net::TransferChunkMessage message;
    message.SetMessageId("message-1");
    message.SetPartId("p3");
    message.SetTransferId("transfer-1");
    message.SetOffset(2);
    return message;
}

relaydesk::net::TransferAcceptMessage makeTransferAccept()
{
    relaydesk::net::TransferAcceptMessage message;
    message.SetMessageId("message-1");
    message.SetPartId("p3");
    message.SetTransferId("transfer-1");
    message.SetReceiverDeviceId("peer-device");
    message.SetSaveStrategy(relaydesk::net::TransferSaveStrategy::Overwrite);
    return message;
}

relaydesk::net::TransferRejectMessage makeTransferReject()
{
    relaydesk::net::TransferRejectMessage message;
    message.SetMessageId("message-1");
    message.SetPartId("p3");
    message.SetTransferId("transfer-1");
    message.SetReceiverDeviceId("peer-device");
    message.SetReason("user_rejected");
    return message;
}

relaydesk::net::TransferCancelMessage makeTransferCancel()
{
    relaydesk::net::TransferCancelMessage message;
    message.SetMessageId("message-1");
    message.SetPartId("p3");
    message.SetTransferId("transfer-1");
    message.SetCancellerDeviceId("local-device");
    message.SetReason("user_cancelled");
    return message;
}

relaydesk::net::TransferCompleteMessage makeTransferComplete()
{
    relaydesk::net::TransferCompleteMessage message;
    message.SetMessageId("message-1");
    message.SetPartId("p3");
    message.SetTransferId("transfer-1");
    message.SetFileSize(4);
    message.SetSha256("hash-transfer-1");
    return message;
}

int roundTripsTransferOfferFrame()
{
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferOfferFrame(makeTransferOffer());
    if (const int check = expect(frame.GetType()
                                     == relaydesk::net::PeerFrameType::TransferOffer,
                                 "Transfer offer frame type mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(frame.GetBody().empty(),
                                 "Transfer offer frame should not contain a body.");
        check != 0) {
        return check;
    }

    const auto encoded = relaydesk::net::encodePeerFrame(frame);
    const relaydesk::net::TransferOfferMessage decoded =
        relaydesk::net::parseTransferOfferFrame(
            relaydesk::net::decodePeerFrame(encoded));
    if (const int check = expect(decoded.GetTransferId() == "transfer-1",
                                 "Decoded transfer offer ID mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetFileName() == "photo.png",
                                 "Decoded transfer offer file name mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetImageTransfer(),
                                 "Decoded transfer offer image flag mismatch.");
        check != 0) {
        return check;
    }

    return expect(decoded.GetSha256().value() == "hash-transfer-1",
                  "Decoded transfer offer hash mismatch.");
}

int treatsLegacyTransferOfferAsFile()
{
    const relaydesk::net::TransferOfferMessage decoded =
        relaydesk::net::parseTransferOfferHeader(
            R"({
                "protocol": "relaydesk.peer",
                "version": 1,
                "type": "transfer_offer",
                "message": {
                    "message_id": "message-1",
                    "part_id": "p3",
                    "transfer_id": "transfer-1",
                    "sender_device_id": "local-device",
                    "file_name": "photo.png",
                    "file_size": 4
                }
            })");
    if (const int check = expect(!decoded.GetImageTransfer(),
                                 "Legacy transfer offer was treated as an image.");
        check != 0) {
        return check;
    }
    return expect(!decoded.GetFolderTransfer(),
                  "Legacy transfer offer was treated as a folder.");
}

int roundTripsFolderTransferOfferFrame()
{
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferOfferFrame(makeFolderTransferOffer());
    const relaydesk::net::TransferOfferMessage decoded =
        relaydesk::net::parseTransferOfferFrame(
            relaydesk::net::decodePeerFrame(
                relaydesk::net::encodePeerFrame(frame)));
    if (const int check = expect(decoded.GetFolderTransfer(),
                                 "Decoded folder transfer flag mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(!decoded.GetImageTransfer(),
                                 "Folder transfer was treated as image.");
        check != 0) {
        return check;
    }
    return expect(decoded.GetFileName() == "Project",
                  "Decoded folder transfer name mismatch.");
}

int roundTripsTransferAcceptFrame()
{
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferAcceptFrame(makeTransferAccept());
    if (const int check = expect(frame.GetType()
                                     == relaydesk::net::PeerFrameType::TransferAccept,
                                 "Transfer accept frame type mismatch.");
        check != 0) {
        return check;
    }

    const relaydesk::net::TransferAcceptMessage decoded =
        relaydesk::net::parseTransferAcceptFrame(
            relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame)));
    if (const int check = expect(decoded.GetReceiverDeviceId() == "peer-device",
                                 "Decoded transfer accept receiver mismatch.");
        check != 0) {
        return check;
    }
    return expect(decoded.GetSaveStrategy()
                      == relaydesk::net::TransferSaveStrategy::Overwrite,
                  "Decoded transfer accept save strategy mismatch.");
}

int roundTripsTransferRejectFrame()
{
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferRejectFrame(makeTransferReject());
    if (const int check = expect(frame.GetType()
                                     == relaydesk::net::PeerFrameType::TransferReject,
                                 "Transfer reject frame type mismatch.");
        check != 0) {
        return check;
    }

    const relaydesk::net::TransferRejectMessage decoded =
        relaydesk::net::parseTransferRejectFrame(
            relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame)));
    if (const int check = expect(decoded.GetReceiverDeviceId() == "peer-device",
                                 "Decoded transfer reject receiver mismatch.");
        check != 0) {
        return check;
    }
    return expect(decoded.GetReason() == "user_rejected",
                  "Decoded transfer reject reason mismatch.");
}

int roundTripsTransferCancelFrame()
{
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferCancelFrame(makeTransferCancel());
    if (const int check = expect(frame.GetType()
                                     == relaydesk::net::PeerFrameType::TransferCancel,
                                 "Transfer cancel frame type mismatch.");
        check != 0) {
        return check;
    }

    const relaydesk::net::TransferCancelMessage decoded =
        relaydesk::net::parseTransferCancelFrame(
            relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame)));
    if (const int check = expect(decoded.GetCancellerDeviceId() == "local-device",
                                 "Decoded transfer cancel device mismatch.");
        check != 0) {
        return check;
    }
    return expect(decoded.GetReason() == "user_cancelled",
                  "Decoded transfer cancel reason mismatch.");
}

int roundTripsTransferChunkFrame()
{
    const std::vector<std::uint8_t> body{0x10, 0x20, 0x30, 0x40};
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferChunkFrame(makeTransferChunk(), body);
    if (const int check = expect(frame.GetType()
                                     == relaydesk::net::PeerFrameType::TransferChunk,
                                 "Transfer chunk frame type mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(frame.GetBody() == body,
                                 "Transfer chunk body mismatch before encoding.");
        check != 0) {
        return check;
    }

    const relaydesk::net::PeerFrame decodedFrame =
        relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame));
    const relaydesk::net::TransferChunkMessage decoded =
        relaydesk::net::parseTransferChunkFrame(decodedFrame);
    if (const int check = expect(decoded.GetOffset() == 2,
                                 "Decoded transfer chunk offset mismatch.");
        check != 0) {
        return check;
    }

    return expect(decodedFrame.GetBody() == body,
                  "Decoded transfer chunk body mismatch.");
}

int roundTripsTransferCompleteFrame()
{
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferCompleteFrame(makeTransferComplete());
    const relaydesk::net::TransferCompleteMessage decoded =
        relaydesk::net::parseTransferCompleteFrame(
            relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame)));
    if (const int check = expect(decoded.GetMessageId() == "message-1",
                                 "Decoded transfer complete message ID mismatch.");
        check != 0) {
        return check;
    }

    return expect(decoded.GetFileSize() == 4,
                  "Decoded transfer complete file size mismatch.");
}

} // namespace

int main()
{
    if (const int result = roundTripsChatMessageFrame(); result != 0) {
        return result;
    }
    if (const int result = rejectsNonChatFrame(); result != 0) {
        return result;
    }
    if (const int result = rejectsChatFrameBody(); result != 0) {
        return result;
    }
    if (const int result = rejectsInvalidEnvelope(); result != 0) {
        return result;
    }
    if (const int result = roundTripsTransferOfferFrame(); result != 0) {
        return result;
    }
    if (const int result = treatsLegacyTransferOfferAsFile(); result != 0) {
        return result;
    }
    if (const int result = roundTripsFolderTransferOfferFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsTransferAcceptFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsTransferRejectFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsTransferCancelFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsTransferChunkFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsTransferCompleteFrame(); result != 0) {
        return result;
    }

    return 0;
}
