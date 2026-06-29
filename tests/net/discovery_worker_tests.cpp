#include "net/discovery_worker.h"

#include "net/boost_asio_udp_discovery_transport.h"
#include "net/discovery_message.h"
#include "storage/peer_profile.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int expect(bool condition, const char* message)
{
    if (!condition) {
        return fail(message);
    }
    return 0;
}

std::filesystem::path testRoot()
{
    return std::filesystem::path(RELAYDESK_DISCOVERY_WORKER_TEST_WORK_DIR);
}

relaydesk::storage::AppPaths makeAppPaths(const std::filesystem::path& caseName)
{
    const std::filesystem::path workDirectory = testRoot() / caseName / "bin";
    std::filesystem::create_directories(workDirectory);
    return relaydesk::storage::AppPaths(workDirectory / "relaydesk.exe");
}

relaydesk::storage::LocalIdentity makeIdentity(const std::string& deviceId,
                                               const std::string& displayName)
{
    return relaydesk::storage::LocalIdentity(deviceId,
                                             deviceId + "-install",
                                             "2026-06-13T00:00:00Z",
                                             displayName + "-HOST",
                                             displayName);
}

relaydesk::net::DiscoveryServiceConfig makeServiceConfig(
    std::uint16_t discoveryUdpPort = 0)
{
    relaydesk::net::DiscoveryServiceConfig config;
    config.SetDiscoveryUdpPort(discoveryUdpPort);
    config.SetAdvertisedTcpPort(39171);
    config.SetCapabilities({"text", "file"});
    return config;
}

relaydesk::net::DiscoveryWorkerConfig makeWorkerConfig()
{
    relaydesk::net::DiscoveryWorkerConfig config;
    config.SetBroadcastEnabled(false);
    config.SetAnnounceOnStart(false);
    config.SetPollTimeout(10ms);
    config.SetBroadcastInterval(100ms);
    return config;
}

relaydesk::storage::PeerProfile makePeerProfile(const std::string& deviceId,
                                                const std::string& displayName)
{
    relaydesk::storage::PeerProfile profile;
    profile.SetDeviceId(deviceId);
    profile.SetHostName(displayName + "-HOST");
    profile.SetDisplayName(displayName);
    profile.SetLastAddresses({"127.0.0.1"});
    profile.SetTcpPort(39171);
    profile.SetCapabilities({"text", "file"});
    profile.SetFirstSeenAt("2026-06-13T00:00:00Z");
    profile.SetLastSeenAt("2026-06-13T00:00:00Z");
    return profile;
}

relaydesk::net::DiscoveryService makeService(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& deviceId,
    const std::string& displayName,
    std::uint16_t discoveryUdpPort = 0)
{
    return relaydesk::net::DiscoveryService(appPaths,
                                            makeIdentity(deviceId, displayName),
                                            makeServiceConfig(discoveryUdpPort));
}

std::uint16_t findUnusedDiscoveryPort()
{
    relaydesk::net::BoostAsioUdpDiscoveryTransport transport(0);
    const std::uint16_t port = transport.GetLocalPort();
    transport.close();
    return port;
}

bool waitForStoredPeer(relaydesk::net::DiscoveryWorker& worker)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.GetStats().GetStoredPeerCount() > 0) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool waitForStoredPeerCount(relaydesk::net::DiscoveryWorker& worker,
                            std::uint64_t expectedCount)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.GetStats().GetStoredPeerCount() >= expectedCount) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool waitForInvalidPacket(relaydesk::net::DiscoveryWorker& worker)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.GetStats().GetInvalidPacketCount() > 0) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool waitForNotifiedPeer(
    std::mutex& mutex,
    const std::optional<relaydesk::storage::PeerProfile>& notifiedPeer)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(mutex);
            if (notifiedPeer.has_value()) {
                return true;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool waitForServiceStoredPeer(relaydesk::net::DiscoveryService& service)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = service.pollOnce(50ms);
        if (result.GetAction()
            == relaydesk::net::DiscoveryServicePollAction::StoredPeer) {
            return true;
        }
    }
    return false;
}

bool waitForBroadcastCount(relaydesk::net::DiscoveryWorker& worker,
                           std::uint64_t expectedCount)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.GetStats().GetBroadcastCount() >= expectedCount) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

int startsAndStopsWorker()
{
    relaydesk::net::DiscoveryWorker worker(
        makeService(makeAppPaths("start-stop"), "local-device", "Local-PC"),
        makeWorkerConfig());

    worker.start();
    if (const int result = expect(worker.IsRunning(),
                                  "Discovery worker did not start.");
        result != 0) {
        return result;
    }
    if (const int result = expect(worker.GetLocalUdpPort() != 0,
                                  "Discovery worker local port was not cached.");
        result != 0) {
        worker.stop();
        return result;
    }

    worker.start();
    worker.stop();
    return expect(!worker.IsRunning(), "Discovery worker did not stop.");
}

int receivesPeerAnnouncementInBackground()
{
    const auto receiverPaths = makeAppPaths("receiver");
    relaydesk::net::DiscoveryWorker receiver(
        makeService(receiverPaths, "receiver-device", "Receiver-PC"),
        makeWorkerConfig());
    relaydesk::net::DiscoveryService sender =
        makeService(makeAppPaths("sender"), "sender-device", "Sender-PC");

    receiver.start();
    sender.sendAnnouncementTo("127.0.0.1", receiver.GetLocalUdpPort());

    if (const int result = expect(waitForStoredPeer(receiver),
                                  "Discovery worker did not store peer.");
        result != 0) {
        receiver.stop();
        return result;
    }
    receiver.stop();

    const auto loaded = relaydesk::storage::loadPeerProfile(receiverPaths,
                                                            "sender-device");
    return expect(loaded.GetDisplayName() == "Sender-PC",
                  "Discovery worker stored peer display name mismatch.");
}

int notifiesStoredPeerInMemory()
{
    std::mutex mutex;
    std::optional<relaydesk::storage::PeerProfile> notifiedPeer;
    std::optional<std::string> notifiedType;
    relaydesk::net::DiscoveryWorkerEvents events;
    events.SetPeerStoredCallback(
        [&mutex, &notifiedPeer, &notifiedType](
            relaydesk::storage::PeerProfile profile,
            std::string announcementType) {
            std::lock_guard lock(mutex);
            notifiedPeer = std::move(profile);
            notifiedType = std::move(announcementType);
        });

    relaydesk::net::DiscoveryWorker receiver(
        makeService(makeAppPaths("notify-receiver"), "receiver-device", "Receiver-PC"),
        makeWorkerConfig(),
        std::move(events));
    relaydesk::net::DiscoveryService sender =
        makeService(makeAppPaths("notify-sender"), "sender-device", "Sender-PC");

    receiver.start();
    sender.sendAnnouncementTo("127.0.0.1", receiver.GetLocalUdpPort());

    if (const int result = expect(waitForNotifiedPeer(mutex, notifiedPeer),
                                  "Discovery worker did not notify stored peer.");
        result != 0) {
        receiver.stop();
        return result;
    }
    receiver.stop();

    std::lock_guard lock(mutex);
    if (const int result = expect(notifiedPeer->GetDeviceId() == "sender-device",
                                  "Discovery worker stored peer notification mismatch.");
        result != 0) {
        return result;
    }

    return expect(notifiedType.value_or("")
                      == relaydesk::net::kDiscoveryAnnouncementTypeHello,
                  "Discovery worker stored peer notification type mismatch.");
}

int notifiesOfflinePeerInMemory()
{
    std::mutex mutex;
    std::optional<relaydesk::storage::PeerProfile> notifiedPeer;
    std::optional<std::string> notifiedType;
    relaydesk::net::DiscoveryWorkerEvents events;
    events.SetPeerStoredCallback(
        [&mutex, &notifiedPeer, &notifiedType](
            relaydesk::storage::PeerProfile profile,
            std::string announcementType) {
            std::lock_guard lock(mutex);
            notifiedPeer = std::move(profile);
            notifiedType = std::move(announcementType);
        });

    relaydesk::net::DiscoveryWorker receiver(
        makeService(makeAppPaths("offline-notify-receiver"),
                    "receiver-device",
                    "Receiver-PC"),
        makeWorkerConfig(),
        std::move(events));
    relaydesk::net::DiscoveryService sender =
        makeService(makeAppPaths("offline-notify-sender"),
                    "sender-device",
                    "Sender-PC");

    receiver.start();
    sender.sendOfflineTo("127.0.0.1", receiver.GetLocalUdpPort());

    if (const int result = expect(waitForNotifiedPeer(mutex, notifiedPeer),
                                  "Discovery worker did not notify offline peer.");
        result != 0) {
        receiver.stop();
        return result;
    }
    std::this_thread::sleep_for(100ms);
    receiver.stop();

    std::lock_guard lock(mutex);
    if (const int result = expect(notifiedPeer->GetDeviceId() == "sender-device",
                                  "Discovery worker offline peer notification mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(notifiedType.value_or("")
                                      == relaydesk::net::kDiscoveryAnnouncementTypeOffline,
                                  "Discovery worker offline notification type mismatch.");
        result != 0) {
        return result;
    }

    return expect(receiver.GetStats().GetReplyCount() == 0,
                  "Discovery worker should not reply to offline packet.");
}

int repliesAfterReceivingPeerAnnouncement()
{
    auto receiverConfig = makeWorkerConfig();
    receiverConfig.SetBroadcastEnabled(true);
    receiverConfig.SetBroadcastInterval(10s);

    relaydesk::net::DiscoveryWorker receiver(
        makeService(makeAppPaths("reply-receiver"), "receiver-device", "Receiver-PC"),
        receiverConfig);
    relaydesk::net::DiscoveryService sender =
        makeService(makeAppPaths("reply-sender"), "sender-device", "Sender-PC");

    receiver.start();
    sender.sendAnnouncementTo("127.0.0.1", receiver.GetLocalUdpPort());

    if (const int result = expect(waitForStoredPeer(receiver),
                                  "Discovery worker did not store peer before reply.");
        result != 0) {
        receiver.stop();
        return result;
    }

    if (const int result = expect(waitForServiceStoredPeer(sender),
                                  "Discovery worker reply was not received.");
        result != 0) {
        receiver.stop();
        return result;
    }

    if (const int result = expect(receiver.GetStats().GetReplyCount() > 0,
                                  "Discovery worker reply was not recorded.");
        result != 0) {
        receiver.stop();
        return result;
    }

    receiver.stop();
    return 0;
}

int rateLimitsRepeatedHelloReplies()
{
    auto receiverConfig = makeWorkerConfig();
    receiverConfig.SetBroadcastEnabled(true);
    receiverConfig.SetBroadcastInterval(10s);

    relaydesk::net::DiscoveryWorker receiver(
        makeService(makeAppPaths("rate-limit-reply-receiver"),
                    "receiver-device",
                    "Receiver-PC"),
        receiverConfig);
    relaydesk::net::DiscoveryService sender =
        makeService(makeAppPaths("rate-limit-reply-sender"),
                    "sender-device",
                    "Sender-PC");

    receiver.start();
    sender.sendAnnouncementTo("127.0.0.1", receiver.GetLocalUdpPort());

    if (const int result = expect(waitForServiceStoredPeer(sender),
                                  "Initial discovery hello did not get a reply.");
        result != 0) {
        receiver.stop();
        return result;
    }

    const std::uint64_t firstReplyCount = receiver.GetStats().GetReplyCount();
    sender.sendAnnouncementTo("127.0.0.1", receiver.GetLocalUdpPort());

    if (const int result = expect(waitForStoredPeerCount(receiver, 2),
                                  "Repeated discovery hello was not processed.");
        result != 0) {
        receiver.stop();
        return result;
    }

    const auto replyResult = sender.pollOnce(200ms);
    const std::uint64_t secondReplyCount = receiver.GetStats().GetReplyCount();
    receiver.stop();

    if (const int result = expect(replyResult.GetAnnouncementType()
                                      != relaydesk::net::kDiscoveryAnnouncementTypeReply,
                                  "Repeated discovery hello received a reply.");
        result != 0) {
        return result;
    }

    return expect(secondReplyCount == firstReplyCount,
                  "Repeated discovery hello recorded another reply.");
}

int repliesAfterReceivingKnownPeerAnnouncement()
{
    auto receiverConfig = makeWorkerConfig();
    receiverConfig.SetBroadcastEnabled(true);
    receiverConfig.SetBroadcastInterval(10s);

    const auto receiverPaths = makeAppPaths("known-reply-receiver");
    relaydesk::storage::savePeerProfile(
        receiverPaths,
        makePeerProfile("sender-device", "Sender-PC"));
    relaydesk::net::DiscoveryWorker receiver(
        makeService(receiverPaths, "receiver-device", "Receiver-PC"),
        receiverConfig);
    relaydesk::net::DiscoveryService sender =
        makeService(makeAppPaths("known-reply-sender"),
                    "sender-device",
                    "Sender-PC");

    receiver.start();
    sender.sendAnnouncementTo("127.0.0.1", receiver.GetLocalUdpPort());

    if (const int result = expect(waitForStoredPeer(receiver),
                                  "Known peer announcement was not processed.");
        result != 0) {
        receiver.stop();
        return result;
    }

    if (const int result = expect(waitForServiceStoredPeer(sender),
                                  "Known peer announcement did not get a reply.");
        result != 0) {
        receiver.stop();
        return result;
    }

    receiver.stop();
    return expect(receiver.GetStats().GetReplyCount() > 0,
                  "Known peer reply was not recorded.");
}

int doesNotReplyToReplyAnnouncement()
{
    auto receiverConfig = makeWorkerConfig();
    receiverConfig.SetBroadcastEnabled(true);
    receiverConfig.SetBroadcastInterval(10s);

    relaydesk::net::DiscoveryWorker receiver(
        makeService(makeAppPaths("reply-loop-receiver"),
                    "receiver-device",
                    "Receiver-PC"),
        receiverConfig);
    relaydesk::net::DiscoveryService sender =
        makeService(makeAppPaths("reply-loop-sender"),
                    "sender-device",
                    "Sender-PC");

    receiver.start();
    sender.sendReplyTo("127.0.0.1", receiver.GetLocalUdpPort());

    if (const int result = expect(waitForStoredPeer(receiver),
                                  "Reply announcement was not processed.");
        result != 0) {
        receiver.stop();
        return result;
    }

    std::this_thread::sleep_for(100ms);
    receiver.stop();
    return expect(receiver.GetStats().GetReplyCount() == 0,
                  "Discovery worker should not reply to a reply packet.");
}

int doesNotReplyAfterStopRequested()
{
    auto receiverConfig = makeWorkerConfig();
    receiverConfig.SetBroadcastEnabled(true);
    receiverConfig.SetAnnounceOnStart(false);
    receiverConfig.SetBroadcastInterval(10s);
    receiverConfig.SetPollTimeout(250ms);

    relaydesk::net::DiscoveryWorker receiver(
        makeService(makeAppPaths("stopping-reply-receiver"),
                    "receiver-device",
                    "Receiver-PC",
                    findUnusedDiscoveryPort()),
        receiverConfig);
    relaydesk::net::DiscoveryService sender =
        makeService(makeAppPaths("stopping-reply-sender"),
                    "sender-device",
                    "Sender-PC");

    receiver.start();
    std::this_thread::sleep_for(30ms);
    const std::uint16_t receiverPort = receiver.GetLocalUdpPort();
    std::thread stopper([&receiver] {
        receiver.stop();
    });
    std::this_thread::sleep_for(30ms);
    sender.sendAnnouncementTo("127.0.0.1", receiverPort);
    stopper.join();

    const auto replyResult = sender.pollOnce(200ms);
    if (const int result = expect(
            replyResult.GetAnnouncementType()
                != relaydesk::net::kDiscoveryAnnouncementTypeReply,
            "Discovery worker replied after stop was requested.");
        result != 0) {
        return result;
    }

    return expect(receiver.GetStats().GetReplyCount() == 0,
                  "Discovery worker recorded reply after stop was requested.");
}

int continuesBroadcastingAfterStartupBurst()
{
    auto config = makeWorkerConfig();
    config.SetBroadcastEnabled(true);
    config.SetAnnounceOnStart(true);
    config.SetStartupBroadcastCount(1);
    config.SetStartupBroadcastInterval(10ms);
    config.SetBroadcastInterval(50ms);

    relaydesk::net::DiscoveryWorker worker(
        makeService(makeAppPaths("continuous-broadcast"),
                    "local-device",
                    "Local-PC",
                    findUnusedDiscoveryPort()),
        config);

    worker.start();
    const bool broadcasted = waitForBroadcastCount(worker, 3);
    worker.stop();

    return expect(broadcasted,
                  "Discovery worker stopped broadcasting after startup burst.");
}

int broadcastsAfterManualFastDiscoveryRequest()
{
    auto config = makeWorkerConfig();
    config.SetBroadcastEnabled(true);
    config.SetAnnounceOnStart(false);
    config.SetStartupBroadcastCount(3);
    config.SetStartupBroadcastInterval(10ms);
    config.SetBroadcastInterval(10s);

    relaydesk::net::DiscoveryWorker worker(
        makeService(makeAppPaths("manual-fast-broadcast"),
                    "local-device",
                    "Local-PC",
                    findUnusedDiscoveryPort()),
        config);

    worker.start();
    std::this_thread::sleep_for(50ms);
    if (const int result = expect(worker.GetStats().GetBroadcastCount() == 0,
                                  "Manual discovery test broadcasted before request.");
        result != 0) {
        worker.stop();
        return result;
    }

    worker.requestFastBroadcast();
    const bool broadcasted = waitForBroadcastCount(worker, 3);
    worker.stop();

    return expect(broadcasted,
                  "Manual fast discovery request did not broadcast.");
}

int recordsInvalidPacketInBackground()
{
    relaydesk::net::DiscoveryWorker worker(
        makeService(makeAppPaths("invalid"), "local-device", "Local-PC"),
        makeWorkerConfig());
    relaydesk::net::BoostAsioUdpDiscoveryTransport sender(0);

    worker.start();
    sender.sendTo("{not-json}", "127.0.0.1", worker.GetLocalUdpPort());

    if (const int result = expect(waitForInvalidPacket(worker),
                                  "Discovery worker did not record invalid packet.");
        result != 0) {
        worker.stop();
        return result;
    }
    worker.stop();

    return expect(!worker.GetStats().GetLastErrorMessage().empty(),
                  "Discovery worker invalid packet should keep error message.");
}

int rejectsInvalidWorkerConfig()
{
    auto config = makeWorkerConfig();
    config.SetPollTimeout(0ms);

    try {
        relaydesk::net::DiscoveryWorker worker(
            makeService(makeAppPaths("invalid-config"), "local-device", "Local-PC"),
            config);
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Invalid discovery worker config was accepted.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = startsAndStopsWorker(); result != 0) {
        return result;
    }

    if (const int result = receivesPeerAnnouncementInBackground(); result != 0) {
        return result;
    }

    if (const int result = notifiesStoredPeerInMemory(); result != 0) {
        return result;
    }

    if (const int result = notifiesOfflinePeerInMemory(); result != 0) {
        return result;
    }

    if (const int result = repliesAfterReceivingPeerAnnouncement(); result != 0) {
        return result;
    }

    if (const int result = rateLimitsRepeatedHelloReplies(); result != 0) {
        return result;
    }

    if (const int result = repliesAfterReceivingKnownPeerAnnouncement();
        result != 0) {
        return result;
    }

    if (const int result = doesNotReplyToReplyAnnouncement(); result != 0) {
        return result;
    }

    if (const int result = doesNotReplyAfterStopRequested(); result != 0) {
        return result;
    }

    if (const int result = continuesBroadcastingAfterStartupBurst(); result != 0) {
        return result;
    }

    if (const int result = broadcastsAfterManualFastDiscoveryRequest(); result != 0) {
        return result;
    }

    if (const int result = recordsInvalidPacketInBackground(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidWorkerConfig(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
