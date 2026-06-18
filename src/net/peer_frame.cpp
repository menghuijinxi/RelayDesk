#include "net/peer_frame.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace relaydesk::net {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'R', 'L', 'D', 'K'};
constexpr std::uint32_t kMaxHeaderLength = 1024u * 1024u;
constexpr std::uint64_t kMaxBodyLength = 1024ull * 1024ull * 1024ull;

void appendUint16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void appendUint32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void appendUint64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

std::uint16_t readUint16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8)
        | static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t readUint32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t readUint64(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

void validateFrameForEncoding(const PeerFrame& frame)
{
    if (frame.GetVersion() != kPeerProtocolVersion) {
        throw std::runtime_error("Peer frame has unsupported protocol version.");
    }

    (void)toWireValue(frame.GetType());
    if (frame.GetHeader().size() > kMaxHeaderLength) {
        throw std::runtime_error("Peer frame header is too large.");
    }

    if (frame.GetBody().size() > kMaxBodyLength) {
        throw std::runtime_error("Peer frame body is too large.");
    }
}

std::size_t checkedPayloadSize(std::uint32_t headerLength,
                               std::uint64_t bodyLength)
{
    if (bodyLength > kMaxBodyLength) {
        throw std::runtime_error("Peer frame body is too large.");
    }

    if (bodyLength > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("Peer frame body length is not supported.");
    }

    const auto bodySize = static_cast<std::size_t>(bodyLength);
    if (headerLength > std::numeric_limits<std::size_t>::max() - bodySize) {
        throw std::runtime_error("Peer frame payload length overflow.");
    }

    return static_cast<std::size_t>(headerLength) + bodySize;
}

} // namespace

PeerFrame::PeerFrame(PeerFrameType type,
                     std::string header,
                     std::vector<std::uint8_t> body)
    : type_(type),
      header_(std::move(header)),
      body_(std::move(body))
{
}

PeerFrameHeader::PeerFrameHeader(std::uint16_t version,
                                 PeerFrameType type,
                                 std::uint32_t flags,
                                 std::uint32_t headerLength,
                                 std::uint64_t bodyLength)
    : version_(version),
      type_(type),
      flags_(flags),
      headerLength_(headerLength),
      bodyLength_(bodyLength)
{
}

std::uint16_t toWireValue(PeerFrameType type)
{
    switch (type) {
    case PeerFrameType::ProfileHello:
        return 1;
    case PeerFrameType::ProfileUpdate:
        return 2;
    case PeerFrameType::ChatMessage:
        return 3;
    case PeerFrameType::ChatMessageUpdate:
        return 4;
    case PeerFrameType::DeliveryReceipt:
        return 5;
    case PeerFrameType::TransferOffer:
        return 6;
    case PeerFrameType::TransferAccept:
        return 7;
    case PeerFrameType::TransferReject:
        return 8;
    case PeerFrameType::TransferChunk:
        return 9;
    case PeerFrameType::TransferComplete:
        return 10;
    case PeerFrameType::TransferCancel:
        return 11;
    case PeerFrameType::Heartbeat:
        return 12;
    case PeerFrameType::AppUpdateRequest:
        return 13;
    case PeerFrameType::AppUpdateChunk:
        return 14;
    case PeerFrameType::AppUpdateComplete:
        return 15;
    }

    throw std::runtime_error("Unsupported peer frame type.");
}

PeerFrameType peerFrameTypeFromWireValue(std::uint16_t value)
{
    switch (value) {
    case 1:
        return PeerFrameType::ProfileHello;
    case 2:
        return PeerFrameType::ProfileUpdate;
    case 3:
        return PeerFrameType::ChatMessage;
    case 4:
        return PeerFrameType::ChatMessageUpdate;
    case 5:
        return PeerFrameType::DeliveryReceipt;
    case 6:
        return PeerFrameType::TransferOffer;
    case 7:
        return PeerFrameType::TransferAccept;
    case 8:
        return PeerFrameType::TransferReject;
    case 9:
        return PeerFrameType::TransferChunk;
    case 10:
        return PeerFrameType::TransferComplete;
    case 11:
        return PeerFrameType::TransferCancel;
    case 12:
        return PeerFrameType::Heartbeat;
    case 13:
        return PeerFrameType::AppUpdateRequest;
    case 14:
        return PeerFrameType::AppUpdateChunk;
    case 15:
        return PeerFrameType::AppUpdateComplete;
    default:
        throw std::runtime_error("Unsupported peer frame type value.");
    }
}

std::vector<std::uint8_t> encodePeerFrame(const PeerFrame& frame)
{
    validateFrameForEncoding(frame);

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPeerFrameHeaderSize
                  + frame.GetHeader().size()
                  + frame.GetBody().size());
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    appendUint16(bytes, frame.GetVersion());
    appendUint16(bytes, toWireValue(frame.GetType()));
    appendUint32(bytes, frame.GetFlags());
    appendUint32(bytes, static_cast<std::uint32_t>(frame.GetHeader().size()));
    appendUint64(bytes, static_cast<std::uint64_t>(frame.GetBody().size()));
    bytes.insert(bytes.end(), frame.GetHeader().begin(), frame.GetHeader().end());
    bytes.insert(bytes.end(), frame.GetBody().begin(), frame.GetBody().end());
    return bytes;
}

PeerFrameHeader decodePeerFrameHeader(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < kPeerFrameHeaderSize) {
        throw std::runtime_error("Peer frame is shorter than the frame header.");
    }

    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        throw std::runtime_error("Peer frame magic is invalid.");
    }

    const std::uint16_t version = readUint16(bytes, 4);
    if (version != kPeerProtocolVersion) {
        throw std::runtime_error("Peer frame has unsupported protocol version.");
    }

    const PeerFrameType type = peerFrameTypeFromWireValue(readUint16(bytes, 6));
    const std::uint32_t flags = readUint32(bytes, 8);
    const std::uint32_t headerLength = readUint32(bytes, 12);
    const std::uint64_t bodyLength = readUint64(bytes, 16);
    if (headerLength > kMaxHeaderLength) {
        throw std::runtime_error("Peer frame header is too large.");
    }
    (void)checkedPayloadSize(headerLength, bodyLength);

    return PeerFrameHeader(version, type, flags, headerLength, bodyLength);
}

std::size_t peerFramePayloadSize(const PeerFrameHeader& header)
{
    return checkedPayloadSize(header.GetHeaderLength(), header.GetBodyLength());
}

PeerFrame decodePeerFrame(const PeerFrameHeader& header,
                          std::span<const std::uint8_t> payloadBytes)
{
    const std::size_t payloadSize = peerFramePayloadSize(header);
    if (payloadBytes.size() != payloadSize) {
        throw std::runtime_error("Peer frame payload length does not match header.");
    }

    const auto headerBegin = payloadBytes.begin();
    const auto bodyBegin = headerBegin + header.GetHeaderLength();

    PeerFrame frame;
    frame.SetVersion(header.GetVersion());
    frame.SetType(header.GetType());
    frame.SetFlags(header.GetFlags());
    frame.SetHeader(std::string(headerBegin, bodyBegin));
    frame.SetBody(std::vector<std::uint8_t>(bodyBegin, payloadBytes.end()));
    return frame;
}

PeerFrame decodePeerFrame(std::span<const std::uint8_t> bytes)
{
    const PeerFrameHeader header = decodePeerFrameHeader(bytes);
    const std::size_t payloadSize = peerFramePayloadSize(header);
    if (bytes.size() - kPeerFrameHeaderSize != payloadSize) {
        throw std::runtime_error("Peer frame payload length does not match header.");
    }

    return decodePeerFrame(
        header,
        bytes.subspan(kPeerFrameHeaderSize, payloadSize));
}

}
