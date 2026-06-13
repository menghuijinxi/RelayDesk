#include "main/app_runtime.h"

#include "core/diagnostic_log.h"
#include "core/platform/async.h"
#include "platform/computer_name.h"
#include "platform/windows_install_id.h"
#include "storage/app_paths.h"
#include "storage/local_identity.h"
#include "storage/peer_profile.h"

#if defined(RELAYDESK_HAS_BOOST_ASIO)
#include "net/discovery_message.h"
#include "net/discovery_service.h"
#include "net/discovery_worker.h"
#endif

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace relaydesk::runtime {
namespace {

using namespace std::chrono_literals;

constexpr const char* kDiscoveryLogFileName = "discovery.log";
constexpr auto kPeerOnlineTimeout = 10s;
constexpr auto kPeerStatusRefreshInterval = 1s;

constexpr bool discoveryTraceEnabled()
{
#if defined(RELAYDESK_ENABLE_DISCOVERY_TRACE)
    return true;
#else
    return false;
#endif
}

std::string choosePeerAddress(const relaydesk::storage::PeerProfile& profile)
{
    if (profile.GetLastAddresses().empty()) {
        return "unknown";
    }

    return profile.GetLastAddresses().front();
}

bool isPeerOnline(std::chrono::steady_clock::time_point lastOnlineSignalAt,
                  std::chrono::steady_clock::time_point now)
{
    return lastOnlineSignalAt != std::chrono::steady_clock::time_point{}
        && lastOnlineSignalAt + kPeerOnlineTimeout >= now;
}

bool hasPeerAddress(const relaydesk::storage::PeerProfile& profile,
                    const std::string& address)
{
    const auto& addresses = profile.GetLastAddresses();
    return std::find(addresses.begin(), addresses.end(), address) != addresses.end();
}

bool isStalePeerForProfile(const PeerListItem& peer,
                           const relaydesk::storage::PeerProfile& profile)
{
    if (peer.GetDeviceId() == profile.GetDeviceId()
        || peer.GetHostName() != profile.GetHostName()
        || peer.GetAddress().empty()
        || peer.GetAddress() == "unknown") {
        return false;
    }

    return hasPeerAddress(profile, peer.GetAddress());
}

bool isStaleOnlineProfile(const PeerListItem& peer,
                          const relaydesk::storage::PeerProfile& profile,
                          bool online)
{
    return online
        && !peer.GetLastSeenAt().empty()
        && !profile.GetLastSeenAt().empty()
        && profile.GetLastSeenAt() <= peer.GetLastSeenAt();
}

PeerListItem makePeerListItem(const relaydesk::storage::PeerProfile& profile,
                              bool online)
{
    PeerListItem item;
    item.SetDeviceId(profile.GetDeviceId());
    item.SetDisplayName(profile.GetDisplayName());
    item.SetHostName(profile.GetHostName());
    item.SetAddress(choosePeerAddress(profile));
    item.SetLastSeenAt(profile.GetLastSeenAt());
    item.SetOnline(online);
    return item;
}

#if defined(RELAYDESK_HAS_BOOST_ASIO)
relaydesk::net::DiscoveryWorkerConfig makeDiscoveryWorkerConfig()
{
    relaydesk::net::DiscoveryWorkerConfig config;
    config.SetBroadcastInterval(2s);
    config.SetStartupBroadcastInterval(500ms);
    config.SetStartupBroadcastCount(8);
    config.SetPollTimeout(100ms);
    config.SetBroadcastEnabled(true);
    config.SetAnnounceOnStart(true);
    return config;
}
#endif

std::filesystem::path makeDiscoveryLogFilePath(
    const relaydesk::storage::AppPaths& appPaths)
{
    return appPaths.GetLogsDirectory() / kDiscoveryLogFileName;
}

std::string getStableDeviceId()
{
    try {
        return relaydesk::platform::getWindowsInstallId();
    } catch (const std::exception&) {
        return {};
    }
}

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

void PeerListItem::SetLastOnlineSignalAt(
    std::chrono::steady_clock::time_point signalAt)
{
    lastOnlineSignalAt_ = signalAt;
}

void PeerListItem::SetOnline(bool online)
{
    online_ = online;
}

PendingPeerProfile::PendingPeerProfile(
    relaydesk::storage::PeerProfile profile,
    bool online)
    : profile_(std::move(profile)),
      online_(online)
{
}

RelayDeskRuntime::RelayDeskRuntime()
{
    initialize();
}

RelayDeskRuntime::~RelayDeskRuntime() = default;

std::optional<PeerListItem> RelayDeskRuntime::GetSelectedPeer() const
{
    const auto selected = std::find_if(
        peers_.begin(),
        peers_.end(),
        [this](const PeerListItem& peer) {
            return peer.GetDeviceId() == selectedPeerDeviceId_;
        });
    if (selected == peers_.end()) {
        return std::nullopt;
    }

    return *selected;
}

void RelayDeskRuntime::refreshPeersIfNeeded()
{
    if (!storageAvailable_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    drainPendingPeerProfiles();
    if (now >= nextPeerStatusRefreshAt_) {
        refreshPeerOnlineStates();
        nextPeerStatusRefreshAt_ = now + kPeerStatusRefreshInterval;
    }
    requestPeerStatusRefresh();
}

void RelayDeskRuntime::selectPeer(std::string deviceId)
{
    const auto selected = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&deviceId](const PeerListItem& peer) {
            return peer.GetDeviceId() == deviceId;
        });
    if (selected == peers_.end()) {
        return;
    }

    selectedPeerDeviceId_ = std::move(deviceId);
}

void RelayDeskRuntime::initialize()
{
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(appPaths);
        diagnosticLogFilePath_ = makeDiscoveryLogFilePath(appPaths);
        logDiagnostic("runtime.initialize.begin");

        const std::string hostName = relaydesk::platform::getComputerNameUtf8();
        const auto identity = relaydesk::storage::loadOrCreateLocalIdentity(
            appPaths,
            hostName,
            getStableDeviceId());

        localUser_.SetDisplayName(identity.GetDisplayName());
        localUser_.SetHostName(identity.GetHostName());
        localUser_.SetDeviceId(identity.GetDeviceId());
        storageAvailable_ = true;
        logDiagnostic("runtime.identity.loaded device_id=" + identity.GetDeviceId()
                      + " host_name=" + identity.GetHostName()
                      + " display_name=" + identity.GetDisplayName());

#if defined(RELAYDESK_HAS_BOOST_ASIO)
        relaydesk::net::DiscoveryServiceConfig serviceConfig;
        relaydesk::net::DiscoveryService discoveryService(
            appPaths,
            identity,
            serviceConfig);
        relaydesk::net::DiscoveryWorkerEvents workerEvents;
        workerEvents.SetPeerStoredCallback(
            [this](relaydesk::storage::PeerProfile profile,
                   std::string announcementType) {
                enqueuePeerProfile(
                    std::move(profile),
                    announcementType
                        != relaydesk::net::kDiscoveryAnnouncementTypeOffline);
            });
        auto discoveryWorker = std::make_unique<relaydesk::net::DiscoveryWorker>(
            std::move(discoveryService),
            makeDiscoveryWorkerConfig(),
            std::move(workerEvents));
        discoveryWorker->start();
        discoveryStarted_ = true;
        discoveryUdpPort_ = discoveryWorker->GetLocalUdpPort();
        logDiagnostic("runtime.discovery.started udp_port="
                      + std::to_string(discoveryUdpPort_));
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
            nextPeers.push_back(makePeerListItem(profile, false));
        }
        peers_ = std::move(nextPeers);
        logDiagnostic("runtime.peer.refresh_from_storage count="
                      + std::to_string(peers_.size()));
        syncSelectedPeer();
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
}

void RelayDeskRuntime::enqueuePeerProfile(
    relaydesk::storage::PeerProfile profile,
    bool online)
{
    const std::string deviceId = profile.GetDeviceId();
    const std::string address = choosePeerAddress(profile);
    const std::string lastSeenAt = profile.GetLastSeenAt();
    std::size_t pendingCount = 0;
    {
        std::lock_guard lock(pendingPeerMutex_);
        pendingPeerProfiles_.emplace_back(std::move(profile), online);
        pendingCount = pendingPeerProfiles_.size();
    }

    logDiagnostic("runtime.peer.enqueue device_id=" + deviceId
                  + " address=" + address
                  + " online=" + std::to_string(online)
                  + " last_seen=" + lastSeenAt
                  + " pending_count=" + std::to_string(pendingCount));
    requestUiRefresh();
}

void RelayDeskRuntime::drainPendingPeerProfiles()
{
    std::vector<PendingPeerProfile> pendingProfiles;
    {
        std::lock_guard lock(pendingPeerMutex_);
        pendingProfiles.swap(pendingPeerProfiles_);
    }

    if (pendingProfiles.empty()) {
        return;
    }

    logDiagnostic("runtime.peer.drain count="
                  + std::to_string(pendingProfiles.size()));
    const auto now = std::chrono::steady_clock::now();
    for (const auto& pendingProfile : pendingProfiles) {
        applyPeerProfile(pendingProfile.GetProfile(),
                         pendingProfile.GetOnline(),
                         now);
    }
    syncSelectedPeer();
}

void RelayDeskRuntime::applyPeerProfile(
    const relaydesk::storage::PeerProfile& profile,
    bool online,
    std::chrono::steady_clock::time_point now)
{
    const auto peerCountBeforeCleanup = peers_.size();
    peers_.erase(
        std::remove_if(
            peers_.begin(),
            peers_.end(),
            [&profile](const PeerListItem& peer) {
                return isStalePeerForProfile(peer, profile);
            }),
        peers_.end());
    const auto stalePeerCount = peerCountBeforeCleanup - peers_.size();

    const auto existing = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&profile](const PeerListItem& peer) {
            return peer.GetDeviceId() == profile.GetDeviceId();
        });
    if (existing != peers_.end()
        && isStaleOnlineProfile(*existing, profile, online)) {
        logDiagnostic("runtime.peer.apply action=ignored_stale_online device_id="
                      + profile.GetDeviceId()
                      + " incoming_last_seen=" + profile.GetLastSeenAt()
                      + " current_last_seen=" + existing->GetLastSeenAt());
        return;
    }

    PeerListItem item = makePeerListItem(profile, online);
    if (online) {
        item.SetLastOnlineSignalAt(now);
    }
    if (existing == peers_.end()) {
        peers_.push_back(item);
        logDiagnostic("runtime.peer.apply action=added device_id="
                      + profile.GetDeviceId()
                      + " address=" + item.GetAddress()
                      + " online=" + std::to_string(online)
                      + " last_seen=" + item.GetLastSeenAt()
                      + " stale_removed="
                      + std::to_string(stalePeerCount));
        return;
    }

    const bool wasOnline = existing->GetOnline();
    const std::string previousLastSeenAt = existing->GetLastSeenAt();
    *existing = item;
    logDiagnostic("runtime.peer.apply action=updated device_id="
                  + profile.GetDeviceId()
                  + " address=" + item.GetAddress()
                  + " online=" + std::to_string(online)
                  + " was_online=" + std::to_string(wasOnline)
                  + " incoming_last_seen=" + item.GetLastSeenAt()
                  + " previous_last_seen=" + previousLastSeenAt
                  + " stale_removed=" + std::to_string(stalePeerCount));
}

void RelayDeskRuntime::refreshPeerOnlineStates()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& peer : peers_) {
        const bool wasOnline = peer.GetOnline();
        const bool online = isPeerOnline(peer.GetLastOnlineSignalAt(), now);
        peer.SetOnline(online);
        if (wasOnline != online) {
            logDiagnostic("runtime.peer.online_changed device_id="
                          + peer.GetDeviceId()
                          + " online=" + std::to_string(online));
        }
    }
}

void RelayDeskRuntime::syncSelectedPeer()
{
    if (peers_.empty()) {
        selectedPeerDeviceId_.clear();
        return;
    }

    const auto selected = std::find_if(
        peers_.begin(),
        peers_.end(),
        [this](const PeerListItem& peer) {
            return peer.GetDeviceId() == selectedPeerDeviceId_;
        });
    if (selected == peers_.end()) {
        selectedPeerDeviceId_ = peers_.front().GetDeviceId();
    }
}

void RelayDeskRuntime::setStartupError(std::string errorMessage)
{
    logDiagnostic("runtime.error message=" + errorMessage);
    startupErrorMessage_ = std::move(errorMessage);
}

void RelayDeskRuntime::requestUiRefresh()
{
    bool expected = false;
    if (!uiRefreshPending_.compare_exchange_strong(expected, true)) {
        return;
    }

    const bool accepted = ::core::async::restart(
        "relaydesk.discovery.refresh",
        [] {
            return ::core::async::success();
        },
        [this](const ::core::async::Result<void>&) {
            uiRefreshPending_.store(false);
        });
    if (!accepted) {
        uiRefreshPending_.store(false);
    }
}

void RelayDeskRuntime::requestPeerStatusRefresh()
{
    bool expected = false;
    if (!peerStatusRefreshPending_.compare_exchange_strong(expected, true)) {
        return;
    }

    const bool accepted = ::core::async::restart(
        "relaydesk.peer.status.refresh",
        [](const ::core::async::CancelToken& token) {
            const auto waitUntil =
                std::chrono::steady_clock::now() + kPeerStatusRefreshInterval;
            while (!token.canceled()
                   && std::chrono::steady_clock::now() < waitUntil) {
                std::this_thread::sleep_for(50ms);
            }
            return ::core::async::success();
        },
        [this](const ::core::async::Result<void>&) {
            peerStatusRefreshPending_.store(false);
        });
    if (!accepted) {
        peerStatusRefreshPending_.store(false);
    }
}

void RelayDeskRuntime::logDiagnostic(const std::string& message) const noexcept
{
    (void)message;
    if constexpr (!discoveryTraceEnabled()) {
        return;
    }

    if (diagnosticLogFilePath_.empty()) {
        return;
    }

    relaydesk::core::appendDiagnosticLogLine(diagnosticLogFilePath_, message);
}

RelayDeskRuntime& getRelayDeskRuntime()
{
    static RelayDeskRuntime runtime;
    return runtime;
}

}
