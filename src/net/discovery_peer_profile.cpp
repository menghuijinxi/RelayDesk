#include "net/discovery_peer_profile.h"

#include <algorithm>
#include <filesystem>
#include <optional>
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
    profile.SetAppVersion(announcement.GetAppVersion());
    profile.SetCapabilities(announcement.GetCapabilities());
    profile.SetFirstSeenAt(firstSeenAt);
    profile.SetLastSeenAt(announcement.GetTimestamp());
    return profile;
}

bool containsAddress(const std::vector<std::string>& addresses,
                     const std::string& address)
{
    return std::find(addresses.begin(), addresses.end(), address) != addresses.end();
}

bool isNewerThanProfile(const DiscoveryAnnouncement& announcement,
                        const relaydesk::storage::PeerProfile& profile)
{
    return announcement.GetTimestamp() >= profile.GetLastSeenAt();
}

bool shouldIgnoreProfileUpdate(const DiscoveryAnnouncement& announcement,
                               const relaydesk::storage::PeerProfile& profile)
{
    if (announcement.GetAppVersion() > profile.GetAppVersion()) {
        return false;
    }

    return announcement.GetTimestamp() <= profile.GetLastSeenAt();
}

std::string resolveLastSeenAt(const DiscoveryAnnouncement& announcement,
                              const relaydesk::storage::PeerProfile& profile)
{
    if (announcement.GetTimestamp() < profile.GetLastSeenAt()) {
        return profile.GetLastSeenAt();
    }

    return announcement.GetTimestamp();
}

void removeStaleProfilesForAnnouncement(
    const relaydesk::storage::AppPaths& appPaths,
    const DiscoveryAnnouncement& announcement,
    const std::string& observedAddress)
{
    for (const auto& profile : relaydesk::storage::loadPeerProfiles(appPaths)) {
        if (profile.GetDeviceId() == announcement.GetDeviceId()
            || profile.GetHostName() != announcement.GetHostName()
            || !containsAddress(profile.GetLastAddresses(), observedAddress)
            || !isNewerThanProfile(announcement, profile)) {
            continue;
        }

        const std::filesystem::path stalePath =
            relaydesk::storage::getPeerProfileFilePath(
                appPaths,
                profile.GetDeviceId()).parent_path();
        std::filesystem::remove_all(stalePath);
    }
}

std::optional<relaydesk::storage::PeerProfile> loadPeerProfileIfReadable(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& peerDeviceId)
{
    try {
        return relaydesk::storage::loadPeerProfile(appPaths, peerDeviceId);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

relaydesk::storage::PeerProfile upsertPeerProfileFromDiscovery(
    const relaydesk::storage::AppPaths& appPaths,
    const DiscoveryAnnouncement& announcement,
    const std::string& observedAddress)
{
    const std::filesystem::path profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, announcement.GetDeviceId());
    const std::optional<relaydesk::storage::PeerProfile> existingProfile =
        std::filesystem::exists(profilePath)
            ? loadPeerProfileIfReadable(appPaths, announcement.GetDeviceId())
            : std::nullopt;
    if (!existingProfile.has_value()) {
        removeStaleProfilesForAnnouncement(appPaths, announcement, observedAddress);
        relaydesk::storage::PeerProfile createdProfile = makeBaseProfile(
            announcement,
            announcement.GetTimestamp(),
            mergeObservedAddress({}, observedAddress));
        relaydesk::storage::savePeerProfile(appPaths, createdProfile);
        return createdProfile;
    }

    if (shouldIgnoreProfileUpdate(announcement, existingProfile.value())) {
        return existingProfile.value();
    }

    removeStaleProfilesForAnnouncement(appPaths, announcement, observedAddress);
    relaydesk::storage::PeerProfile updatedProfile = makeBaseProfile(
        announcement,
        existingProfile->GetFirstSeenAt(),
        mergeObservedAddress(existingProfile->GetLastAddresses(), observedAddress));
    updatedProfile.SetLastSeenAt(resolveLastSeenAt(announcement, existingProfile.value()));
    updatedProfile.SetUnreadMessageCount(existingProfile->GetUnreadMessageCount());
    relaydesk::storage::savePeerProfile(appPaths, updatedProfile);
    return updatedProfile;
}

}
