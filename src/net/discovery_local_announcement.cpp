#include "net/discovery_local_announcement.h"

#include "core/app_version.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace relaydesk::net {
namespace {

constexpr std::uint16_t kMinTcpPort = 1;

void validateCapabilities(const std::vector<std::string>& capabilities)
{
    for (const auto& capability : capabilities) {
        if (capability.empty()) {
            throw std::invalid_argument("Discovery capability cannot be empty.");
        }
    }
}

} // namespace

DiscoveryAnnouncement makeLocalDiscoveryAnnouncement(
    const relaydesk::storage::LocalIdentity& identity,
    std::uint16_t tcpPort,
    std::vector<std::string> capabilities,
    const std::string& timestamp)
{
    if (identity.GetDeviceId().empty()
        || identity.GetHostName().empty()
        || identity.GetDisplayName().empty()) {
        throw std::invalid_argument("Local identity is missing discovery fields.");
    }

    if (tcpPort < kMinTcpPort) {
        throw std::invalid_argument("Discovery TCP port cannot be zero.");
    }

    if (timestamp.empty()) {
        throw std::invalid_argument("Discovery timestamp cannot be empty.");
    }

    validateCapabilities(capabilities);

    DiscoveryAnnouncement announcement;
    announcement.SetDeviceId(identity.GetDeviceId());
    announcement.SetHostName(identity.GetHostName());
    announcement.SetDisplayName(identity.GetDisplayName());
    announcement.SetTcpPort(tcpPort);
    announcement.SetAppVersion(relaydesk::core::kAppVersion);
    announcement.SetCapabilities(std::move(capabilities));
    announcement.SetTimestamp(timestamp);
    return announcement;
}

}
