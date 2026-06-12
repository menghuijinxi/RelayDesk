#include "main/app_runtime.h"

#include "platform/computer_name.h"
#include "storage/app_paths.h"
#include "storage/local_identity.h"
#include "storage/peer_profile.h"

#if defined(RELAYDESK_HAS_BOOST_ASIO)
#include "net/discovery_service.h"
#include "net/discovery_worker.h"
#endif

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace relaydesk::runtime {
namespace {

using namespace std::chrono_literals;

std::string choosePeerAddress(const relaydesk::storage::PeerProfile& profile)
{
    if (profile.GetLastAddresses().empty()) {
        return "unknown";
    }

    return profile.GetLastAddresses().front();
}

PeerListItem makePeerListItem(const relaydesk::storage::PeerProfile& profile)
{
    PeerListItem item;
    item.SetDeviceId(profile.GetDeviceId());
    item.SetDisplayName(profile.GetDisplayName());
    item.SetHostName(profile.GetHostName());
    item.SetAddress(choosePeerAddress(profile));
    item.SetLastSeenAt(profile.GetLastSeenAt());
    item.SetOnline(true);
    return item;
}

#if defined(RELAYDESK_HAS_BOOST_ASIO)
relaydesk::net::DiscoveryWorkerConfig makeDiscoveryWorkerConfig()
{
    relaydesk::net::DiscoveryWorkerConfig config;
    config.SetBroadcastInterval(15s);
    config.SetPollTimeout(100ms);
    config.SetBroadcastEnabled(true);
    config.SetAnnounceOnStart(true);
    return config;
}
#endif

} // namespace

class DiscoveryWorkerHandle {
public:
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    explicit DiscoveryWorkerHandle(
        std::unique_ptr<relaydesk::net::DiscoveryWorker> worker)
        : worker_(std::move(worker))
    {
    }

protected:
    std::unique_ptr<relaydesk::net::DiscoveryWorker> worker_;
#endif
};

void LocalUserSummary::SetDisplayName(std::string displayName)
{
    displayName_ = std::move(displayName);
}

void LocalUserSummary::SetHostName(std::string hostName)
{
    hostName_ = std::move(hostName);
}

void LocalUserSummary::SetDeviceId(std::string deviceId)
{
    deviceId_ = std::move(deviceId);
}

void PeerListItem::SetDeviceId(std::string deviceId)
{
    deviceId_ = std::move(deviceId);
}

void PeerListItem::SetDisplayName(std::string displayName)
{
    displayName_ = std::move(displayName);
}

void PeerListItem::SetHostName(std::string hostName)
{
    hostName_ = std::move(hostName);
}

void PeerListItem::SetAddress(std::string address)
{
    address_ = std::move(address);
}

void PeerListItem::SetLastSeenAt(std::string lastSeenAt)
{
    lastSeenAt_ = std::move(lastSeenAt);
}

void PeerListItem::SetOnline(bool online)
{
    online_ = online;
}

RelayDeskRuntime::RelayDeskRuntime()
{
    initialize();
}

RelayDeskRuntime::~RelayDeskRuntime() = default;

void RelayDeskRuntime::refreshPeersIfNeeded()
{
    if (!storageAvailable_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < nextPeerRefreshAt_) {
        return;
    }

    refreshPeers();
    nextPeerRefreshAt_ = now + 1s;
}

void RelayDeskRuntime::initialize()
{
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(appPaths);

        const std::string hostName = relaydesk::platform::getComputerNameUtf8();
        const auto identity =
            relaydesk::storage::loadOrCreateLocalIdentity(appPaths, hostName);

        localUser_.SetDisplayName(identity.GetDisplayName());
        localUser_.SetHostName(identity.GetHostName());
        localUser_.SetDeviceId(identity.GetDeviceId());
        storageAvailable_ = true;

#if defined(RELAYDESK_HAS_BOOST_ASIO)
        relaydesk::net::DiscoveryServiceConfig serviceConfig;
        relaydesk::net::DiscoveryService discoveryService(
            appPaths,
            identity,
            serviceConfig);
        auto discoveryWorker = std::make_unique<relaydesk::net::DiscoveryWorker>(
            std::move(discoveryService),
            makeDiscoveryWorkerConfig());
        discoveryWorker->start();
        discoveryStarted_ = true;
        discoveryUdpPort_ = discoveryWorker->GetLocalUdpPort();
        discoveryWorker_ =
            std::make_unique<DiscoveryWorkerHandle>(std::move(discoveryWorker));
#endif

        refreshPeers();
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
}

void RelayDeskRuntime::refreshPeers()
{
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        const std::vector<relaydesk::storage::PeerProfile> profiles =
            relaydesk::storage::loadPeerProfiles(appPaths);

        std::vector<PeerListItem> nextPeers;
        nextPeers.reserve(profiles.size());
        for (const auto& profile : profiles) {
            nextPeers.push_back(makePeerListItem(profile));
        }
        peers_ = std::move(nextPeers);
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
}

void RelayDeskRuntime::setStartupError(std::string errorMessage)
{
    startupErrorMessage_ = std::move(errorMessage);
}

RelayDeskRuntime& getRelayDeskRuntime()
{
    static RelayDeskRuntime runtime;
    return runtime;
}

}
