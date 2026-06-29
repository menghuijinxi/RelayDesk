#include "net/discovery_peer_profile.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
    return std::filesystem::path(RELAYDESK_DISCOVERY_TEST_WORK_DIR);
}

relaydesk::storage::AppPaths makeAppPaths(const std::filesystem::path& caseName)
{
    const std::filesystem::path workDirectory = testRoot() / caseName / "bin";
    std::filesystem::create_directories(workDirectory);
    return relaydesk::storage::AppPaths(workDirectory / "relaydesk.exe");
}

relaydesk::net::DiscoveryAnnouncement makeAnnouncement(const std::string& timestamp)
{
    relaydesk::net::DiscoveryAnnouncement announcement;
    announcement.SetDeviceId("peer-device");
    announcement.SetHostName("DESKTOP-OFFICE-12");
    announcement.SetDisplayName("Alice-PC");
    announcement.SetTcpPort(39171);
    announcement.SetAppVersion(3);
    announcement.SetCapabilities({"text", "emoji", "file"});
    announcement.SetTimestamp(timestamp);
    return announcement;
}

std::string readFileText(const std::filesystem::path& filePath)
{
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open test file.");
    }

    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

int createsPeerProfileFromDiscovery()
{
    const auto appPaths = makeAppPaths("create");
    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:00:00Z"),
        "192.168.1.42");

    if (const int result = expect(profile.GetDeviceId() == "peer-device",
                                  "Created peer device ID mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetFirstSeenAt() == "2026-06-12T10:00:00Z",
                                  "Created peer first seen mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetLastSeenAt() == "2026-06-12T10:00:00Z",
                                  "Created peer last seen mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetLastAddresses().size() == 1,
                                  "Created peer address count mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetLastAddresses()[0] == "192.168.1.42",
                                  "Created peer observed address mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetAppVersion() == 3,
                                  "Created peer app version mismatch.");
        result != 0) {
        return result;
    }

    const auto loaded = relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    if (const int result = expect(loaded.GetDisplayName() == "Alice-PC",
                                  "Created peer profile was not persisted.");
        result != 0) {
        return result;
    }
    return expect(loaded.GetAppVersion() == 3,
                  "Created peer app version was not persisted.");
}

int updatesPeerProfileAndPreservesFirstSeen()
{
    const auto appPaths = makeAppPaths("update");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:00:00Z"),
        "192.168.1.42"));
    auto existingProfile =
        relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    existingProfile.SetUnreadMessageCount(3);
    relaydesk::storage::savePeerProfile(appPaths, existingProfile);

    auto update = makeAnnouncement("2026-06-12T10:15:00Z");
    update.SetDisplayName("Alice-Renamed");
    update.SetTcpPort(40171);
    update.SetCapabilities({"text", "file", "folder"});
    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        update,
        "192.168.1.50");

    if (const int result = expect(profile.GetFirstSeenAt() == "2026-06-12T10:00:00Z",
                                  "Updated peer first seen should be preserved.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetLastSeenAt() == "2026-06-12T10:15:00Z",
                                  "Updated peer last seen mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetDisplayName() == "Alice-Renamed",
                                  "Updated peer display name mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetTcpPort() == 40171,
                                  "Updated peer TCP port mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetLastAddresses().size() == 2,
                                  "Updated peer address count mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetUnreadMessageCount() == 3,
                                  "Updated peer unread count should be preserved.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetLastAddresses()[0] == "192.168.1.50",
                                  "Latest observed address should be first.");
        result != 0) {
        return result;
    }

    return expect(profile.GetLastAddresses()[1] == "192.168.1.42",
                  "Previous observed address should be retained.");
}

int repairsCorruptPeerProfileFromDiscovery()
{
    const auto appPaths = makeAppPaths("repair-corrupt-profile");
    const std::filesystem::path profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "peer-device");
    std::filesystem::create_directories(profilePath.parent_path());
    {
        std::ofstream output(profilePath, std::ios::binary | std::ios::trunc);
        output << "{invalid json";
    }

    relaydesk::storage::PeerProfile profile;
    try {
        profile = relaydesk::net::upsertPeerProfileFromDiscovery(
            appPaths,
            makeAnnouncement("2026-06-12T10:20:00Z"),
            "192.168.1.77");
    } catch (const std::exception& exception) {
        std::cerr << "Corrupt peer profile repair threw: "
                  << exception.what() << '\n';
        return 1;
    }

    if (const int result = expect(profile.GetDeviceId() == "peer-device",
                                  "Corrupt peer profile was not repaired.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetLastAddresses().size() == 1,
                                  "Repaired peer address count mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetLastAddresses()[0] == "192.168.1.77",
                                  "Repaired peer observed address mismatch.");
        result != 0) {
        return result;
    }

    const auto loaded =
        relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    return expect(loaded.GetDisplayName() == "Alice-PC",
                  "Repaired peer profile was not persisted.");
}

int deduplicatesObservedAddress()
{
    const auto appPaths = makeAppPaths("deduplicate");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:00:00Z"),
        "192.168.1.42"));

    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:05:00Z"),
        "192.168.1.42");

    if (const int result = expect(profile.GetLastAddresses().size() == 1,
                                  "Duplicate observed address was retained.");
        result != 0) {
        return result;
    }

    return expect(profile.GetLastAddresses()[0] == "192.168.1.42",
                  "Observed address mismatch after deduplication.");
}

int keepsRepeatedHeartbeatInMemoryOnly()
{
    const auto appPaths = makeAppPaths("heartbeat-no-write");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:00:00Z"),
        "192.168.1.42"));
    const std::filesystem::path profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "peer-device");
    const std::string beforeHeartbeat = readFileText(profilePath);

    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:05:00Z"),
        "192.168.1.42");
    const std::string afterHeartbeat = readFileText(profilePath);

    if (const int result = expect(profile.GetLastSeenAt()
                                      == "2026-06-12T10:05:00Z",
                                  "Heartbeat should update in-memory last seen.");
        result != 0) {
        return result;
    }

    if (const int result = expect(beforeHeartbeat == afterHeartbeat,
                                  "Repeated heartbeat rewrote peer profile.");
        result != 0) {
        return result;
    }

    const auto persisted =
        relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    return expect(persisted.GetLastSeenAt() == "2026-06-12T10:00:00Z",
                  "Repeated heartbeat persisted last seen.");
}

int ignoresStaleAnnouncementTimestamp()
{
    const auto appPaths = makeAppPaths("stale-timestamp");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:10:00Z"),
        "192.168.1.42"));

    auto staleAnnouncement = makeAnnouncement("2026-06-12T10:05:00Z");
    staleAnnouncement.SetDisplayName("Alice-Old");
    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        staleAnnouncement,
        "192.168.1.50");

    if (const int result = expect(profile.GetLastSeenAt()
                                      == "2026-06-12T10:10:00Z",
                                  "Stale announcement moved last seen backward.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetDisplayName() == "Alice-PC",
                                  "Stale announcement overwrote display name.");
        result != 0) {
        return result;
    }

    const auto loaded = relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    if (const int result = expect(loaded.GetLastAddresses()[0] == "192.168.1.42",
                                  "Stale announcement overwrote observed address.");
        result != 0) {
        return result;
    }

    return expect(loaded.GetLastSeenAt() == "2026-06-12T10:10:00Z",
                  "Persisted peer profile accepted stale announcement.");
}

int acceptsHigherAppVersionWithStaleTimestamp()
{
    const auto appPaths = makeAppPaths("stale-version-upgrade");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:10:00Z"),
        "192.168.1.42"));

    auto update = makeAnnouncement("2026-06-12T10:05:00Z");
    update.SetAppVersion(4);
    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        update,
        "192.168.1.50");

    if (const int result = expect(profile.GetAppVersion() == 4,
                                  "Stale higher app version was ignored.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profile.GetLastSeenAt()
                                      == "2026-06-12T10:10:00Z",
                                  "Higher app version moved last seen backward.");
        result != 0) {
        return result;
    }

    const auto loaded = relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    if (const int result = expect(loaded.GetAppVersion() == 4,
                                  "Higher app version was not persisted.");
        result != 0) {
        return result;
    }

    return expect(loaded.GetLastAddresses()[0] == "192.168.1.50",
                  "Higher app version did not refresh observed address.");
}

int removesStaleProfileForSameHostAndAddress()
{
    const auto appPaths = makeAppPaths("stale-device-id");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:00:00Z"),
        "192.168.1.42"));

    auto stableAnnouncement = makeAnnouncement("2026-06-12T10:05:00Z");
    stableAnnouncement.SetDeviceId("windows-install-guid");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        stableAnnouncement,
        "192.168.1.42"));

    const auto staleProfilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "peer-device");
    if (const int result = expect(!std::filesystem::exists(staleProfilePath),
                                  "Stale peer profile was not removed.");
        result != 0) {
        return result;
    }

    const auto stableProfile =
        relaydesk::storage::loadPeerProfile(appPaths, "windows-install-guid");
    return expect(stableProfile.GetHostName() == "DESKTOP-OFFICE-12",
                  "Stable peer profile was not persisted.");
}

int rejectsEmptyObservedAddress()
{
    const auto appPaths = makeAppPaths("empty-address");
    try {
        static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
            appPaths,
            makeAnnouncement("2026-06-12T10:00:00Z"),
            ""));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Empty observed discovery address was accepted.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = createsPeerProfileFromDiscovery(); result != 0) {
        return result;
    }

    if (const int result = updatesPeerProfileAndPreservesFirstSeen(); result != 0) {
        return result;
    }

    if (const int result = repairsCorruptPeerProfileFromDiscovery(); result != 0) {
        return result;
    }

    if (const int result = deduplicatesObservedAddress(); result != 0) {
        return result;
    }

    if (const int result = keepsRepeatedHeartbeatInMemoryOnly(); result != 0) {
        return result;
    }

    if (const int result = ignoresStaleAnnouncementTimestamp(); result != 0) {
        return result;
    }

    if (const int result = acceptsHigherAppVersionWithStaleTimestamp();
        result != 0) {
        return result;
    }

    if (const int result = removesStaleProfileForSameHostAndAddress(); result != 0) {
        return result;
    }

    if (const int result = rejectsEmptyObservedAddress(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
