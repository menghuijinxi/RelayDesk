#include "net/discovery_service.h"

#include "net/boost_asio_udp_discovery_transport.h"
#include "storage/peer_profile.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

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
    return std::filesystem::path(RELAYDESK_DISCOVERY_SERVICE_TEST_WORK_DIR);
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

relaydesk::net::DiscoveryServiceConfig makeTestConfig()
{
    relaydesk::net::DiscoveryServiceConfig config;
    config.SetDiscoveryUdpPort(0);
    config.SetAdvertisedTcpPort(39171);
    config.SetCapabilities({"text", "file"});
    return config;
}

int defaultPortsMatchProjectDecision()
{
    const relaydesk::net::DiscoveryServiceConfig config;

    if (const int result = expect(config.GetDiscoveryUdpPort() == 25581,
                                  "Default discovery UDP port mismatch.");
        result != 0) {
        return result;
    }

    return expect(config.GetDiscoveryUdpPort()
                      == relaydesk::net::kDefaultDiscoveryUdpPort,
                  "Default discovery UDP port constant mismatch.");
}

int sendsAnnouncementAndStoresPeer()
{
    const auto receiverPaths = makeAppPaths("receiver");
    relaydesk::net::DiscoveryService sender(
        makeAppPaths("sender"),
        makeIdentity("sender-device", "Sender-PC"),
        makeTestConfig());
    relaydesk::net::DiscoveryService receiver(
        receiverPaths,
        makeIdentity("receiver-device", "Receiver-PC"),
        makeTestConfig());

    sender.sendAnnouncementTo("127.0.0.1", receiver.GetLocalUdpPort());
    const auto result = receiver.pollOnce(500ms);

    if (const int check = expect(result.GetAction()
                                     == relaydesk::net::DiscoveryServicePollAction::StoredPeer,
                                 "Discovery service did not store peer.");
        check != 0) {
        return check;
    }

    if (const int check = expect(result.HasPeerProfile(),
                                 "Stored peer result should include profile.");
        check != 0) {
        return check;
    }

    const auto& profile = result.GetPeerProfile().value();
    if (const int check = expect(profile.GetDeviceId() == "sender-device",
                                 "Stored discovery peer device ID mismatch.");
        check != 0) {
        return check;
    }

    if (const int check = expect(profile.GetTcpPort() == 39171,
                                 "Stored discovery peer TCP port mismatch.");
        check != 0) {
        return check;
    }

    const auto loaded = relaydesk::storage::loadPeerProfile(receiverPaths,
                                                            "sender-device");
    return expect(loaded.GetLastAddresses()[0] == "127.0.0.1",
                  "Stored discovery peer observed address mismatch.");
}

int ignoresSelfAnnouncement()
{
    const auto appPaths = makeAppPaths("self");
    relaydesk::net::DiscoveryService service(
        appPaths,
        makeIdentity("local-device", "Local-PC"),
        makeTestConfig());

    service.sendAnnouncementTo("127.0.0.1", service.GetLocalUdpPort());
    const auto result = service.pollOnce(500ms);

    if (const int check = expect(result.GetAction()
                                     == relaydesk::net::DiscoveryServicePollAction::IgnoredSelf,
                                 "Self discovery announcement was not ignored.");
        check != 0) {
        return check;
    }

    const auto profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "local-device");
    return expect(!std::filesystem::exists(profilePath),
                  "Self discovery announcement should not create a peer profile.");
}

int returnsNoPacketOnTimeout()
{
    relaydesk::net::DiscoveryService service(
        makeAppPaths("timeout"),
        makeIdentity("local-device", "Local-PC"),
        makeTestConfig());

    const auto result = service.pollOnce(20ms);
    return expect(result.GetAction()
                      == relaydesk::net::DiscoveryServicePollAction::NoPacket,
                  "Discovery service timeout should return no packet.");
}

int reportsInvalidPacket()
{
    relaydesk::net::DiscoveryService service(
        makeAppPaths("invalid-packet"),
        makeIdentity("local-device", "Local-PC"),
        makeTestConfig());
    relaydesk::net::BoostAsioUdpDiscoveryTransport sender(0);

    sender.sendTo("{not-json}", "127.0.0.1", service.GetLocalUdpPort());
    const auto result = service.pollOnce(500ms);

    if (const int check = expect(result.GetAction()
                                     == relaydesk::net::DiscoveryServicePollAction::InvalidPacket,
                                 "Invalid discovery packet was not reported.");
        check != 0) {
        return check;
    }

    return expect(!result.GetErrorMessage().empty(),
                  "Invalid discovery packet should include an error message.");
}

int rejectsInvalidConfig()
{
    auto config = makeTestConfig();
    config.SetAdvertisedTcpPort(0);

    try {
        relaydesk::net::DiscoveryService service(
            makeAppPaths("invalid-config"),
            makeIdentity("local-device", "Local-PC"),
            config);
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Invalid discovery service config was accepted.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = defaultPortsMatchProjectDecision(); result != 0) {
        return result;
    }

    if (const int result = sendsAnnouncementAndStoresPeer(); result != 0) {
        return result;
    }

    if (const int result = ignoresSelfAnnouncement(); result != 0) {
        return result;
    }

    if (const int result = returnsNoPacketOnTimeout(); result != 0) {
        return result;
    }

    if (const int result = reportsInvalidPacket(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidConfig(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
