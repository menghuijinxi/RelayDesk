#include "net/discovery_service.h"

#include "core/diagnostic_log.h"
#include "core/time.h"
#include "net/discovery_local_announcement.h"
#include "net/discovery_message.h"
#include "net/discovery_processor.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace relaydesk::net {
namespace {

constexpr std::uint16_t kMinPort = 1;
constexpr const char* kDiscoveryLogFileName = "discovery.log";

constexpr bool discoveryTraceEnabled()
{
#if defined(RELAYDESK_ENABLE_DISCOVERY_TRACE)
    return true;
#else
    return false;
#endif
}

std::filesystem::path makeDiscoveryLogFilePath(
    const relaydesk::storage::AppPaths& appPaths)
{
    return appPaths.GetLogsDirectory() / kDiscoveryLogFileName;
}

std::string endpointText(const UdpDiscoveryPacket& packet)
{
    return packet.GetObservedAddress() + ":"
        + std::to_string(packet.GetObservedPort());
}

std::string pollActionText(DiscoveryServicePollAction action)
{
    switch (action) {
    case DiscoveryServicePollAction::NoPacket:
        return "no_packet";
    case DiscoveryServicePollAction::InvalidPacket:
        return "invalid_packet";
    case DiscoveryServicePollAction::IgnoredSelf:
        return "ignored_self";
    case DiscoveryServicePollAction::StoredPeer:
        return "stored_peer";
    }

    return "unknown";
}

void validateConfig(const DiscoveryServiceConfig& config)
{
    if (config.GetAdvertisedTcpPort() < kMinPort) {
        throw std::invalid_argument("Discovery advertised TCP port cannot be zero.");
    }

    if (config.GetAppVersion() < 0) {
        throw std::invalid_argument("Discovery app version cannot be negative.");
    }

    for (const auto& capability : config.GetCapabilities()) {
        if (capability.empty()) {
            throw std::invalid_argument("Discovery capability cannot be empty.");
        }
    }
}

DiscoveryServicePollResult fromProcessResult(
    const DiscoveryProcessResult& processResult,
    const DiscoveryAnnouncement& announcement,
    const UdpDiscoveryPacket& packet)
{
    if (processResult.GetAction() == DiscoveryProcessAction::IgnoredSelf) {
        return DiscoveryServicePollResult::IgnoredSelf();
    }

    if (!processResult.HasPeerProfile()) {
        throw std::runtime_error("Stored discovery peer did not return a profile.");
    }

    return DiscoveryServicePollResult::StoredPeer(
        processResult.GetPeerProfile().value(),
        processResult.GetPeerCreated(),
        packet.GetObservedAddress(),
        announcement.GetType(),
        packet.GetObservedPort());
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
    relaydesk::storage::PeerProfile peerProfile,
    bool peerCreated,
    std::string observedAddress,
    std::string announcementType,
    std::uint16_t observedPort)
{
    DiscoveryServicePollResult result;
    result.action_ = DiscoveryServicePollAction::StoredPeer;
    result.peerProfile_ = std::move(peerProfile);
    result.peerCreated_ = peerCreated;
    result.observedAddress_ = std::move(observedAddress);
    result.announcementType_ = std::move(announcementType);
    result.observedPort_ = observedPort;
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
    if constexpr (discoveryTraceEnabled()) {
        const auto logFilePath = makeDiscoveryLogFilePath(appPaths_);
        transport_.SetLogCallback(
            [logFilePath](std::string message) {
                relaydesk::core::appendDiagnosticLogLine(
                    logFilePath,
                    "transport." + message);
            });
        logDiagnostic("service.start local_udp_port="
                      + std::to_string(transport_.GetLocalPort())
                      + " discovery_udp_port="
                      + std::to_string(config_.GetDiscoveryUdpPort())
                      + " advertised_tcp_port="
                      + std::to_string(config_.GetAdvertisedTcpPort())
                      + " app_version="
                      + std::to_string(config_.GetAppVersion())
                      + " device_id=" + localIdentity_.GetDeviceId()
                      + " host_name=" + localIdentity_.GetHostName()
                      + " display_name=" + localIdentity_.GetDisplayName());
    }
}

std::uint16_t DiscoveryService::GetLocalUdpPort() const
{
    return transport_.GetLocalPort();
}

void DiscoveryService::broadcastNow()
{
    logDiagnostic("service.broadcast_hello.begin port="
                  + std::to_string(config_.GetDiscoveryUdpPort()));
    try {
        transport_.sendBroadcast(
            makeAnnouncementPayload(kDiscoveryAnnouncementTypeHello),
            config_.GetDiscoveryUdpPort());
        logDiagnostic("service.broadcast_hello.success");
    } catch (const std::exception& error) {
        logDiagnostic(std::string("service.broadcast_hello.failed error=")
                      + error.what());
        throw;
    }
}

void DiscoveryService::broadcastOfflineNow()
{
    logDiagnostic("service.broadcast_offline.begin port="
                  + std::to_string(config_.GetDiscoveryUdpPort()));
    try {
        transport_.sendBroadcast(
            makeAnnouncementPayload(kDiscoveryAnnouncementTypeOffline),
            config_.GetDiscoveryUdpPort());
        logDiagnostic("service.broadcast_offline.success");
    } catch (const std::exception& error) {
        logDiagnostic(std::string("service.broadcast_offline.failed error=")
                      + error.what());
        throw;
    }
}

void DiscoveryService::sendAnnouncementTo(const std::string& address,
                                          std::uint16_t port)
{
    logDiagnostic("service.send_hello.begin address=" + address
                  + " port=" + std::to_string(port));
    try {
        transport_.sendTo(makeAnnouncementPayload(kDiscoveryAnnouncementTypeHello),
                          address,
                          port);
        logDiagnostic("service.send_hello.success address=" + address
                      + " port=" + std::to_string(port));
    } catch (const std::exception& error) {
        logDiagnostic("service.send_hello.failed address=" + address
                      + " port=" + std::to_string(port)
                      + " error=" + error.what());
        throw;
    }
}

void DiscoveryService::sendReplyTo(const std::string& address,
                                   std::uint16_t port)
{
    logDiagnostic("service.send_reply.begin address=" + address
                  + " port=" + std::to_string(port));
    try {
        transport_.sendTo(makeAnnouncementPayload(kDiscoveryAnnouncementTypeReply),
                          address,
                          port);
        logDiagnostic("service.send_reply.success address=" + address
                      + " port=" + std::to_string(port));
    } catch (const std::exception& error) {
        logDiagnostic("service.send_reply.failed address=" + address
                      + " port=" + std::to_string(port)
                      + " error=" + error.what());
        throw;
    }
}

void DiscoveryService::sendOfflineTo(const std::string& address,
                                     std::uint16_t port)
{
    logDiagnostic("service.send_offline.begin address=" + address
                  + " port=" + std::to_string(port));
    try {
        transport_.sendTo(
            makeAnnouncementPayload(kDiscoveryAnnouncementTypeOffline),
            address,
            port);
        logDiagnostic("service.send_offline.success address=" + address
                      + " port=" + std::to_string(port));
    } catch (const std::exception& error) {
        logDiagnostic("service.send_offline.failed address=" + address
                      + " port=" + std::to_string(port)
                      + " error=" + error.what());
        throw;
    }
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
        logDiagnostic("service.packet.invalid from=" + endpointText(packet.value())
                      + " error=" + error.what());
        return DiscoveryServicePollResult::InvalidPacket(error.what());
    }

    logDiagnostic("service.packet.valid from=" + endpointText(packet.value())
                  + " type=" + announcement.GetType()
                  + " timestamp=" + announcement.GetTimestamp()
                  + " device_id=" + announcement.GetDeviceId()
                  + " host_name=" + announcement.GetHostName()
                  + " display_name=" + announcement.GetDisplayName());
    const DiscoveryProcessResult processResult =
        processDiscoveryAnnouncement(appPaths_,
                                     localIdentity_.GetDeviceId(),
                                     announcement,
                                     packet->GetObservedAddress());
    const DiscoveryServicePollResult result =
        fromProcessResult(processResult, announcement, packet.value());
    logDiagnostic("service.packet.result action="
                  + pollActionText(result.GetAction())
                  + " type=" + announcement.GetType()
                  + " timestamp=" + announcement.GetTimestamp()
                  + " device_id=" + announcement.GetDeviceId()
                  + " peer_created="
                  + std::to_string(processResult.GetPeerCreated())
                  + " from=" + endpointText(packet.value()));
    return result;
}

void DiscoveryService::close()
{
    logDiagnostic("service.close");
    transport_.close();
}

void DiscoveryService::logDiagnostic(std::string message) const
{
    (void)message;
    if constexpr (discoveryTraceEnabled()) {
        relaydesk::core::appendDiagnosticLogLine(
            makeDiscoveryLogFilePath(appPaths_),
            message);
    }
}

std::string DiscoveryService::makeAnnouncementPayload(
    std::string announcementType) const
{
    DiscoveryAnnouncement announcement = makeLocalDiscoveryAnnouncement(
        localIdentity_,
        config_.GetAdvertisedTcpPort(),
        config_.GetCapabilities(),
        relaydesk::core::currentUtcTimestamp());
    announcement.SetType(std::move(announcementType));
    announcement.SetAppVersion(config_.GetAppVersion());
    return serializeDiscoveryAnnouncement(announcement);
}

}
