#include "net/discovery_worker.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace relaydesk::net {

DiscoveryWorker::DiscoveryWorker(DiscoveryService discoveryService,
                                 DiscoveryWorkerConfig workerConfig)
    : discoveryService_(std::move(discoveryService)),
      workerConfig_(workerConfig)
{
    validateWorkerConfig();
}

DiscoveryWorker::~DiscoveryWorker()
{
    stop();
}

std::uint16_t DiscoveryWorker::GetLocalUdpPort() const
{
    return discoveryService_.GetLocalUdpPort();
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

    thread_ = std::jthread([this](std::stop_token stopToken) {
        run(stopToken);
    });
}

void DiscoveryWorker::stop()
{
    if (!thread_.joinable()) {
        return;
    }

    thread_.request_stop();
    thread_.join();
}

void DiscoveryWorker::run(std::stop_token stopToken)
{
    auto nextBroadcastAt = std::chrono::steady_clock::now();
    if (!workerConfig_.GetAnnounceOnStart()) {
        nextBroadcastAt += workerConfig_.GetBroadcastInterval();
    }

    while (!stopToken.stop_requested()) {
        try {
            const auto now = std::chrono::steady_clock::now();
            if (workerConfig_.GetBroadcastEnabled() && now >= nextBroadcastAt) {
                discoveryService_.broadcastNow();
                recordBroadcast();
                nextBroadcastAt = now + workerConfig_.GetBroadcastInterval();
            }

            recordPollResult(discoveryService_.pollOnce(workerConfig_.GetPollTimeout()));
        } catch (const std::exception& error) {
            recordError(error.what());
            std::this_thread::sleep_for(workerConfig_.GetPollTimeout());
        }
    }

    discoveryService_.close();
}

void DiscoveryWorker::validateWorkerConfig() const
{
    if (workerConfig_.GetBroadcastInterval().count() <= 0) {
        throw std::invalid_argument("Discovery broadcast interval must be positive.");
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

void DiscoveryWorker::recordError(std::string errorMessage)
{
    std::lock_guard lock(mutex_);
    stats_.SetErrorCount(stats_.GetErrorCount() + 1);
    stats_.SetLastErrorMessage(std::move(errorMessage));
}

}
