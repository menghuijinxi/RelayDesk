#pragma once

#include "net/discovery_message.h"
#include "storage/app_paths.h"
#include "storage/peer_profile.h"

#include <string>

namespace relaydesk::net {

relaydesk::storage::PeerProfile upsertPeerProfileFromDiscovery(
    const relaydesk::storage::AppPaths& appPaths,
    const DiscoveryAnnouncement& announcement,
    const std::string& observedAddress);

}
