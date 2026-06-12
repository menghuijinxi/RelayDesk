#pragma once

#include "net/discovery_service.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace relaydesk::net {

class DiscoveryWorkerConfig {
public:
    std::chrono::milliseconds GetBroadcastInterval() const
    {
        return broadcastInterval_;
    }
    std::chrono::milliseconds GetPollTimeout() const { return pollTimeout_; }
    bool GetBroadcastEnabled() const { return broadcastEnabled_; }
    bool GetAnnounceOnStart() const { return announceOnStart_; }

    void SetBroadcastInterval(std::chrono::milliseconds broadcastInterval)
    {
        broadcastInterval_ = broadcastInterval;
    }
    void SetPollTimeout(std::chrono::milliseconds pollTimeout)
    {
        pollTimeout_ = pollTimeout;
    }
    void SetBroadcastEnabled(bool broadcastEnabled)
    {
        broadcastEnabled_ = broadcastEnabled;
    }
    void SetAnnounceOnStart(bool announceOnStart)
    {
        announceOnStart_ = announceOnStart;
    }

protected:
    std::chrono::milliseconds broadcastInterval_{std::chrono::seconds(5)};
    std::chrono::milliseconds pollTimeout_{std::chrono::milliseconds(100)};
    bool broadcastEnabled_ = true;
    bool announceOnStart_ = true;
};

class DiscoveryWorkerStats {
public:
    std::uint64_t GetBroadcastCount() const { return broadcastCount_; }
    std::uint64_t GetStoredPeerCount() const { return storedPeerCount_; }
    std::uint64_t GetIgnoredSelfCount() const { return ignoredSelfCount_; }
    std::uint64_t GetInvalidPacketCount() const { return invalidPacketCount_; }
    std::uint64_t GetErrorCount() const { return errorCount_; }
    const std::string& GetLastErrorMessage() const { return lastErrorMessage_; }

    void SetBroadcastCount(std::uint64_t broadcastCount)
    {
        broadcastCount_ = broadcastCount;
    }
    void SetStoredPeerCount(std::uint64_t storedPeerCount)
    {
        storedPeerCount_ = storedPeerCount;
    }
    void SetIgnoredSelfCount(std::uint64_t ignoredSelfCount)
    {
        ignoredSelfCount_ = ignoredSelfCount;
    }
    void SetInvalidPacketCount(std::uint64_t invalidPacketCount)
    {
        invalidPacketCount_ = invalidPacketCount;
    }
    void SetErrorCount(std::uint64_t errorCount)
    {
        errorCount_ = errorCount;
    }
    void SetLastErrorMessage(std::string lastErrorMessage)
    {
        lastErrorMessage_ = std::move(lastErrorMessage);
    }

protected:
    std::uint64_t broadcastCount_ = 0;
    std::uint64_t storedPeerCount_ = 0;
    std::uint64_t ignoredSelfCount_ = 0;
    std::uint64_t invalidPacketCount_ = 0;
    std::uint64_t errorCount_ = 0;
    std::string lastErrorMessage_;
};

class DiscoveryWorker {
public:
    DiscoveryWorker(DiscoveryService discoveryService,
                    DiscoveryWorkerConfig workerConfig);
    ~DiscoveryWorker();

    DiscoveryWorker(const DiscoveryWorker&) = delete;
    DiscoveryWorker& operator=(const DiscoveryWorker&) = delete;

    std::uint16_t GetLocalUdpPort() const;
    bool IsRunning() const;
    DiscoveryWorkerStats GetStats() const;

    void start();
    void stop();

protected:
    void run(std::stop_token stopToken);
    void validateWorkerConfig() const;
    void recordBroadcast();
    void recordPollResult(const DiscoveryServicePollResult& result);
    void recordError(std::string errorMessage);

    DiscoveryService discoveryService_;
    DiscoveryWorkerConfig workerConfig_;
    mutable std::mutex mutex_;
    DiscoveryWorkerStats stats_;
    std::jthread thread_;
};

}
