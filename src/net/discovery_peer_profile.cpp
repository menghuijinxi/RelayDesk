#include "net/discovery_peer_profile.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace relaydesk::net {
namespace {

std::vector<std::string> mergeObservedAddress(std::vector<std::string> existingAddresses,
                                              const std::string& observedAddress)
{
    if (observedAddress.empty()) {
        throw std::invalid_argument("Observed discovery address cannot be empty.");
    }

    std::vector<std::string> mergedAddresses{observedAddress};
    for (const auto& address : existingAddresses) {
        if (address != observedAddress
            && std::find(mergedAddresses.begin(), mergedAddresses.end(), address)
                == mergedAddresses.end()) {
            mergedAddresses.push_back(address);
        }
    }
    return mergedAddresses;
}

relaydesk::storage::PeerProfile makeBaseProfile(
    const DiscoveryAnnouncement& announcement,
    const std::string& firstSeenAt,
    const std::vector<std::string>& addresses)
{
    relaydesk::storage::PeerProfile profile;
    profile.SetDeviceId(announcement.GetDeviceId());
    profile.SetHostName(announcement.GetHostName());
    profile.SetDisplayName(announcement.GetDisplayName());
    profile.SetLastAddresses(addresses);
    profile.SetTcpPort(announcement.GetTcpPort());
    profile.SetCapabilities(announcement.GetCapabilities());
    profile.SetFirstSeenAt(firstSeenAt);
    profile.SetLastSeenAt(announcement.GetTimestamp());
    return profile;
}

} // namespace

relaydesk::storage::PeerProfile upsertPeerProfileFromDiscovery(
    const relaydesk::storage::AppPaths& appPaths,
    const DiscoveryAnnouncement& announcement,
    const std::string& observedAddress)
{
    const std::filesystem::path profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, announcement.GetDeviceId());
    if (!std::filesystem::exists(profilePath)) {
        relaydesk::storage::PeerProfile createdProfile = makeBaseProfile(
            announcement,
            announcement.GetTimestamp(),
            mergeObservedAddress({}, observedAddress));
        relaydesk::storage::savePeerProfile(appPaths, createdProfile);
        return createdProfile;
    }

    const relaydesk::storage::PeerProfile existingProfile =
        relaydesk::storage::loadPeerProfile(appPaths, announcement.GetDeviceId());
    relaydesk::storage::PeerProfile updatedProfile = makeBaseProfile(
        announcement,
        existingProfile.GetFirstSeenAt(),
        mergeObservedAddress(existingProfile.GetLastAddresses(), observedAddress));
    relaydesk::storage::savePeerProfile(appPaths, updatedProfile);
    return updatedProfile;
}

}
