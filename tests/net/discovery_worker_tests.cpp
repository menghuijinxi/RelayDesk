#include "net/discovery_worker.h"

#include "net/boost_asio_udp_discovery_transport.h"
#include "storage/peer_profile.h"

#include <chrono>
#include <filesystem>
#include <iostream>
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

relaydesk::net::DiscoveryServiceConfig makeServiceConfig()
{
    relaydesk::net::DiscoveryServiceConfig config;
    config.SetDiscoveryUdpPort(0);
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

relaydesk::net::DiscoveryService makeService(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& deviceId,
    const std::string& displayName)
{
    return relaydesk::net::DiscoveryService(appPaths,
                                            makeIdentity(deviceId, displayName),
                                            makeServiceConfig());
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

    if (const int result = recordsInvalidPacketInBackground(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidWorkerConfig(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
