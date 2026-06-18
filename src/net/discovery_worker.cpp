#include "net/discovery_worker.h"

#include "net/discovery_message.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace relaydesk::net {

DiscoveryWorker::DiscoveryWorker(DiscoveryService discoveryService,
                                 DiscoveryWorkerConfig workerConfig,
                                 DiscoveryWorkerEvents events)
    : discoveryService_(std::move(discoveryService)),
      localUdpPort_(discoveryService_.GetLocalUdpPort()),
      workerConfig_(workerConfig),
      events_(std::move(events))
{
    validateWorkerConfig();
}

DiscoveryWorker::~DiscoveryWorker()
{
    stop();
}

std::uint16_t DiscoveryWorker::GetLocalUdpPort() const
{
    return localUdpPort_;
}

bool DiscoveryWorker::IsRunning() const
{
    return thread_.joinable();
}

DiscoveryWorkerStats DiscoveryWorker::GetStats() const
{
    std::lock_guard lock(mutex_);
    return stats_;
}

void DiscoveryWorker::start()
{
    if (thread_.joinable()) {
        return;
    }

    stopping_.store(false);
    discoveryService_.logDiagnostic("worker.start_requested");
    thread_ = std::jthread([this](std::stop_token stopToken) {
        run(stopToken);
    });
}

void DiscoveryWorker::stop()
{
    if (!thread_.joinable()) {
        return;
    }

    discoveryService_.logDiagnostic("worker.stop_requested");
    stopping_.store(true);
    discoveryService_.logDiagnostic("worker.stop.stopping_flag_set");
    thread_.request_stop();
    discoveryService_.logDiagnostic("worker.stop.join_begin");
    thread_.join();
    discoveryService_.logDiagnostic("worker.stop.joined");
    if (workerConfig_.GetBroadcastEnabled()) {
        try {
            discoveryService_.logDiagnostic("worker.stop.offline_broadcast_begin");
            discoveryService_.broadcastOfflineNow();
            recordBroadcast();
            discoveryService_.logDiagnostic("worker.stop.offline_broadcast_done");
        } catch (const std::exception& error) {
            discoveryService_.logDiagnostic(
                std::string("worker.offline_broadcast_error error=")
                + error.what());
            recordError(error.what());
        }
    }
    discoveryService_.close();
    discoveryService_.logDiagnostic("worker.stop.closed");
}

void DiscoveryWorker::updateLocalIdentity(
    relaydesk::storage::LocalIdentity localIdentity)
{
    discoveryService_.updateLocalIdentity(std::move(localIdentity));
}

void DiscoveryWorker::run(std::stop_token stopToken)
{
    discoveryService_.logDiagnostic(
        "worker.run broadcast_enabled="
        + std::to_string(workerConfig_.GetBroadcastEnabled())
        + " announce_on_start="
        + std::to_string(workerConfig_.GetAnnounceOnStart())
        + " broadcast_interval_ms="
        + std::to_string(workerConfig_.GetBroadcastInterval().count())
        + " startup_interval_ms="
        + std::to_string(workerConfig_.GetStartupBroadcastInterval().count())
        + " startup_count="
        + std::to_string(workerConfig_.GetStartupBroadcastCount())
        + " poll_timeout_ms="
        + std::to_string(workerConfig_.GetPollTimeout().count()));
    auto nextBroadcastAt = std::chrono::steady_clock::now();
    int startupBroadcastsLeft = workerConfig_.GetStartupBroadcastCount();
    if (!workerConfig_.GetAnnounceOnStart()) {
        nextBroadcastAt += workerConfig_.GetBroadcastInterval();
        startupBroadcastsLeft = 0;
    }

    while (!stopToken.stop_requested()) {
        try {
            const auto now = std::chrono::steady_clock::now();
            if (workerConfig_.GetBroadcastEnabled()
                && !isStopping()
                && now >= nextBroadcastAt) {
                discoveryService_.broadcastNow();
                recordBroadcast();
                if (startupBroadcastsLeft > 0) {
                    --startupBroadcastsLeft;
                }
                if (startupBroadcastsLeft > 0) {
                    nextBroadcastAt =
                        now + workerConfig_.GetStartupBroadcastInterval();
                } else {
                    nextBroadcastAt = now + workerConfig_.GetBroadcastInterval();
                }
            }

            const DiscoveryServicePollResult result =
                discoveryService_.pollOnce(workerConfig_.GetPollTimeout());
            recordPollResult(result);
            notifyStoredPeer(result);
            if (workerConfig_.GetBroadcastEnabled()
                && !isStopping()
                && result.GetAction() == DiscoveryServicePollAction::StoredPeer
                && result.GetAnnouncementType() == kDiscoveryAnnouncementTypeHello) {
                discoveryService_.logDiagnostic(
                    "worker.reply_to_hello address="
                    + result.GetObservedAddress()
                    + " port=" + std::to_string(result.GetObservedPort()));
                discoveryService_.sendReplyTo(result.GetObservedAddress(),
                                              result.GetObservedPort());
                recordReply();
            } else if (result.GetAction()
                       == DiscoveryServicePollAction::StoredPeer) {
                discoveryService_.logDiagnostic(
                    "worker.no_reply action=stored_peer type="
                    + result.GetAnnouncementType()
                    + " stopping=" + std::to_string(isStopping()));
            }
        } catch (const std::exception& error) {
            discoveryService_.logDiagnostic(
                std::string("worker.error error=") + error.what());
            recordError(error.what());
            std::this_thread::sleep_for(workerConfig_.GetPollTimeout());
        }
    }

    discoveryService_.logDiagnostic("worker.run_exit");
}

bool DiscoveryWorker::isStopping() const
{
    return stopping_.load();
}

void DiscoveryWorker::validateWorkerConfig() const
{
    if (workerConfig_.GetBroadcastInterval().count() <= 0) {
        throw std::invalid_argument("Discovery broadcast interval must be positive.");
    }

    if (workerConfig_.GetStartupBroadcastInterval().count() <= 0) {
        throw std::invalid_argument(
            "Discovery startup broadcast interval must be positive.");
    }

    if (workerConfig_.GetStartupBroadcastCount() < 0) {
        throw std::invalid_argument(
            "Discovery startup broadcast count cannot be negative.");
    }

    if (workerConfig_.GetPollTimeout().count() <= 0) {
        throw std::invalid_argument("Discovery poll timeout must be positive.");
    }
}

void DiscoveryWorker::recordBroadcast()
{
    std::lock_guard lock(mutex_);
    stats_.SetBroadcastCount(stats_.GetBroadcastCount() + 1);
}

void DiscoveryWorker::recordReply()
{
    std::lock_guard lock(mutex_);
    stats_.SetReplyCount(stats_.GetReplyCount() + 1);
}

void DiscoveryWorker::recordPollResult(const DiscoveryServicePollResult& result)
{
    std::lock_guard lock(mutex_);
    switch (result.GetAction()) {
    case DiscoveryServicePollAction::NoPacket:
        break;
    case DiscoveryServicePollAction::InvalidPacket:
        stats_.SetInvalidPacketCount(stats_.GetInvalidPacketCount() + 1);
        stats_.SetLastErrorMessage(result.GetErrorMessage());
        break;
    case DiscoveryServicePollAction::IgnoredSelf:
        stats_.SetIgnoredSelfCount(stats_.GetIgnoredSelfCount() + 1);
        break;
    case DiscoveryServicePollAction::StoredPeer:
        stats_.SetStoredPeerCount(stats_.GetStoredPeerCount() + 1);
        break;
    }
}

void DiscoveryWorker::notifyStoredPeer(const DiscoveryServicePollResult& result)
{
    const auto& peerStoredCallback = events_.GetPeerStoredCallback();
    if (!peerStoredCallback
        || result.GetAction() != DiscoveryServicePollAction::StoredPeer
        || !result.HasPeerProfile()) {
        return;
    }

    peerStoredCallback(result.GetPeerProfile().value(),
                       result.GetAnnouncementType());
}

void DiscoveryWorker::recordError(std::string errorMessage)
{
    std::lock_guard lock(mutex_);
    stats_.SetErrorCount(stats_.GetErrorCount() + 1);
    stats_.SetLastErrorMessage(std::move(errorMessage));
}

}
