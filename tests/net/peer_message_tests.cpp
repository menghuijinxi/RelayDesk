#include "net/peer_frame.h"
#include "net/peer_message.h"
#include "storage/avatar_store.h"
#include "storage/history_store.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
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
    relaydesk::storage::ChatMessageQuote quote;
    quote.SetMessageId("original-message");
    quote.SetSenderDisplayName("Peer");
    quote.SetPreviewText("原消息摘要");
    record.SetQuote(std::move(quote));
    record.AddPart(makeTextPart("p1", "hello"));
    record.AddPart(makeEmojiPart("p2", "thumbs_up"));
    return record;
}

relaydesk::storage::ChatMessageRecord makeScreenShakeRecord()
{
    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId("relaydesk-event:screen-shake:v1:event-1");
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId("local-device", "peer-device"));
    record.SetDirection(relaydesk::storage::MessageDirection::Outgoing);
    record.SetSenderDeviceId("local-device");
    record.SetReceiverDeviceId("peer-device");
    record.SetSenderDisplayNameSnapshot("Local");
    record.SetReceiverDisplayNameSnapshot("Peer");
    record.SetCreatedAt("2026-08-10T00:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Pending);
    record.AddPart(makeTextPart("relaydesk-event:screen-shake:v1",
                                "relaydesk-event:screen-shake:v1"));
    return record;
}

int recognizesOnlyStrictScreenShakeChatMessages()
{
    const relaydesk::storage::ChatMessageRecord decoded =
        relaydesk::net::parseChatMessageFrame(
            relaydesk::net::makeChatMessageFrame(makeScreenShakeRecord()));
    if (const int check = expect(
            relaydesk::net::isScreenShakeChatMessage(decoded),
            "Strict screen shake chat message was not recognized.");
        check != 0) {
        return check;
    }

    relaydesk::storage::ChatMessageRecord ordinary = decoded;
    ordinary.SetMessageId("ordinary-message");
    if (const int check = expect(
            !relaydesk::net::isScreenShakeChatMessage(ordinary),
            "Ordinary chat message was recognized as a screen shake event.");
        check != 0) {
        return check;
    }

    relaydesk::storage::ChatMessageRecord malformed = decoded;
    malformed.SetParts({makeTextPart("relaydesk-event:screen-shake:v1",
                                     "different-event")});
    return expect(!relaydesk::net::isScreenShakeChatMessage(malformed),
                  "Malformed screen shake chat message was recognized.");
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
    if (const int check = expect(decoded.GetQuote().has_value(),
                                 "Decoded chat message quote is missing.");
        check != 0) {
        return check;
    }
    if (const int check = expect(
            decoded.GetQuote()->GetMessageId() == "original-message" &&
                decoded.GetQuote()->GetSenderDisplayName() == "Peer" &&
                decoded.GetQuote()->GetPreviewText() == "原消息摘要",
            "Decoded chat message quote mismatch.");
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
    message.SetResumeRequest(true);
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
    message.SetResumeOffset(2);
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

relaydesk::net::AppUpdateRequestMessage makeAppUpdateRequest()
{
    relaydesk::net::AppUpdateRequestMessage message;
    message.SetRequestId("update-request-1");
    message.SetRequesterDeviceId("peer-device");
    message.SetCurrentAppVersion(1);
    message.SetRequestedAppVersion(2);
    return message;
}

relaydesk::net::AppUpdateChunkMessage makeAppUpdateChunk()
{
    relaydesk::net::AppUpdateChunkMessage message;
    message.SetRequestId("update-request-1");
    message.SetOffset(4);
    message.SetFileSize(8);
    return message;
}

relaydesk::net::AppUpdateCompleteMessage makeAppUpdateComplete()
{
    relaydesk::net::AppUpdateCompleteMessage message;
    message.SetRequestId("update-request-1");
    message.SetAppVersion(2);
    message.SetFileName("relaydesk.exe");
    message.SetFileSize(8);
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
    if (const int check = expect(decoded.GetResumeRequest(),
                                 "Decoded transfer offer resume flag mismatch.");
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
    if (const int check = expect(!decoded.GetResumeRequest(),
                                 "Legacy transfer offer was treated as resume.");
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
    if (const int check = expect(decoded.GetResumeOffset() == 2,
                                 "Decoded transfer accept resume offset mismatch.");
        check != 0) {
        return check;
    }
    return expect(decoded.GetSaveStrategy()
                      == relaydesk::net::TransferSaveStrategy::Overwrite,
                  "Decoded transfer accept save strategy mismatch.");
}

int treatsLegacyTransferAcceptAsFreshUniqueReceive()
{
    const relaydesk::net::TransferAcceptMessage decoded =
        relaydesk::net::parseTransferAcceptHeader(
            R"({
                "protocol": "relaydesk.peer",
                "version": 1,
                "type": "transfer_accept",
                "message": {
                    "message_id": "message-1",
                    "part_id": "p3",
                    "transfer_id": "transfer-1",
                    "receiver_device_id": "peer-device"
                }
            })");
    if (const int check = expect(decoded.GetResumeOffset() == 0,
                                 "Legacy transfer accept resume offset mismatch.");
        check != 0) {
        return check;
    }
    return expect(decoded.GetSaveStrategy()
                      == relaydesk::net::TransferSaveStrategy::Unique,
                  "Legacy transfer accept save strategy mismatch.");
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

int roundTripsFolderTransferChunkFrame()
{
    relaydesk::net::TransferChunkMessage message = makeTransferChunk();
    message.SetFolderRelativePath("nested/file.txt");
    message.SetFolderFileOffset(8);
    message.SetFolderDirectory(false);

    const relaydesk::net::PeerFrame decodedFrame =
        relaydesk::net::decodePeerFrame(
            relaydesk::net::encodePeerFrame(
                relaydesk::net::makeTransferChunkFrame(message, {})));
    const relaydesk::net::TransferChunkMessage decoded =
        relaydesk::net::parseTransferChunkFrame(decodedFrame);
    if (const int check =
            expect(decoded.GetFolderRelativePath().has_value()
                       && decoded.GetFolderRelativePath().value()
                           == "nested/file.txt",
                   "Decoded folder transfer chunk path mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetFolderFileOffset() == 8,
                                 "Decoded folder transfer chunk offset mismatch.");
        check != 0) {
        return check;
    }
    return expect(!decoded.GetFolderDirectory(),
                  "Decoded folder transfer chunk directory flag mismatch.");
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

int roundTripsAppUpdateRequestFrame()
{
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeAppUpdateRequestFrame(makeAppUpdateRequest());
    if (const int check = expect(
            frame.GetType() == relaydesk::net::PeerFrameType::AppUpdateRequest,
            "App update request frame type mismatch.");
        check != 0) {
        return check;
    }

    const relaydesk::net::AppUpdateRequestMessage decoded =
        relaydesk::net::parseAppUpdateRequestFrame(
            relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame)));
    if (const int check = expect(decoded.GetRequestId() == "update-request-1",
                                 "Decoded app update request ID mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetRequesterDeviceId() == "peer-device",
                                 "Decoded app update requester mismatch.");
        check != 0) {
        return check;
    }
    return expect(decoded.GetRequestedAppVersion() == 2,
                  "Decoded app update requested version mismatch.");
}

int roundTripsAppUpdateChunkFrame()
{
    const std::vector<std::uint8_t> body{0x01, 0x02, 0x03, 0x04};
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeAppUpdateChunkFrame(makeAppUpdateChunk(), body);
    if (const int check = expect(
            frame.GetType() == relaydesk::net::PeerFrameType::AppUpdateChunk,
            "App update chunk frame type mismatch.");
        check != 0) {
        return check;
    }

    const relaydesk::net::PeerFrame decodedFrame =
        relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame));
    const relaydesk::net::AppUpdateChunkMessage decoded =
        relaydesk::net::parseAppUpdateChunkFrame(decodedFrame);
    if (const int check = expect(decoded.GetOffset() == 4,
                                 "Decoded app update chunk offset mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetFileSize() == 8,
                                 "Decoded app update chunk file size mismatch.");
        check != 0) {
        return check;
    }
    return expect(decodedFrame.GetBody() == body,
                  "Decoded app update chunk body mismatch.");
}


int roundTripsAvatarRequestFrame()
{
    relaydesk::net::AvatarRequestMessage message;
    message.SetRequesterDeviceId("local-device");
    message.SetDeviceId("peer-device");
    const std::string avatarHash(64, 'a');
    message.SetAvatarSha256(avatarHash);
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeAvatarRequestFrame(message);
    if (const int result = expect(frame.GetBody().empty(),
                                  "Avatar request frame should not have a body.");
        result != 0) {
        return result;
    }

    const auto decoded = relaydesk::net::parseAvatarRequestFrame(
        relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame)));
    if (const int result = expect(decoded.GetRequesterDeviceId() == "local-device",
                                  "Avatar request requester did not round-trip.");
        result != 0) {
        return result;
    }
    if (const int result = expect(decoded.GetDeviceId() == "peer-device",
                                  "Avatar request device did not round-trip.");
        result != 0) {
        return result;
    }
    return expect(decoded.GetAvatarSha256() == avatarHash,
                  "Avatar request hash did not round-trip.");
}

int roundTripsAvatarFrame()
{
    relaydesk::net::AvatarMessage message;
    message.SetDeviceId("peer-device");
    const std::string avatarHash(64, 'b');
    message.SetAvatarSha256(avatarHash);
    const std::vector<std::uint8_t> body{1, 2, 3, 4};
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeAvatarFrame(message, body);
    const relaydesk::net::PeerFrame decodedFrame =
        relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame));
    const auto decoded = relaydesk::net::parseAvatarFrame(decodedFrame);
    if (const int result = expect(decoded.GetDeviceId() == "peer-device",
                                  "Avatar device did not round-trip.");
        result != 0) {
        return result;
    }
    if (const int result = expect(decoded.GetAvatarSha256() == avatarHash,
                                  "Avatar hash did not round-trip.");
        result != 0) {
        return result;
    }
    return expect(decodedFrame.GetBody() == body,
                  "Avatar body did not round-trip.");
}

int roundTripsClearedAvatarFrame()
{
    relaydesk::net::AvatarMessage message;
    message.SetDeviceId("peer-device");
    message.SetAvatarSha256("");
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeAvatarFrame(message, {});
    const auto decoded = relaydesk::net::parseAvatarFrame(
        relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame)));
    return expect(decoded.GetDeviceId() == "peer-device"
                      && decoded.GetAvatarSha256().empty(),
                  "Cleared avatar frame did not round-trip.");
}

int rejectsInvalidAvatarFrames()
{
    relaydesk::net::AvatarRequestMessage request;
    request.SetRequesterDeviceId("local-device");
    request.SetDeviceId("peer-device");
    request.SetAvatarSha256("short");
    if (const int result = expectThrows(
            [&] { relaydesk::net::makeAvatarRequestFrame(request); },
            "Invalid avatar request was accepted.");
        result != 0) {
        return result;
    }

    relaydesk::net::AvatarMessage message;
    message.SetDeviceId("peer-device");
    message.SetAvatarSha256(std::string(64, 'a'));
    if (const int result = expectThrows(
            [&] { relaydesk::net::makeAvatarFrame(message, {}); },
            "Avatar frame without a body was accepted.");
        result != 0) {
        return result;
    }

    message.SetAvatarSha256("");
    if (const int result = expectThrows(
            [&] {
                relaydesk::net::makeAvatarFrame(
                    message,
                    std::vector<std::uint8_t>{1});
            },
            "Cleared avatar frame accepted a body.");
        result != 0) {
        return result;
    }

    const std::vector<std::uint8_t> oversized(
        relaydesk::storage::kMaxAvatarImageBytes + 1u,
        static_cast<std::uint8_t>(1));
    message.SetAvatarSha256(std::string(64, 'a'));
    return expectThrows(
        [&] { relaydesk::net::makeAvatarFrame(message, oversized); },
        "Oversized avatar frame was accepted.");
}

int roundTripsAppUpdateCompleteFrame()
{
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeAppUpdateCompleteFrame(makeAppUpdateComplete());
    const relaydesk::net::AppUpdateCompleteMessage decoded =
        relaydesk::net::parseAppUpdateCompleteFrame(
            relaydesk::net::decodePeerFrame(relaydesk::net::encodePeerFrame(frame)));
    if (const int check = expect(decoded.GetRequestId() == "update-request-1",
                                 "Decoded app update complete ID mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetAppVersion() == 2,
                                 "Decoded app update complete version mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetFileName() == "relaydesk.exe",
                                 "Decoded app update complete file name mismatch.");
        check != 0) {
        return check;
    }
    return expect(decoded.GetFileSize() == 8,
                  "Decoded app update complete file size mismatch.");
}

} // namespace

int main()
{
    if (const int result = roundTripsChatMessageFrame(); result != 0) {
        return result;
    }
    if (const int result = recognizesOnlyStrictScreenShakeChatMessages();
        result != 0) {
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
    if (const int result = treatsLegacyTransferAcceptAsFreshUniqueReceive();
        result != 0) {
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
    if (const int result = roundTripsFolderTransferChunkFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsTransferCompleteFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsAppUpdateRequestFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsAppUpdateChunkFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsAppUpdateCompleteFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsAvatarRequestFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsAvatarFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsClearedAvatarFrame(); result != 0) {
        return result;
    }
    if (const int result = rejectsInvalidAvatarFrames(); result != 0) {
        return result;
    }

    return 0;
}
