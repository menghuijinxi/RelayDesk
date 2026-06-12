#include "net/boost_asio_udp_discovery_transport.h"

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

namespace relaydesk::net {
namespace {

using boost::asio::ip::udp;

constexpr std::size_t kMaxUdpPayloadSize = 65507;
constexpr std::uint16_t kMinUdpPort = 1;
constexpr auto kReceivePollInterval = std::chrono::milliseconds(5);

void throwNetworkError(const char* action, const boost::system::error_code& error)
{
    throw std::runtime_error(std::string(action) + ": " + error.message());
}

udp::endpoint makeUdpEndpoint(const std::string& address, std::uint16_t port)
{
    if (port < kMinUdpPort) {
        throw std::invalid_argument("Discovery UDP port cannot be zero.");
    }

    boost::system::error_code error;
    const auto parsedAddress = boost::asio::ip::make_address_v4(address, error);
    if (error) {
        throw std::invalid_argument("Discovery UDP address is invalid.");
    }

    return udp::endpoint(parsedAddress, port);
}

} // namespace

UdpDiscoveryPacket::UdpDiscoveryPacket(std::string payload,
                                       std::string observedAddress,
                                       std::uint16_t observedPort)
    : payload_(std::move(payload)),
      observedAddress_(std::move(observedAddress)),
      observedPort_(observedPort)
{
}

class BoostAsioUdpDiscoveryTransport::Impl {
public:
    explicit Impl(std::uint16_t listenPort)
        : socket_(ioContext_)
    {
        boost::system::error_code error;
        socket_.open(udp::v4(), error);
        if (error) {
            throwNetworkError("Failed to open discovery UDP socket", error);
        }

        socket_.set_option(boost::asio::socket_base::reuse_address(true), error);
        if (error) {
            throwNetworkError("Failed to enable discovery UDP address reuse", error);
        }

        socket_.set_option(boost::asio::socket_base::broadcast(true), error);
        if (error) {
            throwNetworkError("Failed to enable discovery UDP broadcast", error);
        }

        socket_.bind(udp::endpoint(udp::v4(), listenPort), error);
        if (error) {
            throwNetworkError("Failed to bind discovery UDP socket", error);
        }

        socket_.non_blocking(true, error);
        if (error) {
            throwNetworkError("Failed to configure discovery UDP socket", error);
        }
    }

    std::uint16_t GetLocalPort() const
    {
        boost::system::error_code error;
        const auto endpoint = socket_.local_endpoint(error);
        if (error) {
            throwNetworkError("Failed to read discovery UDP local endpoint", error);
        }
        return endpoint.port();
    }

    void sendTo(const std::string& payload,
                const std::string& address,
                std::uint16_t port)
    {
        if (payload.empty()) {
            throw std::invalid_argument("Discovery UDP payload cannot be empty.");
        }

        boost::system::error_code error;
        const std::size_t sentBytes = socket_.send_to(
            boost::asio::buffer(payload),
            makeUdpEndpoint(address, port),
            0,
            error);
        if (error) {
            throwNetworkError("Failed to send discovery UDP payload", error);
        }

        if (sentBytes != payload.size()) {
            throw std::runtime_error("Discovery UDP payload was not fully sent.");
        }
    }

    std::optional<UdpDiscoveryPacket> tryReceiveFor(
        std::chrono::milliseconds timeout)
    {
        if (timeout.count() < 0) {
            throw std::invalid_argument("Discovery UDP receive timeout is invalid.");
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::array<char, kMaxUdpPayloadSize> buffer{};

        for (;;) {
            udp::endpoint remoteEndpoint;
            boost::system::error_code error;
            const std::size_t receivedBytes = socket_.receive_from(
                boost::asio::buffer(buffer),
                remoteEndpoint,
                0,
                error);
            if (!error) {
                return makePacket(buffer, receivedBytes, remoteEndpoint);
            }

            if (error != boost::asio::error::would_block
                && error != boost::asio::error::try_again) {
                throwNetworkError("Failed to receive discovery UDP payload", error);
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }

            std::this_thread::sleep_for(kReceivePollInterval);
        }
    }

    void close()
    {
        boost::system::error_code ignoredError;
        socket_.close(ignoredError);
    }

protected:
    UdpDiscoveryPacket makePacket(const std::array<char, kMaxUdpPayloadSize>& buffer,
                                  std::size_t receivedBytes,
                                  const udp::endpoint& remoteEndpoint)
    {
        return UdpDiscoveryPacket(std::string(buffer.data(), receivedBytes),
                                  remoteEndpoint.address().to_string(),
                                  remoteEndpoint.port());
    }

    boost::asio::io_context ioContext_;
    udp::socket socket_;
};

BoostAsioUdpDiscoveryTransport::BoostAsioUdpDiscoveryTransport(
    std::uint16_t listenPort)
    : impl_(std::make_unique<Impl>(listenPort))
{
}

BoostAsioUdpDiscoveryTransport::~BoostAsioUdpDiscoveryTransport() = default;

BoostAsioUdpDiscoveryTransport::BoostAsioUdpDiscoveryTransport(
    BoostAsioUdpDiscoveryTransport&&) noexcept = default;

BoostAsioUdpDiscoveryTransport& BoostAsioUdpDiscoveryTransport::operator=(
    BoostAsioUdpDiscoveryTransport&&) noexcept = default;

std::uint16_t BoostAsioUdpDiscoveryTransport::GetLocalPort() const
{
    return impl_->GetLocalPort();
}

void BoostAsioUdpDiscoveryTransport::sendTo(const std::string& payload,
                                            const std::string& address,
                                            std::uint16_t port)
{
    impl_->sendTo(payload, address, port);
}

void BoostAsioUdpDiscoveryTransport::sendBroadcast(const std::string& payload,
                                                   std::uint16_t port)
{
    impl_->sendTo(payload, "255.255.255.255", port);
}

std::optional<UdpDiscoveryPacket> BoostAsioUdpDiscoveryTransport::tryReceiveFor(
    std::chrono::milliseconds timeout)
{
    return impl_->tryReceiveFor(timeout);
}

void BoostAsioUdpDiscoveryTransport::close()
{
    impl_->close();
}

}
