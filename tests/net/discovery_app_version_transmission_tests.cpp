#include "core/app_version.h"
#include "net/discovery_message.h"
#include "net/discovery_service.h"
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
    return std::filesystem::path(
        RELAYDESK_DISCOVERY_APP_VERSION_TEST_WORK_DIR);
}

relaydesk::storage::AppPaths makeAppPaths(
    const std::filesystem::path& caseName)
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
                                             "2026-06-18T00:00:00Z",
                                             displayName + "-HOST",
                                             displayName);
}

relaydesk::net::DiscoveryServiceConfig makeDefaultVersionTestConfig()
{
    relaydesk::net::DiscoveryServiceConfig config;
    config.SetDiscoveryUdpPort(0);
    config.SetAdvertisedTcpPort(39171);
    config.SetCapabilities({"text", "file"});
    return config;
}

class TestableDiscoveryService : public relaydesk::net::DiscoveryService {
public:
    using relaydesk::net::DiscoveryService::DiscoveryService;
    using relaydesk::net::DiscoveryService::makeAnnouncementPayload;
};

int defaultConfigUsesCurrentAppVersion()
{
    const relaydesk::net::DiscoveryServiceConfig config =
        makeDefaultVersionTestConfig();
    return expect(config.GetAppVersion() == relaydesk::core::kAppVersion,
                  "Default discovery config app version mismatch.");
}

int servicePayloadContainsCurrentAppVersion()
{
    TestableDiscoveryService service(
        makeAppPaths("payload"),
        makeIdentity("sender-device", "Sender-PC"),
        makeDefaultVersionTestConfig());

    const relaydesk::net::DiscoveryAnnouncement parsed =
        relaydesk::net::parseDiscoveryAnnouncement(
            service.makeAnnouncementPayload(
                relaydesk::net::kDiscoveryAnnouncementTypeHello));
    return expect(parsed.GetAppVersion() == relaydesk::core::kAppVersion,
                  "Discovery service payload app version mismatch.");
}

int udpPacketStoresCurrentAppVersion()
{
    const auto receiverPaths = makeAppPaths("receiver");
    relaydesk::net::DiscoveryService sender(
        makeAppPaths("sender"),
        makeIdentity("sender-device", "Sender-PC"),
        makeDefaultVersionTestConfig());
    relaydesk::net::DiscoveryService receiver(
        receiverPaths,
        makeIdentity("receiver-device", "Receiver-PC"),
        makeDefaultVersionTestConfig());

    sender.sendAnnouncementTo("127.0.0.1", receiver.GetLocalUdpPort());
    const auto result = receiver.pollOnce(500ms);

    if (const int check = expect(
            result.GetAction()
                == relaydesk::net::DiscoveryServicePollAction::StoredPeer,
            "Discovery service did not store versioned peer announcement.");
        check != 0) {
        return check;
    }

    if (const int check = expect(result.HasPeerProfile(),
                                 "Stored peer result should include profile.");
        check != 0) {
        return check;
    }

    const auto& profile = result.GetPeerProfile().value();
    if (const int check = expect(
            profile.GetAppVersion() == relaydesk::core::kAppVersion,
            "Stored peer app version mismatch.");
        check != 0) {
        return check;
    }

    const relaydesk::storage::PeerProfile loaded =
        relaydesk::storage::loadPeerProfile(receiverPaths, "sender-device");
    return expect(loaded.GetAppVersion() == relaydesk::core::kAppVersion,
                  "Persisted peer app version mismatch.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = defaultConfigUsesCurrentAppVersion(); result != 0) {
        return result;
    }

    if (const int result = servicePayloadContainsCurrentAppVersion();
        result != 0) {
        return result;
    }

    if (const int result = udpPacketStoresCurrentAppVersion(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
