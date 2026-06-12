#include "net/discovery_peer_profile.h"

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
    announcement.SetCapabilities({"text", "emoji", "file"});
    announcement.SetTimestamp(timestamp);
    return announcement;
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

    const auto loaded = relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    return expect(loaded.GetDisplayName() == "Alice-PC",
                  "Created peer profile was not persisted.");
}

int updatesPeerProfileAndPreservesFirstSeen()
{
    const auto appPaths = makeAppPaths("update");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        makeAnnouncement("2026-06-12T10:00:00Z"),
        "192.168.1.42"));

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

    if (const int result = expect(profile.GetLastAddresses()[0] == "192.168.1.50",
                                  "Latest observed address should be first.");
        result != 0) {
        return result;
    }

    return expect(profile.GetLastAddresses()[1] == "192.168.1.42",
                  "Previous observed address should be retained.");
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

    if (const int result = deduplicatesObservedAddress(); result != 0) {
        return result;
    }

    if (const int result = rejectsEmptyObservedAddress(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
