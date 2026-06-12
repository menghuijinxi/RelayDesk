#include "net/discovery_service.h"

#include "core/time.h"
#include "net/discovery_local_announcement.h"
#include "net/discovery_processor.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace relaydesk::net {
namespace {

constexpr std::uint16_t kMinPort = 1;

void validateConfig(const DiscoveryServiceConfig& config)
{
    if (config.GetAdvertisedTcpPort() < kMinPort) {
        throw std::invalid_argument("Discovery advertised TCP port cannot be zero.");
    }

    for (const auto& capability : config.GetCapabilities()) {
        if (capability.empty()) {
            throw std::invalid_argument("Discovery capability cannot be empty.");
        }
    }
}

DiscoveryServicePollResult fromProcessResult(
    const DiscoveryProcessResult& processResult)
{
    if (processResult.GetAction() == DiscoveryProcessAction::IgnoredSelf) {
        return DiscoveryServicePollResult::IgnoredSelf();
    }

    if (!processResult.HasPeerProfile()) {
        throw std::runtime_error("Stored discovery peer did not return a profile.");
    }

    return DiscoveryServicePollResult::StoredPeer(
        processResult.GetPeerProfile().value());
}

} // namespace

DiscoveryServicePollResult DiscoveryServicePollResult::NoPacket()
{
    return {};
}

DiscoveryServicePollResult DiscoveryServicePollResult::InvalidPacket(
    std::string errorMessage)
{
    DiscoveryServicePollResult result;
    result.action_ = DiscoveryServicePollAction::InvalidPacket;
    result.errorMessage_ = std::move(errorMessage);
    return result;
}

DiscoveryServicePollResult DiscoveryServicePollResult::IgnoredSelf()
{
    DiscoveryServicePollResult result;
    result.action_ = DiscoveryServicePollAction::IgnoredSelf;
    return result;
}

DiscoveryServicePollResult DiscoveryServicePollResult::StoredPeer(
    relaydesk::storage::PeerProfile peerProfile)
{
    DiscoveryServicePollResult result;
    result.action_ = DiscoveryServicePollAction::StoredPeer;
    result.peerProfile_ = std::move(peerProfile);
    return result;
}

DiscoveryService::DiscoveryService(relaydesk::storage::AppPaths appPaths,
                                   relaydesk::storage::LocalIdentity localIdentity,
                                   DiscoveryServiceConfig config)
    : appPaths_(std::move(appPaths)),
      localIdentity_(std::move(localIdentity)),
      config_(std::move(config)),
      transport_(config_.GetDiscoveryUdpPort())
{
    validateConfig(config_);
}

std::uint16_t DiscoveryService::GetLocalUdpPort() const
{
    return transport_.GetLocalPort();
}

void DiscoveryService::broadcastNow()
{
    transport_.sendBroadcast(makeAnnouncementPayload(),
                             config_.GetDiscoveryUdpPort());
}

void DiscoveryService::sendAnnouncementTo(const std::string& address,
                                          std::uint16_t port)
{
    transport_.sendTo(makeAnnouncementPayload(), address, port);
}

DiscoveryServicePollResult DiscoveryService::pollOnce(
    std::chrono::milliseconds timeout)
{
    const auto packet = transport_.tryReceiveFor(timeout);
    if (!packet.has_value()) {
        return DiscoveryServicePollResult::NoPacket();
    }

    DiscoveryAnnouncement announcement;
    try {
        announcement = parseDiscoveryAnnouncement(packet->GetPayload());
    } catch (const std::exception& error) {
        return DiscoveryServicePollResult::InvalidPacket(error.what());
    }

    return fromProcessResult(processDiscoveryAnnouncement(
        appPaths_,
        localIdentity_.GetDeviceId(),
        announcement,
        packet->GetObservedAddress()));
}

void DiscoveryService::close()
{
    transport_.close();
}

std::string DiscoveryService::makeAnnouncementPayload() const
{
    DiscoveryAnnouncement announcement = makeLocalDiscoveryAnnouncement(
        localIdentity_,
        config_.GetAdvertisedTcpPort(),
        config_.GetCapabilities(),
        relaydesk::core::currentUtcTimestamp());
    return serializeDiscoveryAnnouncement(announcement);
}

}
