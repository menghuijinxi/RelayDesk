#include "net/boost_asio_udp_discovery_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#if defined(_WIN32)
#include <iphlpapi.h>
#include <ws2tcpip.h>
#endif

namespace relaydesk::net {
namespace {

using boost::asio::ip::udp;

constexpr std::size_t kMaxUdpPayloadSize = 65507;
constexpr std::uint16_t kMinUdpPort = 1;
constexpr auto kReceivePollInterval = std::chrono::milliseconds(5);
constexpr const char* kLimitedBroadcastAddress = "255.255.255.255";

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

#if defined(_WIN32)
struct LocalIpv4Address {
    std::uint32_t hostOrderAddress = 0;
    std::uint8_t prefixLength = 0;
    ULONG routeMetric = 0;
    std::string text;
};

std::optional<LocalIpv4Address> makeLocalIpv4Address(
    const IP_ADAPTER_ADDRESSES& adapter,
    const IP_ADAPTER_UNICAST_ADDRESS& unicastAddress)
{
    if (unicastAddress.Address.lpSockaddr == nullptr
        || unicastAddress.Address.lpSockaddr->sa_family != AF_INET) {
        return std::nullopt;
    }

    const auto* socketAddress =
        reinterpret_cast<const SOCKADDR_IN*>(unicastAddress.Address.lpSockaddr);
    const std::uint32_t hostOrderAddress =
        ntohl(socketAddress->sin_addr.S_un.S_addr);

    char addressText[INET_ADDRSTRLEN]{};
    if (InetNtopA(AF_INET,
                  &socketAddress->sin_addr,
                  addressText,
                  static_cast<DWORD>(std::size(addressText))) == nullptr) {
        return std::nullopt;
    }
    return LocalIpv4Address{
        hostOrderAddress,
        unicastAddress.OnLinkPrefixLength,
        adapter.Ipv4Metric,
        addressText,
    };
}

std::vector<LocalIpv4Address> findActiveLocalIpv4Addresses()
{
    ULONG bufferSize = 15 * 1024;
    std::vector<unsigned char> buffer(bufferSize);
    constexpr ULONG kAdapterFlags =
        GAA_FLAG_SKIP_ANYCAST
        | GAA_FLAG_SKIP_MULTICAST
        | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(
        AF_INET,
        kAdapterFlags,
        nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
        &bufferSize);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferSize);
        result = GetAdaptersAddresses(
            AF_INET,
            kAdapterFlags,
            nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
            &bufferSize);
    }
    if (result != NO_ERROR) {
        return {};
    }

    std::vector<LocalIpv4Address> addresses;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp
            || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        for (auto* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr;
             unicast = unicast->Next) {
            const auto address = makeLocalIpv4Address(*adapter, *unicast);
            if (address.has_value()
                && std::none_of(
                    addresses.begin(),
                    addresses.end(),
                    [&address](const LocalIpv4Address& existing) {
                        return existing.text == address->text;
                    })) {
                addresses.push_back(address.value());
            }
        }
    }

    std::stable_sort(
        addresses.begin(),
        addresses.end(),
        [](const LocalIpv4Address& left, const LocalIpv4Address& right) {
            return left.routeMetric < right.routeMetric;
        });

    return addresses;
}

std::optional<std::string> makeDirectedBroadcastAddress(
    const LocalIpv4Address& localAddress)
{
    if (localAddress.prefixLength >= 32) {
        return std::nullopt;
    }
    const std::uint32_t networkMask = localAddress.prefixLength == 0
        ? 0
        : 0xFFFFFFFFu << (32 - localAddress.prefixLength);
    IN_ADDR broadcastAddress{};
    broadcastAddress.S_un.S_addr =
        htonl(localAddress.hostOrderAddress | ~networkMask);

    char addressText[INET_ADDRSTRLEN]{};
    if (InetNtopA(AF_INET,
                  &broadcastAddress,
                  addressText,
                  static_cast<DWORD>(std::size(addressText))) == nullptr) {
        return std::nullopt;
    }
    return std::string(addressText);
}

std::vector<std::string> findDirectedBroadcastAddresses()
{
    std::vector<std::string> addresses;
    for (const auto& localAddress : findActiveLocalIpv4Addresses()) {
        const auto broadcastAddress = makeDirectedBroadcastAddress(localAddress);
        if (broadcastAddress.has_value()
            && std::find(addresses.begin(),
                         addresses.end(),
                         broadcastAddress.value()) == addresses.end()) {
            addresses.push_back(broadcastAddress.value());
        }
    }
    return addresses;
}
#endif

std::vector<std::string> makeBroadcastAddresses()
{
    std::vector<std::string> addresses{kLimitedBroadcastAddress};
#if defined(_WIN32)
    for (const auto& address : findDirectedBroadcastAddresses()) {
        if (std::find(addresses.begin(), addresses.end(), address)
            == addresses.end()) {
            addresses.push_back(address);
        }
    }
#endif
    return addresses;
}

std::string joinAddresses(const std::vector<std::string>& addresses)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        output << addresses[index];
    }
    return output.str();
}

} // namespace

std::optional<std::string> findPreferredLocalIpv4Address()
{
#if defined(_WIN32)
    const std::vector<LocalIpv4Address> addresses =
        findActiveLocalIpv4Addresses();
    if (!addresses.empty()) {
        return addresses.front().text;
    }
#endif
    return std::nullopt;
}

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

        log("udp.send.begin address=" + address
            + " port=" + std::to_string(port)
            + " bytes=" + std::to_string(payload.size()));
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
        log("udp.send.success address=" + address
            + " port=" + std::to_string(port)
            + " bytes=" + std::to_string(sentBytes));
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
                log("udp.receive from="
                    + remoteEndpoint.address().to_string()
                    + ":" + std::to_string(remoteEndpoint.port())
                    + " bytes=" + std::to_string(receivedBytes));
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

    void SetLogCallback(UdpDiscoveryLogCallback logCallback)
    {
        logCallback_ = std::move(logCallback);
    }

    void log(std::string message)
    {
        if (!logCallback_) {
            return;
        }

        try {
            logCallback_(std::move(message));
        } catch (...) {
        }
    }

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
    UdpDiscoveryLogCallback logCallback_;
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
    std::exception_ptr firstError;
    bool sent = false;
    const auto addresses = makeBroadcastAddresses();
    impl_->log("udp.broadcast.targets port=" + std::to_string(port)
               + " count=" + std::to_string(addresses.size())
               + " addresses=" + joinAddresses(addresses));
    for (const auto& address : addresses) {
        try {
            impl_->sendTo(payload, address, port);
            sent = true;
        } catch (const std::exception& error) {
            impl_->log("udp.broadcast.failed address=" + address
                       + " port=" + std::to_string(port)
                       + " error=" + error.what());
            if (firstError == nullptr) {
                firstError = std::current_exception();
            }
        }
    }

    if (!sent && firstError != nullptr) {
        std::rethrow_exception(firstError);
    }
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

void BoostAsioUdpDiscoveryTransport::SetLogCallback(
    UdpDiscoveryLogCallback logCallback)
{
    impl_->SetLogCallback(std::move(logCallback));
}

}
