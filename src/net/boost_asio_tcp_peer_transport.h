#pragma once

#include "net/peer_frame.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace relaydesk::net {

using TcpPeerFrameCallback =
    std::function<void(PeerFrame, std::string, std::uint16_t)>;
using TcpPeerErrorCallback = std::function<void(std::string)>;

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

protected:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
