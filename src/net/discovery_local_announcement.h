#pragma once

#include "net/discovery_message.h"
#include "storage/local_identity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace relaydesk::net {

DiscoveryAnnouncement makeLocalDiscoveryAnnouncement(
    const relaydesk::storage::LocalIdentity& identity,
    std::uint16_t tcpPort,
    std::vector<std::string> capabilities,
    const std::string& timestamp);

}
