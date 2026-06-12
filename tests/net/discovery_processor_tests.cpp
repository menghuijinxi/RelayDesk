#include "net/discovery_processor.h"
#include "storage/peer_profile.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

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
    return std::filesystem::path(RELAYDESK_DISCOVERY_PROCESSOR_TEST_WORK_DIR);
}

relaydesk::storage::AppPaths makeAppPaths(const std::filesystem::path& caseName)
{
    const std::filesystem::path workDirectory = testRoot() / caseName / "bin";
    std::filesystem::create_directories(workDirectory);
    return relaydesk::storage::AppPaths(workDirectory / "relaydesk.exe");
}

relaydesk::net::DiscoveryAnnouncement makeAnnouncement(
    const std::string& deviceId,
    const std::string& timestamp)
{
    relaydesk::net::DiscoveryAnnouncement announcement;
    announcement.SetDeviceId(deviceId);
    announcement.SetHostName("DESKTOP-OFFICE-12");
    announcement.SetDisplayName("Alice-PC");
    announcement.SetTcpPort(39171);
    announcement.SetCapabilities({"text", "file"});
    announcement.SetTimestamp(timestamp);
    return announcement;
}

int ignoresSelfAnnouncement()
{
    const auto appPaths = makeAppPaths("ignore-self");
    const auto result = relaydesk::net::processDiscoveryAnnouncement(
        appPaths,
        "local-device",
        makeAnnouncement("local-device", "2026-06-12T11:00:00Z"),
        "192.168.1.42");

    const bool ignoredSelf =
        result.GetAction() == relaydesk::net::DiscoveryProcessAction::IgnoredSelf;
    if (const int check = expect(ignoredSelf,
                                 "Self announcement should be ignored.");
        check != 0) {
        return check;
    }

    if (const int check = expect(!result.HasPeerProfile(),
                                 "Self announcement should not return a peer profile.");
        check != 0) {
        return check;
    }

    const auto profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "local-device");
    return expect(!std::filesystem::exists(profilePath),
                  "Self announcement should not persist a peer profile.");
}

int storesPeerAnnouncement()
{
    const auto appPaths = makeAppPaths("store-peer");
    const auto result = relaydesk::net::processDiscoveryAnnouncement(
        appPaths,
        "local-device",
        makeAnnouncement("peer-device", "2026-06-12T11:05:00Z"),
        "192.168.1.43");

    const bool storedPeer =
        result.GetAction() == relaydesk::net::DiscoveryProcessAction::StoredPeer;
    if (const int check = expect(storedPeer,
                                 "Peer announcement should be stored.");
        check != 0) {
        return check;
    }

    if (const int check = expect(result.HasPeerProfile(),
                                 "Peer announcement should return a peer profile.");
        check != 0) {
        return check;
    }

    const auto& profile = result.GetPeerProfile().value();
    if (const int check = expect(profile.GetDeviceId() == "peer-device",
                                 "Stored peer device ID mismatch.");
        check != 0) {
        return check;
    }

    const auto loaded = relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    return expect(loaded.GetLastAddresses()[0] == "192.168.1.43",
                  "Stored peer profile address mismatch.");
}

int rejectsEmptyLocalDeviceId()
{
    const auto appPaths = makeAppPaths("empty-local-device");
    try {
        static_cast<void>(relaydesk::net::processDiscoveryAnnouncement(
            appPaths,
            "",
            makeAnnouncement("peer-device", "2026-06-12T11:10:00Z"),
            "192.168.1.43"));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Empty local device ID was accepted.");
}

int propagatesPeerObservedAddressValidation()
{
    const auto appPaths = makeAppPaths("empty-peer-address");
    try {
        static_cast<void>(relaydesk::net::processDiscoveryAnnouncement(
            appPaths,
            "local-device",
            makeAnnouncement("peer-device", "2026-06-12T11:15:00Z"),
            ""));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Empty observed address for peer announcement was accepted.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = ignoresSelfAnnouncement(); result != 0) {
        return result;
    }

    if (const int result = storesPeerAnnouncement(); result != 0) {
        return result;
    }

    if (const int result = rejectsEmptyLocalDeviceId(); result != 0) {
        return result;
    }

    if (const int result = propagatesPeerObservedAddressValidation(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
