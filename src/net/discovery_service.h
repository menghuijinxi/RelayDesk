#pragma once

#include "net/boost_asio_udp_discovery_transport.h"
#include "storage/app_paths.h"
#include "storage/local_identity.h"
#include "storage/peer_profile.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace relaydesk::net {

constexpr std::uint16_t kDefaultDiscoveryUdpPort = 25581;
constexpr std::uint16_t kDefaultAdvertisedTcpPort = 39171;

class DiscoveryServiceConfig {
public:
    std::uint16_t GetDiscoveryUdpPort() const { return discoveryUdpPort_; }
    std::uint16_t GetAdvertisedTcpPort() const { return advertisedTcpPort_; }
    const std::vector<std::string>& GetCapabilities() const { return capabilities_; }

    void SetDiscoveryUdpPort(std::uint16_t discoveryUdpPort)
    {
        discoveryUdpPort_ = discoveryUdpPort;
    }
    void SetAdvertisedTcpPort(std::uint16_t advertisedTcpPort)
    {
        advertisedTcpPort_ = advertisedTcpPort;
    }
    void SetCapabilities(std::vector<std::string> capabilities)
    {
        capabilities_ = std::move(capabilities);
    }

protected:
    std::uint16_t discoveryUdpPort_ = kDefaultDiscoveryUdpPort;
    std::uint16_t advertisedTcpPort_ = kDefaultAdvertisedTcpPort;
    std::vector<std::string> capabilities_{"text", "file"};
};

enum class DiscoveryServicePollAction {
    NoPacket,
    InvalidPacket,
    IgnoredSelf,
    StoredPeer,
};

class DiscoveryServicePollResult {
public:
    DiscoveryServicePollAction GetAction() const { return action_; }
    const std::string& GetErrorMessage() const { return errorMessage_; }
    const std::string& GetObservedAddress() const { return observedAddress_; }
    const std::string& GetAnnouncementType() const { return announcementType_; }
    std::uint16_t GetObservedPort() const { return observedPort_; }
    bool HasPeerProfile() const { return peerProfile_.has_value(); }
    bool GetPeerCreated() const { return peerCreated_; }
    const std::optional<relaydesk::storage::PeerProfile>& GetPeerProfile() const
    {
        return peerProfile_;
    }

    static DiscoveryServicePollResult NoPacket();
    static DiscoveryServicePollResult InvalidPacket(std::string errorMessage);
    static DiscoveryServicePollResult IgnoredSelf();
    static DiscoveryServicePollResult StoredPeer(
        relaydesk::storage::PeerProfile peerProfile,
        bool peerCreated,
        std::string observedAddress,
        std::string announcementType,
        std::uint16_t observedPort);

protected:
    DiscoveryServicePollAction action_ = DiscoveryServicePollAction::NoPacket;
    std::string errorMessage_;
    std::string observedAddress_;
    std::string announcementType_;
    std::uint16_t observedPort_ = 0;
    std::optional<relaydesk::storage::PeerProfile> peerProfile_;
    bool peerCreated_ = false;
};

class DiscoveryService {
public:
    DiscoveryService(relaydesk::storage::AppPaths appPaths,
                     relaydesk::storage::LocalIdentity localIdentity,
                     DiscoveryServiceConfig config);

    std::uint16_t GetLocalUdpPort() const;

    void broadcastNow();
    void broadcastOfflineNow();
    void sendAnnouncementTo(const std::string& address, std::uint16_t port);
    void sendReplyTo(const std::string& address, std::uint16_t port);
    void sendOfflineTo(const std::string& address, std::uint16_t port);
    DiscoveryServicePollResult pollOnce(std::chrono::milliseconds timeout);
    void logDiagnostic(std::string message) const;
    void close();

protected:
    std::string makeAnnouncementPayload(std::string announcementType) const;

    relaydesk::storage::AppPaths appPaths_;
    relaydesk::storage::LocalIdentity localIdentity_;
    DiscoveryServiceConfig config_;
    BoostAsioUdpDiscoveryTransport transport_;
};

}
