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

    return expect(decoded.GetSha256().value() == "hash-transfer-1",
                  "Decoded transfer offer hash mismatch.");
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
    if (const int result = roundTripsTransferChunkFrame(); result != 0) {
        return result;
    }
    if (const int result = roundTripsTransferCompleteFrame(); result != 0) {
        return result;
    }

    return 0;
}
