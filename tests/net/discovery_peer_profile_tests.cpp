#include "net/discovery_peer_profile.h"

#include "storage/avatar_store.h"

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


std::string repeatingAvatarHash(char digit)
{
    return std::string(64, digit);
}

void writeTestBytes(const std::filesystem::path& path, const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to write avatar test file.");
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path avatarDirectoryFor(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& deviceId)
{
    return relaydesk::storage::peerAvatarDirectory(
        relaydesk::storage::getPeerProfileFilePath(
            appPaths,
            deviceId).parent_path());
}

int storesAvatarHashAndPrunesUnmatchedFiles()
{
    const auto appPaths = makeAppPaths("avatar-store");
    const std::string keepHash = repeatingAvatarHash('a');
    const std::string otherHash = repeatingAvatarHash('b');
    auto announcement = makeAnnouncement("2026-06-12T10:00:00Z");
    announcement.SetAvatarSha256(keepHash);
    const std::filesystem::path directory =
        avatarDirectoryFor(appPaths, "peer-device");
    const std::filesystem::path keepPath =
        relaydesk::storage::avatarImagePath(directory, keepHash);
    const std::filesystem::path otherPath =
        relaydesk::storage::avatarImagePath(directory, otherHash);
    const std::filesystem::path writingPath =
        directory / (otherHash + ".jpg.writing");
    writeTestBytes(keepPath, "keep");
    writeTestBytes(otherPath, "other");
    writeTestBytes(writingPath, "writing");

    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        announcement,
        "192.168.1.42");
    if (const int result = expect(profile.GetAvatarSha256() == keepHash,
                                  "Created peer avatar hash mismatch.");
        result != 0) {
        return result;
    }
    if (const int result = expect(std::filesystem::exists(keepPath),
                                  "Current avatar file was removed.");
        result != 0) {
        return result;
    }
    if (const int result = expect(!std::filesystem::exists(otherPath),
                                  "Unmatched avatar file was kept.");
        result != 0) {
        return result;
    }
    if (const int result = expect(std::filesystem::exists(writingPath),
                                  "In-progress avatar file was removed.");
        result != 0) {
        return result;
    }
    const auto loaded =
        relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    return expect(loaded.GetAvatarSha256() == keepHash,
                  "Avatar hash was not persisted.");
}

int preservesAvatarHashWhenAnnouncementOmitsField()
{
    const auto appPaths = makeAppPaths("avatar-preserve");
    const std::string keepHash = repeatingAvatarHash('a');
    auto initial = makeAnnouncement("2026-06-12T10:00:00Z");
    initial.SetAvatarSha256(keepHash);
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        initial,
        "192.168.1.42"));

    auto update = makeAnnouncement("2026-06-12T10:05:00Z");
    update.SetDisplayName("Alice-Next");
    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        update,
        "192.168.1.42");
    if (const int result = expect(profile.GetAvatarSha256() == keepHash,
                                  "Missing avatar field cleared the hash.");
        result != 0) {
        return result;
    }
    const auto loaded =
        relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    return expect(loaded.GetAvatarSha256() == keepHash
                      && loaded.GetDisplayName() == "Alice-Next",
                  "Missing avatar field was not preserved.");
}

int acceptsEqualTimestampAvatarHashChange()
{
    const auto appPaths = makeAppPaths("avatar-equal-timestamp");
    const std::string keepHash = repeatingAvatarHash('a');
    const std::string nextHash = repeatingAvatarHash('b');
    auto initial = makeAnnouncement("2026-06-12T10:00:00Z");
    initial.SetAvatarSha256(keepHash);
    const std::filesystem::path keepPath =
        relaydesk::storage::avatarImagePath(
            avatarDirectoryFor(appPaths, "peer-device"),
            keepHash);
    writeTestBytes(keepPath, "keep");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        initial,
        "192.168.1.42"));

    auto update = makeAnnouncement("2026-06-12T10:00:00Z");
    update.SetAvatarSha256(nextHash);
    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        update,
        "192.168.1.42");
    if (const int result = expect(profile.GetAvatarSha256() == nextHash,
                                  "Same-second avatar hash change was ignored.");
        result != 0) {
        return result;
    }
    const auto loaded =
        relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    if (const int result = expect(loaded.GetAvatarSha256() == nextHash,
                                  "Same-second avatar hash was not saved.");
        result != 0) {
        return result;
    }
    return expect(std::filesystem::exists(keepPath),
                  "Previous avatar was deleted before the replacement arrived.");
}

int ignoresOlderAvatarHashChange()
{
    const auto appPaths = makeAppPaths("avatar-stale-hash");
    const std::string keepHash = repeatingAvatarHash('a');
    auto initial = makeAnnouncement("2026-06-12T10:10:00Z");
    initial.SetAvatarSha256(keepHash);
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        initial,
        "192.168.1.42"));

    auto stale = makeAnnouncement("2026-06-12T10:05:00Z");
    stale.SetAvatarSha256(repeatingAvatarHash('b'));
    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        stale,
        "192.168.1.50");
    if (const int result = expect(profile.GetAvatarSha256() == keepHash,
                                  "Older avatar hash replaced the current hash.");
        result != 0) {
        return result;
    }
    const auto loaded =
        relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    return expect(loaded.GetAvatarSha256() == keepHash
                      && loaded.GetLastAddresses()[0] == "192.168.1.42",
                  "Older avatar announcement was persisted.");
}

int clearsAvatarHashAndPrunesFiles()
{
    const auto appPaths = makeAppPaths("avatar-clear");
    const std::string keepHash = repeatingAvatarHash('a');
    auto initial = makeAnnouncement("2026-06-12T10:00:00Z");
    initial.SetAvatarSha256(keepHash);
    const std::filesystem::path keepPath =
        relaydesk::storage::avatarImagePath(
            avatarDirectoryFor(appPaths, "peer-device"),
            keepHash);
    writeTestBytes(keepPath, "keep");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        initial,
        "192.168.1.42"));

    auto cleared = makeAnnouncement("2026-06-12T10:00:00Z");
    cleared.SetAvatarSha256("");
    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        cleared,
        "192.168.1.42");
    if (const int result = expect(profile.GetAvatarSha256().empty(),
                                  "Empty avatar hash did not clear the profile.");
        result != 0) {
        return result;
    }
    if (const int result = expect(!std::filesystem::exists(keepPath),
                                  "Cleared avatar file was kept.");
        result != 0) {
        return result;
    }
    const std::filesystem::path profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "peer-device");
    return expect(readFileText(profilePath).find("avatar_sha256")
                      == std::string::npos,
                  "Cleared avatar hash was still written.");
}

int keepsEmptyAvatarHeartbeatInMemoryOnly()
{
    const auto appPaths = makeAppPaths("avatar-empty-heartbeat");
    auto initial = makeAnnouncement("2026-06-12T10:00:00Z");
    initial.SetAvatarSha256("");
    static_cast<void>(relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        initial,
        "192.168.1.42"));
    const std::filesystem::path profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "peer-device");
    const std::string beforeHeartbeat = readFileText(profilePath);

    auto heartbeat = makeAnnouncement("2026-06-12T10:05:00Z");
    heartbeat.SetAvatarSha256("");
    const auto profile = relaydesk::net::upsertPeerProfileFromDiscovery(
        appPaths,
        heartbeat,
        "192.168.1.42");
    if (const int result = expect(profile.GetLastSeenAt()
                                      == "2026-06-12T10:05:00Z",
                                  "Empty avatar heartbeat did not update memory.");
        result != 0) {
        return result;
    }
    if (const int result = expect(beforeHeartbeat == readFileText(profilePath),
                                  "Empty avatar heartbeat rewrote the profile.");
        result != 0) {
        return result;
    }
    const auto loaded =
        relaydesk::storage::loadPeerProfile(appPaths, "peer-device");
    return expect(loaded.GetLastSeenAt() == "2026-06-12T10:00:00Z"
                      && loaded.GetAvatarSha256().empty(),
                  "Empty avatar heartbeat persisted last seen.");
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

    if (const int result = storesAvatarHashAndPrunesUnmatchedFiles();
        result != 0) {
        return result;
    }

    if (const int result = preservesAvatarHashWhenAnnouncementOmitsField();
        result != 0) {
        return result;
    }

    if (const int result = acceptsEqualTimestampAvatarHashChange();
        result != 0) {
        return result;
    }

    if (const int result = ignoresOlderAvatarHashChange(); result != 0) {
        return result;
    }

    if (const int result = clearsAvatarHashAndPrunesFiles(); result != 0) {
        return result;
    }

    if (const int result = keepsEmptyAvatarHeartbeatInMemoryOnly();
        result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
