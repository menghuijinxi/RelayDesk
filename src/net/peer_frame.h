#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace relaydesk::net {

constexpr std::uint16_t kPeerProtocolVersion = 1;
constexpr std::size_t kPeerFrameHeaderSize = 24;

enum class PeerFrameType : std::uint16_t {
    ProfileHello = 1,
    ProfileUpdate = 2,
    ChatMessage = 3,
    ChatMessageUpdate = 4,
    DeliveryReceipt = 5,
    TransferOffer = 6,
    TransferAccept = 7,
    TransferReject = 8,
    TransferChunk = 9,
    TransferComplete = 10,
    TransferCancel = 11,
    Heartbeat = 12,
    AppUpdateRequest = 13,
    AppUpdateChunk = 14,
    AppUpdateComplete = 15,
};

class PeerFrame {
public:
    PeerFrame() = default;
    PeerFrame(PeerFrameType type,
              std::string header,
              std::vector<std::uint8_t> body = {});

    std::uint16_t GetVersion() const { return version_; }
    PeerFrameType GetType() const { return type_; }
    std::uint32_t GetFlags() const { return flags_; }
    const std::string& GetHeader() const { return header_; }
    const std::vector<std::uint8_t>& GetBody() const { return body_; }

    void SetVersion(std::uint16_t version) { version_ = version; }
    void SetType(PeerFrameType type) { type_ = type; }
    void SetFlags(std::uint32_t flags) { flags_ = flags; }
    void SetHeader(std::string header) { header_ = std::move(header); }
    void SetBody(std::vector<std::uint8_t> body) { body_ = std::move(body); }

protected:
    std::uint16_t version_ = kPeerProtocolVersion;
    PeerFrameType type_ = PeerFrameType::Heartbeat;
    std::uint32_t flags_ = 0;
    std::string header_;
    std::vector<std::uint8_t> body_;
};

class PeerFrameHeader {
public:
    PeerFrameHeader() = default;
    PeerFrameHeader(std::uint16_t version,
                    PeerFrameType type,
                    std::uint32_t flags,
                    std::uint32_t headerLength,
                    std::uint64_t bodyLength);

    std::uint16_t GetVersion() const { return version_; }
    PeerFrameType GetType() const { return type_; }
    std::uint32_t GetFlags() const { return flags_; }
    std::uint32_t GetHeaderLength() const { return headerLength_; }
    std::uint64_t GetBodyLength() const { return bodyLength_; }

protected:
    std::uint16_t version_ = kPeerProtocolVersion;
    PeerFrameType type_ = PeerFrameType::Heartbeat;
    std::uint32_t flags_ = 0;
    std::uint32_t headerLength_ = 0;
    std::uint64_t bodyLength_ = 0;
};

std::uint16_t toWireValue(PeerFrameType type);
PeerFrameType peerFrameTypeFromWireValue(std::uint16_t value);
std::vector<std::uint8_t> encodePeerFrame(const PeerFrame& frame);
PeerFrameHeader decodePeerFrameHeader(std::span<const std::uint8_t> bytes);
std::size_t peerFramePayloadSize(const PeerFrameHeader& header);
PeerFrame decodePeerFrame(const PeerFrameHeader& header,
                          std::span<const std::uint8_t> payloadBytes);
PeerFrame decodePeerFrame(std::span<const std::uint8_t> bytes);

}
