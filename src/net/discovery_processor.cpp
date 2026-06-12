#include "net/discovery_processor.h"

#include "net/discovery_peer_profile.h"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace relaydesk::net {

DiscoveryProcessResult DiscoveryProcessResult::IgnoredSelf()
{
    return {};
}

DiscoveryProcessResult DiscoveryProcessResult::StoredPeer(
    relaydesk::storage::PeerProfile peerProfile,
    bool peerCreated)
{
    DiscoveryProcessResult result;
    result.action_ = DiscoveryProcessAction::StoredPeer;
    result.peerProfile_ = std::move(peerProfile);
    result.peerCreated_ = peerCreated;
    return result;
}

DiscoveryProcessResult processDiscoveryAnnouncement(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& localDeviceId,
    const DiscoveryAnnouncement& announcement,
    const std::string& observedAddress)
{
    if (localDeviceId.empty()) {
        throw std::invalid_argument("Local device ID cannot be empty.");
    }

    if (announcement.GetDeviceId() == localDeviceId) {
        return DiscoveryProcessResult::IgnoredSelf();
    }

    const bool peerCreated = !std::filesystem::exists(
        relaydesk::storage::getPeerProfileFilePath(appPaths,
                                                   announcement.GetDeviceId()));
    return DiscoveryProcessResult::StoredPeer(
        upsertPeerProfileFromDiscovery(appPaths, announcement, observedAddress),
        peerCreated);
}

}
