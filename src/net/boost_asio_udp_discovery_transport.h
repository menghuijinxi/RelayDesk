#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace relaydesk::net {

class UdpDiscoveryPacket {
public:
    UdpDiscoveryPacket(std::string payload,
                       std::string observedAddress,
                       std::uint16_t observedPort);

    const std::string& GetPayload() const { return payload_; }
    const std::string& GetObservedAddress() const { return observedAddress_; }
    std::uint16_t GetObservedPort() const { return observedPort_; }

protected:
    std::string payload_;
    std::string observedAddress_;
    std::uint16_t observedPort_ = 0;
};

class BoostAsioUdpDiscoveryTransport {
public:
    explicit BoostAsioUdpDiscoveryTransport(std::uint16_t listenPort);
    ~BoostAsioUdpDiscoveryTransport();

    BoostAsioUdpDiscoveryTransport(const BoostAsioUdpDiscoveryTransport&) = delete;
    BoostAsioUdpDiscoveryTransport& operator=(
        const BoostAsioUdpDiscoveryTransport&) = delete;
    BoostAsioUdpDiscoveryTransport(BoostAsioUdpDiscoveryTransport&&) noexcept;
    BoostAsioUdpDiscoveryTransport& operator=(
        BoostAsioUdpDiscoveryTransport&&) noexcept;

    std::uint16_t GetLocalPort() const;

    void sendTo(const std::string& payload,
                const std::string& address,
                std::uint16_t port);
    void sendBroadcast(const std::string& payload, std::uint16_t port);
    std::optional<UdpDiscoveryPacket> tryReceiveFor(std::chrono::milliseconds timeout);
    void close();

protected:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
