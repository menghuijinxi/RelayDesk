#include "net/boost_asio_tcp_peer_transport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

namespace relaydesk::net {
namespace {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

constexpr std::uint16_t kMinTcpPort = 1;
constexpr auto kAcceptPollInterval = 5ms;
constexpr int kPeerTcpBufferSize = 4 * 1024 * 1024;

void throwNetworkError(const char* action, const boost::system::error_code& error)
{
    throw std::runtime_error(std::string(action) + ": " + error.message());
}

tcp::endpoint makeTcpEndpoint(const std::string& address, std::uint16_t port)
{
    if (port < kMinTcpPort) {
        throw std::invalid_argument("Peer TCP port cannot be zero.");
    }

    boost::system::error_code error;
    const auto parsedAddress = boost::asio::ip::make_address(address, error);
    if (error) {
        throw std::invalid_argument("Peer TCP address is invalid.");
    }

    return tcp::endpoint(parsedAddress, port);
}

void readExact(tcp::socket& socket, boost::asio::mutable_buffer buffer)
{
    boost::system::error_code error;
    boost::asio::read(socket, buffer, error);
    if (error) {
        throwNetworkError("Failed to read peer TCP frame", error);
    }
}

void writeFrame(tcp::socket& socket, const PeerFrame& frame)
{
    const std::array<std::uint8_t, kPeerFrameHeaderSize> header =
        encodePeerFrameHeader(frame);
    const std::array<boost::asio::const_buffer, 3> buffers{{
        boost::asio::buffer(header),
        boost::asio::buffer(frame.GetHeader()),
        boost::asio::buffer(frame.GetBody()),
    }};

    boost::system::error_code error;
    boost::asio::write(socket, buffers, error);
    if (error) {
        throwNetworkError("Failed to write peer TCP frame", error);
    }
}

void configureSocketForFrameTransfer(tcp::socket& socket)
{
    boost::system::error_code ignoredError;
    socket.set_option(tcp::no_delay(true), ignoredError);
    socket.set_option(
        boost::asio::socket_base::send_buffer_size(kPeerTcpBufferSize),
        ignoredError);
    socket.set_option(
        boost::asio::socket_base::receive_buffer_size(kPeerTcpBufferSize),
        ignoredError);
}

bool isExpectedAcceptStopError(const boost::system::error_code& error)
{
    return error == boost::asio::error::operation_aborted
        || error == boost::asio::error::bad_descriptor;
}

} // namespace

class BoostAsioTcpPeerTransport::Impl {
public:
    explicit Impl(std::uint16_t listenPort)
        : acceptor_(ioContext_)
    {
        boost::system::error_code error;
        acceptor_.open(tcp::v4(), error);
        if (error) {
            throwNetworkError("Failed to open peer TCP acceptor", error);
        }

        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), error);
        if (error) {
            throwNetworkError("Failed to enable peer TCP address reuse", error);
        }

        acceptor_.bind(tcp::endpoint(tcp::v4(), listenPort), error);
        if (error) {
            throwNetworkError("Failed to bind peer TCP acceptor", error);
        }

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
        if (error) {
            throwNetworkError("Failed to listen on peer TCP acceptor", error);
        }

        acceptor_.non_blocking(true, error);
        if (error) {
            throwNetworkError("Failed to configure peer TCP acceptor", error);
        }
    }

    ~Impl()
    {
        stop();
    }

    std::uint16_t GetLocalPort() const
    {
        boost::system::error_code error;
        const auto endpoint = acceptor_.local_endpoint(error);
        if (error) {
            throwNetworkError("Failed to read peer TCP local endpoint", error);
        }
        return endpoint.port();
    }

    void SetFrameCallback(TcpPeerFrameCallback frameCallback)
    {
        std::lock_guard lock(callbackMutex_);
        frameCallback_ = std::move(frameCallback);
    }

    void SetErrorCallback(TcpPeerErrorCallback errorCallback)
    {
        std::lock_guard lock(callbackMutex_);
        errorCallback_ = std::move(errorCallback);
    }

    void start()
    {
        if (thread_.joinable()) {
            return;
        }

        stopping_.store(false);
        thread_ = std::jthread([this](std::stop_token stopToken) {
            run(stopToken);
        });
    }

    void stop()
    {
        stopping_.store(true);
        boost::system::error_code ignoredError;
        acceptor_.close(ignoredError);
        if (!thread_.joinable()) {
            return;
        }

        thread_.request_stop();
        thread_.join();
    }

    void sendFrameTo(const std::string& address,
                     std::uint16_t port,
                     const PeerFrame& frame)
    {
        bool sent = false;
        sendFramesTo(address, port, [&frame, sent]() mutable
            -> std::optional<PeerFrame> {
            if (sent) {
                return std::nullopt;
            }
            sent = true;
            return frame;
        });
    }

    void sendFramesTo(
        const std::string& address,
        std::uint16_t port,
        const TcpPeerFrameProducer& frameProducer,
        const TcpPeerFrameSentCallback& frameSentCallback = {})
    {
        boost::asio::io_context clientIoContext;
        tcp::socket socket(clientIoContext);
        boost::system::error_code error;
        socket.connect(makeTcpEndpoint(address, port), error);
        if (error) {
            throwNetworkError("Failed to connect peer TCP socket", error);
        }
        configureSocketForFrameTransfer(socket);

        while (true) {
            std::optional<PeerFrame> frame = frameProducer();
            if (!frame.has_value()) {
                break;
            }
            writeFrame(socket, frame.value());
            if (frameSentCallback) {
                frameSentCallback();
            }
        }
        socket.shutdown(tcp::socket::shutdown_both, error);
        socket.close(error);
    }

protected:
    void run(std::stop_token stopToken)
    {
        while (!stopToken.stop_requested() && !stopping_.load()) {
            try {
                tcp::socket socket(ioContext_);
                boost::system::error_code error;
                acceptor_.accept(socket, error);
                if (error == boost::asio::error::would_block
                    || error == boost::asio::error::try_again) {
                    std::this_thread::sleep_for(kAcceptPollInterval);
                    continue;
                }
                if (error) {
                    if (stopping_.load() && isExpectedAcceptStopError(error)) {
                        return;
                    }
                    throwNetworkError("Failed to accept peer TCP socket", error);
                }

                configureSocketForFrameTransfer(socket);
                receiveFrames(std::move(socket));
            } catch (const std::exception& error) {
                if (!stopping_.load()) {
                    notifyError(error.what());
                }
            }
        }
    }

    void receiveFrames(tcp::socket socket)
    {
        boost::system::error_code error;
        const auto endpoint = socket.remote_endpoint(error);
        const std::string remoteAddress =
            error ? std::string{} : endpoint.address().to_string();
        const std::uint16_t remotePort = error ? 0 : endpoint.port();

        while (!stopping_.load()) {
            std::array<std::uint8_t, kPeerFrameHeaderSize> headerBytes{};
            boost::asio::read(socket, boost::asio::buffer(headerBytes), error);
            if (error == boost::asio::error::eof
                || error == boost::asio::error::connection_reset) {
                return;
            }
            if (error) {
                throwNetworkError("Failed to read peer TCP frame", error);
            }

            const PeerFrameHeader header = decodePeerFrameHeader(headerBytes);
            std::vector<std::uint8_t> payload(peerFramePayloadSize(header));
            if (!payload.empty()) {
                readExact(socket, boost::asio::buffer(payload));
            }

            notifyFrame(decodePeerFrame(header, payload), remoteAddress, remotePort);
        }
    }

    void notifyFrame(PeerFrame frame,
                     std::string address,
                     std::uint16_t port)
    {
        TcpPeerFrameCallback callback;
        {
            std::lock_guard lock(callbackMutex_);
            callback = frameCallback_;
        }
        if (!callback) {
            return;
        }

        callback(std::move(frame), std::move(address), port);
    }

    void notifyError(std::string message)
    {
        TcpPeerErrorCallback callback;
        {
            std::lock_guard lock(callbackMutex_);
            callback = errorCallback_;
        }
        if (!callback) {
            return;
        }

        callback(std::move(message));
    }

    boost::asio::io_context ioContext_;
    tcp::acceptor acceptor_;
    std::mutex callbackMutex_;
    TcpPeerFrameCallback frameCallback_;
    TcpPeerErrorCallback errorCallback_;
    std::jthread thread_;
    std::atomic_bool stopping_ = false;
};

BoostAsioTcpPeerTransport::BoostAsioTcpPeerTransport(std::uint16_t listenPort)
    : impl_(std::make_unique<Impl>(listenPort))
{
}

BoostAsioTcpPeerTransport::~BoostAsioTcpPeerTransport() = default;

BoostAsioTcpPeerTransport::BoostAsioTcpPeerTransport(
    BoostAsioTcpPeerTransport&&) noexcept = default;

BoostAsioTcpPeerTransport& BoostAsioTcpPeerTransport::operator=(
    BoostAsioTcpPeerTransport&&) noexcept = default;

std::uint16_t BoostAsioTcpPeerTransport::GetLocalPort() const
{
    return impl_->GetLocalPort();
}

void BoostAsioTcpPeerTransport::SetFrameCallback(
    TcpPeerFrameCallback frameCallback)
{
    impl_->SetFrameCallback(std::move(frameCallback));
}

void BoostAsioTcpPeerTransport::SetErrorCallback(
    TcpPeerErrorCallback errorCallback)
{
    impl_->SetErrorCallback(std::move(errorCallback));
}

void BoostAsioTcpPeerTransport::start()
{
    impl_->start();
}

void BoostAsioTcpPeerTransport::stop()
{
    impl_->stop();
}

void BoostAsioTcpPeerTransport::sendFrameTo(const std::string& address,
                                            std::uint16_t port,
                                            const PeerFrame& frame)
{
    impl_->sendFrameTo(address, port, frame);
}

void BoostAsioTcpPeerTransport::sendFramesTo(
    const std::string& address,
    std::uint16_t port,
    const TcpPeerFrameProducer& frameProducer,
    const TcpPeerFrameSentCallback& frameSentCallback)
{
    impl_->sendFramesTo(address, port, frameProducer, frameSentCallback);
}

}
