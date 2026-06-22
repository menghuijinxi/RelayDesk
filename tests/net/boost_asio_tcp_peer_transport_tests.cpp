#include "net/boost_asio_tcp_peer_transport.h"
#include "net/peer_message.h"
#include "storage/history_store.h"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio.hpp>

namespace {

using namespace std::chrono_literals;
using boost::asio::ip::tcp;

class ReceivedFrameState {
public:
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<relaydesk::net::PeerFrame> frame;
    std::vector<relaydesk::net::PeerFrame> frames;
    std::string remoteAddress;
    std::uint16_t remotePort = 0;
    std::vector<std::string> errors;
};

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

relaydesk::storage::ChatMessageRecord makeChatRecord()
{
    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId("tcp-message-1");
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId("sender-device",
                                                     "receiver-device"));
    record.SetDirection(relaydesk::storage::MessageDirection::Outgoing);
    record.SetSenderDeviceId("sender-device");
    record.SetReceiverDeviceId("receiver-device");
    record.SetSenderDisplayNameSnapshot("Sender");
    record.SetReceiverDisplayNameSnapshot("Receiver");
    record.SetCreatedAt("2026-06-14T01:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Pending);
    record.AddPart(makeTextPart("p1", "hello over tcp"));
    record.AddPart(makeEmojiPart("p2", "thumbs_up"));
    return record;
}

relaydesk::net::TransferChunkMessage makeTransferChunk()
{
    relaydesk::net::TransferChunkMessage message;
    message.SetMessageId("tcp-message-1");
    message.SetPartId("p3");
    message.SetTransferId("tcp-transfer-1");
    message.SetOffset(0);
    return message;
}

relaydesk::net::TransferOfferMessage makeTransferOffer()
{
    relaydesk::net::TransferOfferMessage message;
    message.SetMessageId("tcp-message-1");
    message.SetPartId("p3");
    message.SetTransferId("tcp-transfer-1");
    message.SetSenderDeviceId("sender-device");
    message.SetFileName("tcp-transfer.bin");
    message.SetFileSize(4);
    return message;
}

relaydesk::net::TransferCompleteMessage makeTransferComplete(
    std::uintmax_t fileSize)
{
    relaydesk::net::TransferCompleteMessage message;
    message.SetMessageId("tcp-message-1");
    message.SetPartId("p3");
    message.SetTransferId("tcp-transfer-1");
    message.SetFileSize(fileSize);
    return message;
}

bool waitForFrameOrError(ReceivedFrameState& state)
{
    std::unique_lock lock(state.mutex);
    return state.condition.wait_for(lock, 2s, [&state] {
        return state.frame.has_value() || !state.errors.empty();
    });
}

bool waitForFrameCountOrError(ReceivedFrameState& state, std::size_t count)
{
    std::unique_lock lock(state.mutex);
    return state.condition.wait_for(lock, 2s, [&state, count] {
        return state.frames.size() >= count || !state.errors.empty();
    });
}

bool waitForErrorCount(ReceivedFrameState& state, std::size_t count)
{
    std::unique_lock lock(state.mutex);
    return state.condition.wait_for(lock, 2s, [&state, count] {
        return state.errors.size() >= count;
    });
}

void writeRawFrame(tcp::socket& socket, const relaydesk::net::PeerFrame& frame)
{
    const std::array<std::uint8_t, relaydesk::net::kPeerFrameHeaderSize>
        frameHeader = relaydesk::net::encodePeerFrameHeader(frame);
    const std::array<boost::asio::const_buffer, 3> buffers{{
        boost::asio::buffer(frameHeader),
        boost::asio::buffer(frame.GetHeader()),
        boost::asio::buffer(frame.GetBody()),
    }};
    boost::asio::write(socket, buffers);
}

void writePartialRawFrameBody(tcp::socket& socket,
                              const relaydesk::net::PeerFrame& frame,
                              std::size_t bodyBytesToWrite)
{
    const std::array<std::uint8_t, relaydesk::net::kPeerFrameHeaderSize>
        frameHeader = relaydesk::net::encodePeerFrameHeader(frame);
    boost::asio::write(socket, boost::asio::buffer(frameHeader));
    boost::asio::write(socket, boost::asio::buffer(frame.GetHeader()));
    boost::asio::write(socket,
                       boost::asio::buffer(frame.GetBody().data(),
                                           bodyBytesToWrite));
}

int sendsAndReceivesChatMessageFrame()
{
    ReceivedFrameState state;
    relaydesk::net::BoostAsioTcpPeerTransport receiver(0);
    receiver.SetFrameCallback(
        [&state](relaydesk::net::PeerFrame frame,
                 std::string remoteAddress,
                 std::uint16_t remotePort) {
            {
                std::lock_guard lock(state.mutex);
                state.frame = std::move(frame);
                state.remoteAddress = std::move(remoteAddress);
                state.remotePort = remotePort;
            }
            state.condition.notify_all();
        });
    receiver.SetErrorCallback(
        [&state](std::string message) {
            {
                std::lock_guard lock(state.mutex);
                state.errors.push_back(std::move(message));
            }
            state.condition.notify_all();
        });
    receiver.start();

    relaydesk::net::BoostAsioTcpPeerTransport sender(0);
    sender.sendFrameTo("127.0.0.1",
                       receiver.GetLocalPort(),
                       relaydesk::net::makeChatMessageFrame(makeChatRecord()));

    const bool received = waitForFrameOrError(state);
    receiver.stop();

    if (const int check = expect(received,
                                 "TCP peer transport did not receive a frame.");
        check != 0) {
        return check;
    }
    if (const int check = expect(state.errors.empty(),
                                 "TCP peer transport reported an error.");
        check != 0) {
        return check;
    }
    if (const int check = expect(state.frame.has_value(),
                                 "TCP peer transport did not store a frame.");
        check != 0) {
        return check;
    }
    if (const int check = expect(!state.remoteAddress.empty()
                                     && state.remotePort > 0,
                                 "TCP peer transport remote endpoint was missing.");
        check != 0) {
        return check;
    }

    const relaydesk::storage::ChatMessageRecord record =
        relaydesk::net::parseChatMessageFrame(state.frame.value());
    if (const int check = expect(record.GetMessageId() == "tcp-message-1",
                                 "Received TCP chat message ID mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(record.GetParts().size() == 2,
                                 "Received TCP chat message part count mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(record.GetParts()[0].GetText().value()
                                     == "hello over tcp",
                                 "Received TCP text part mismatch.");
        check != 0) {
        return check;
    }

    return expect(record.GetParts()[1].GetEmoji().value() == "thumbs_up",
                  "Received TCP emoji part mismatch.");
}

int sendsAndReceivesTransferChunkFrame()
{
    ReceivedFrameState state;
    relaydesk::net::BoostAsioTcpPeerTransport receiver(0);
    receiver.SetFrameCallback(
        [&state](relaydesk::net::PeerFrame frame,
                 std::string remoteAddress,
                 std::uint16_t remotePort) {
            {
                std::lock_guard lock(state.mutex);
                if (!state.frame.has_value()) {
                    state.frame = std::move(frame);
                }
                state.remoteAddress = std::move(remoteAddress);
                state.remotePort = remotePort;
            }
            state.condition.notify_all();
        });
    receiver.SetErrorCallback(
        [&state](std::string message) {
            {
                std::lock_guard lock(state.mutex);
                state.errors.push_back(std::move(message));
            }
            state.condition.notify_all();
        });
    receiver.start();

    const std::vector<std::uint8_t> body{0x01, 0x02, 0x03, 0x04};
    relaydesk::net::BoostAsioTcpPeerTransport sender(0);
    int nextFrame = 0;
    sender.sendFramesTo(
        "127.0.0.1",
        receiver.GetLocalPort(),
        [&nextFrame, &body]() -> std::optional<relaydesk::net::PeerFrame> {
            ++nextFrame;
            if (nextFrame == 1) {
                return relaydesk::net::makeTransferChunkFrame(
                    makeTransferChunk(),
                    body);
            }
            if (nextFrame == 2) {
                return relaydesk::net::makeTransferCompleteFrame(
                    makeTransferComplete(body.size()));
            }
            return std::nullopt;
        });

    const bool received = waitForFrameOrError(state);
    receiver.stop();

    if (const int check = expect(received,
                                 "TCP peer transport did not receive chunk frame.");
        check != 0) {
        return check;
    }
    if (const int check = expect(state.errors.empty(),
                                 "TCP peer transport reported a chunk error.");
        check != 0) {
        return check;
    }
    if (const int check = expect(state.frame.has_value(),
                                 "TCP peer transport did not store chunk frame.");
        check != 0) {
        return check;
    }

    const relaydesk::net::TransferChunkMessage chunk =
        relaydesk::net::parseTransferChunkFrame(state.frame.value());
    if (const int check = expect(chunk.GetTransferId() == "tcp-transfer-1",
                                 "Received TCP chunk transfer ID mismatch.");
        check != 0) {
        return check;
    }

    return expect(state.frame->GetBody() == body,
                  "Received TCP chunk body mismatch.");
}

int sendsAndReceivesMultipleFramesOverOneConnection()
{
    ReceivedFrameState state;
    relaydesk::net::BoostAsioTcpPeerTransport receiver(0);
    receiver.SetFrameCallback(
        [&state](relaydesk::net::PeerFrame frame,
                 std::string remoteAddress,
                 std::uint16_t remotePort) {
            {
                std::lock_guard lock(state.mutex);
                state.frames.push_back(std::move(frame));
                state.remoteAddress = std::move(remoteAddress);
                state.remotePort = remotePort;
            }
            state.condition.notify_all();
        });
    receiver.SetErrorCallback(
        [&state](std::string message) {
            {
                std::lock_guard lock(state.mutex);
                state.errors.push_back(std::move(message));
            }
            state.condition.notify_all();
        });
    receiver.start();

    int nextFrame = 0;
    relaydesk::net::BoostAsioTcpPeerTransport sender(0);
    sender.sendFramesTo(
        "127.0.0.1",
        receiver.GetLocalPort(),
        [&nextFrame]() -> std::optional<relaydesk::net::PeerFrame> {
            ++nextFrame;
            if (nextFrame == 1) {
                return relaydesk::net::makeChatMessageFrame(makeChatRecord());
            }
            if (nextFrame == 2) {
                const std::vector<std::uint8_t> body{0x05, 0x06};
                return relaydesk::net::makeTransferChunkFrame(
                    makeTransferChunk(),
                    body);
            }
            if (nextFrame == 3) {
                return relaydesk::net::makeTransferCompleteFrame(
                    makeTransferComplete(2));
            }
            return std::nullopt;
        });

    const bool received = waitForFrameCountOrError(state, 3);
    receiver.stop();

    if (const int check = expect(received,
                                 "TCP peer transport did not receive both frames.");
        check != 0) {
        return check;
    }
    if (const int check = expect(state.errors.empty(),
                                 "TCP peer transport reported a multi-frame error.");
        check != 0) {
        return check;
    }
    if (const int check = expect(state.frames.size() == 3,
                                 "TCP peer transport multi-frame count mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(state.frames[0].GetType()
                                     == relaydesk::net::PeerFrameType::ChatMessage,
                                 "First multi-frame type mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(state.frames[1].GetType()
                                     == relaydesk::net::PeerFrameType::TransferChunk,
                                 "Second multi-frame type mismatch.");
        check != 0) {
        return check;
    }
    if (const int check =
            expect(state.frames[2].GetType()
                       == relaydesk::net::PeerFrameType::TransferComplete,
                   "Third multi-frame type mismatch.");
        check != 0) {
        return check;
    }

    return expect(!state.remoteAddress.empty() && state.remotePort > 0,
                  "TCP peer transport multi-frame endpoint was missing.");
}

int reportsInterruptedTransferStreamWhenPeerDisconnectsMidFrame()
{
    ReceivedFrameState state;
    relaydesk::net::BoostAsioTcpPeerTransport receiver(0);
    receiver.SetFrameCallback(
        [&state](relaydesk::net::PeerFrame frame,
                 std::string remoteAddress,
                 std::uint16_t remotePort) {
            {
                std::lock_guard lock(state.mutex);
                state.frames.push_back(std::move(frame));
                state.remoteAddress = std::move(remoteAddress);
                state.remotePort = remotePort;
            }
            state.condition.notify_all();
        });
    receiver.SetErrorCallback(
        [&state](std::string message) {
            {
                std::lock_guard lock(state.mutex);
                state.errors.push_back(std::move(message));
            }
            state.condition.notify_all();
        });
    receiver.start();

    boost::asio::io_context ioContext;
    tcp::socket socket(ioContext);
    socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                 receiver.GetLocalPort()));
    writeRawFrame(socket,
                  relaydesk::net::makeTransferOfferFrame(makeTransferOffer()));
    const std::vector<std::uint8_t> body{0x01, 0x02, 0x03, 0x04};
    const relaydesk::net::PeerFrame chunkFrame =
        relaydesk::net::makeTransferChunkFrame(makeTransferChunk(), body);
    writePartialRawFrameBody(socket, chunkFrame, 2);
    socket.close();

    const bool interrupted = waitForErrorCount(state, 1);
    receiver.stop();

    if (const int check =
            expect(interrupted,
                   "TCP peer transport did not report an interrupted stream.");
        check != 0) {
        return check;
    }
    if (const int check =
            expect(state.errors[0]
                       == "Peer TCP transfer stream closed before completion.",
                   "TCP peer transport reported the wrong interrupted stream error.");
        check != 0) {
        return check;
    }

    return expect(state.frames.size() == 1
                      && state.frames[0].GetType()
                          == relaydesk::net::PeerFrameType::TransferOffer,
                  "TCP peer transport should only deliver the complete offer frame.");
}

} // namespace

int main()
{
    try {
        if (const int result = sendsAndReceivesChatMessageFrame(); result != 0) {
            return result;
        }
        if (const int result = sendsAndReceivesTransferChunkFrame(); result != 0) {
            return result;
        }
        if (const int result = sendsAndReceivesMultipleFramesOverOneConnection();
            result != 0) {
            return result;
        }
        return reportsInterruptedTransferStreamWhenPeerDisconnectsMidFrame();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
