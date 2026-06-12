#pragma once

#include "net/discovery_message.h"
#include "storage/app_paths.h"
#include "storage/peer_profile.h"

#include <optional>
#include <string>

namespace relaydesk::net {

enum class DiscoveryProcessAction {
    IgnoredSelf,
    StoredPeer,
};

class DiscoveryProcessResult {
public:
    DiscoveryProcessAction GetAction() const { return action_; }
    bool HasPeerProfile() const { return peerProfile_.has_value(); }
    const std::optional<relaydesk::storage::PeerProfile>& GetPeerProfile() const
    {
        return peerProfile_;
    }

    static DiscoveryProcessResult IgnoredSelf();
    static DiscoveryProcessResult StoredPeer(
        relaydesk::storage::PeerProfile peerProfile);

protected:
    DiscoveryProcessAction action_ = DiscoveryProcessAction::IgnoredSelf;
    std::optional<relaydesk::storage::PeerProfile> peerProfile_;
};

DiscoveryProcessResult processDiscoveryAnnouncement(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& localDeviceId,
    const DiscoveryAnnouncement& announcement,
    const std::string& observedAddress);

}
