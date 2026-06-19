#pragma once

#include "net/peer_frame.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace relaydesk::net {

using TcpPeerFrameCallback =
    std::function<void(PeerFrame, std::string, std::uint16_t)>;
using TcpPeerErrorCallback = std::function<void(std::string)>;
using TcpPeerFrameProducer = std::function<std::optional<PeerFrame>()>;
using TcpPeerFrameSentCallback = std::function<void()>;

class BoostAsioTcpPeerTransport {
public:
    explicit BoostAsioTcpPeerTransport(std::uint16_t listenPort);
    ~BoostAsioTcpPeerTransport();

    BoostAsioTcpPeerTransport(const BoostAsioTcpPeerTransport&) = delete;
    BoostAsioTcpPeerTransport& operator=(const BoostAsioTcpPeerTransport&) = delete;
    BoostAsioTcpPeerTransport(BoostAsioTcpPeerTransport&&) noexcept;
    BoostAsioTcpPeerTransport& operator=(
        BoostAsioTcpPeerTransport&&) noexcept;

    std::uint16_t GetLocalPort() const;

    void SetFrameCallback(TcpPeerFrameCallback frameCallback);
    void SetErrorCallback(TcpPeerErrorCallback errorCallback);
    void start();
    void stop();
    void sendFrameTo(const std::string& address,
                     std::uint16_t port,
                     const PeerFrame& frame);
    void sendFramesTo(const std::string& address,
                      std::uint16_t port,
                      const TcpPeerFrameProducer& frameProducer,
                      const TcpPeerFrameSentCallback& frameSentCallback = {});

protected:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
