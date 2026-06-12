#include "net/discovery_worker.h"

#include "net/boost_asio_udp_discovery_transport.h"
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
    relaydesk::net::DiscoveryWorkerEvents events;
    events.SetPeerStoredCallback(
        [&mutex, &notifiedPeer](relaydesk::storage::PeerProfile profile) {
            std::lock_guard lock(mutex);
            notifiedPeer = std::move(profile);
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
    return expect(notifiedPeer->GetDeviceId() == "sender-device",
                  "Discovery worker stored peer notification mismatch.");
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

    if (const int result = repliesAfterReceivingPeerAnnouncement(); result != 0) {
        return result;
    }

    if (const int result = repliesAfterReceivingKnownPeerAnnouncement();
        result != 0) {
        return result;
    }

    if (const int result = doesNotReplyToReplyAnnouncement(); result != 0) {
        return result;
    }

    if (const int result = continuesBroadcastingAfterStartupBurst(); result != 0) {
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
