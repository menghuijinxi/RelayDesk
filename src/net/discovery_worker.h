#pragma once

#include "net/discovery_service.h"
#include "storage/peer_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
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
    std::chrono::milliseconds GetStartupBroadcastInterval() const
    {
        return startupBroadcastInterval_;
    }
    std::chrono::milliseconds GetPollTimeout() const { return pollTimeout_; }
    int GetStartupBroadcastCount() const { return startupBroadcastCount_; }
    bool GetBroadcastEnabled() const { return broadcastEnabled_; }
    bool GetAnnounceOnStart() const { return announceOnStart_; }

    void SetBroadcastInterval(std::chrono::milliseconds broadcastInterval)
    {
        broadcastInterval_ = broadcastInterval;
    }
    void SetStartupBroadcastInterval(
        std::chrono::milliseconds startupBroadcastInterval)
    {
        startupBroadcastInterval_ = startupBroadcastInterval;
    }
    void SetPollTimeout(std::chrono::milliseconds pollTimeout)
    {
        pollTimeout_ = pollTimeout;
    }
    void SetStartupBroadcastCount(int startupBroadcastCount)
    {
        startupBroadcastCount_ = startupBroadcastCount;
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
    std::chrono::milliseconds startupBroadcastInterval_{std::chrono::seconds(1)};
    std::chrono::milliseconds pollTimeout_{std::chrono::milliseconds(100)};
    int startupBroadcastCount_ = 3;
    bool broadcastEnabled_ = true;
    bool announceOnStart_ = true;
};

using PeerStoredCallback =
    std::function<void(relaydesk::storage::PeerProfile, std::string)>;

class DiscoveryWorkerEvents {
public:
    const PeerStoredCallback& GetPeerStoredCallback() const
    {
        return peerStoredCallback_;
    }
    void SetPeerStoredCallback(PeerStoredCallback peerStoredCallback)
    {
        peerStoredCallback_ = std::move(peerStoredCallback);
    }

protected:
    PeerStoredCallback peerStoredCallback_;
};

class DiscoveryWorkerStats {
public:
    std::uint64_t GetBroadcastCount() const { return broadcastCount_; }
    std::uint64_t GetStoredPeerCount() const { return storedPeerCount_; }
    std::uint64_t GetIgnoredSelfCount() const { return ignoredSelfCount_; }
    std::uint64_t GetInvalidPacketCount() const { return invalidPacketCount_; }
    std::uint64_t GetReplyCount() const { return replyCount_; }
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
    void SetReplyCount(std::uint64_t replyCount)
    {
        replyCount_ = replyCount;
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
    std::uint64_t replyCount_ = 0;
    std::uint64_t errorCount_ = 0;
    std::string lastErrorMessage_;
};

class DiscoveryWorker {
public:
    DiscoveryWorker(DiscoveryService discoveryService,
                    DiscoveryWorkerConfig workerConfig,
                    DiscoveryWorkerEvents events = {});
    ~DiscoveryWorker();

    DiscoveryWorker(const DiscoveryWorker&) = delete;
    DiscoveryWorker& operator=(const DiscoveryWorker&) = delete;

    std::uint16_t GetLocalUdpPort() const;
    bool IsRunning() const;
    DiscoveryWorkerStats GetStats() const;

    void start();
    void stop();
    void updateLocalIdentity(relaydesk::storage::LocalIdentity localIdentity);

protected:
    void run(std::stop_token stopToken);
    bool isStopping() const;
    void validateWorkerConfig() const;
    void recordBroadcast();
    void recordReply();
    void recordPollResult(const DiscoveryServicePollResult& result);
    void notifyStoredPeer(const DiscoveryServicePollResult& result);
    void recordError(std::string errorMessage);

    DiscoveryService discoveryService_;
    std::uint16_t localUdpPort_ = 0;
    DiscoveryWorkerConfig workerConfig_;
    DiscoveryWorkerEvents events_;
    mutable std::mutex mutex_;
    DiscoveryWorkerStats stats_;
    std::jthread thread_;
    std::atomic_bool stopping_ = false;
};

}
