#include "storage/app_paths.h"
#include "storage/peer_profile.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
    return std::filesystem::path(RELAYDESK_PEER_PROFILE_TEST_WORK_DIR);
}

relaydesk::storage::AppPaths makeAppPaths(const std::filesystem::path& caseName)
{
    const std::filesystem::path workDirectory = testRoot() / caseName / "bin";
    std::filesystem::create_directories(workDirectory);
    return relaydesk::storage::AppPaths(workDirectory / "relaydesk.exe");
}

void writeJsonFile(const std::filesystem::path& filePath, const nlohmann::json& content)
{
    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to write test file.");
    }
    output << content.dump(4) << '\n';
}

relaydesk::storage::PeerProfile makeProfile(const std::string& deviceId)
{
    relaydesk::storage::PeerProfile profile;
    profile.SetDeviceId(deviceId);
    profile.SetHostName("DESKTOP-OFFICE-12");
    profile.SetDisplayName("Alice-PC");
    profile.SetLastAddresses({"192.168.1.42", "fe80::42"});
    profile.SetTcpPort(39171);
    profile.SetAppVersion(7);
    profile.SetCapabilities({"text", "emoji", "file", "folder"});
    profile.SetFirstSeenAt("2026-06-12T10:00:00Z");
    profile.SetLastSeenAt("2026-06-12T10:05:00Z");
    return profile;
}

int savesAndLoadsPeerProfile()
{
    const auto appPaths = makeAppPaths("round-trip");
    const auto profile = makeProfile("peer-device-1");
    relaydesk::storage::savePeerProfile(appPaths, profile);

    const auto loaded =
        relaydesk::storage::loadPeerProfile(appPaths, profile.GetDeviceId());
    if (const int result = expect(loaded.GetDeviceId() == "peer-device-1",
                                  "Peer device ID did not round-trip.");
        result != 0) {
        return result;
    }

    if (const int result = expect(loaded.GetDisplayName() == "Alice-PC",
                                  "Peer display name did not round-trip.");
        result != 0) {
        return result;
    }

    if (const int result = expect(loaded.GetLastAddresses().size() == 2,
                                  "Peer address list size mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(loaded.GetLastAddresses()[0] == "192.168.1.42",
                                  "Peer address list did not round-trip.");
        result != 0) {
        return result;
    }

    if (const int result = expect(loaded.GetTcpPort() == 39171,
                                  "Peer TCP port did not round-trip.");
        result != 0) {
        return result;
    }

    if (const int result = expect(loaded.GetAppVersion() == 7,
                                  "Peer app version did not round-trip.");
        result != 0) {
        return result;
    }

    return expect(loaded.GetCapabilities().size() == 4,
                  "Peer capabilities did not round-trip.");
}

int loadsLegacyPeerProfileWithoutAppVersion()
{
    const auto appPaths = makeAppPaths("legacy-version");
    const std::filesystem::path profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "peer-device-legacy");
    writeJsonFile(
        profilePath,
        nlohmann::json{
            {"schema_version", 1},
            {"device_id", "peer-device-legacy"},
            {"host_name", "DESKTOP-OFFICE-12"},
            {"display_name", "Alice-PC"},
            {"last_addresses", {"192.168.1.42"}},
            {"tcp_port", 39171},
            {"capabilities", {"text"}},
            {"first_seen_at", "2026-06-12T10:00:00Z"},
            {"last_seen_at", "2026-06-12T10:05:00Z"},
        });

    const auto loaded =
        relaydesk::storage::loadPeerProfile(appPaths, "peer-device-legacy");
    return expect(loaded.GetAppVersion() == 0,
                  "Legacy peer app version should default to zero.");
}

int updatesExistingPeerProfile()
{
    const auto appPaths = makeAppPaths("update");
    auto profile = makeProfile("peer-device-2");
    relaydesk::storage::savePeerProfile(appPaths, profile);

    profile.SetDisplayName("Alice-Renamed");
    profile.SetLastAddresses({"192.168.1.50"});
    profile.SetLastSeenAt("2026-06-12T10:30:00Z");
    relaydesk::storage::savePeerProfile(appPaths, profile);

    const auto loaded = relaydesk::storage::loadPeerProfile(appPaths, "peer-device-2");
    if (const int result = expect(loaded.GetDisplayName() == "Alice-Renamed",
                                  "Peer display name update was not saved.");
        result != 0) {
        return result;
    }

    if (const int result = expect(loaded.GetLastAddresses().size() == 1,
                                  "Peer address update left stale addresses.");
        result != 0) {
        return result;
    }

    return expect(loaded.GetLastSeenAt() == "2026-06-12T10:30:00Z",
                  "Peer last seen update was not saved.");
}

int listsSavedPeerProfilesInStableOrder()
{
    const auto appPaths = makeAppPaths("list");
    relaydesk::storage::savePeerProfile(appPaths, makeProfile("peer-device-b"));
    relaydesk::storage::savePeerProfile(appPaths, makeProfile("peer-device-a"));

    const std::vector<relaydesk::storage::PeerProfile> profiles =
        relaydesk::storage::loadPeerProfiles(appPaths);
    if (const int result = expect(profiles.size() == 2,
                                  "Peer profile list size mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(profiles[0].GetDeviceId() == "peer-device-a",
                                  "Peer profile list order is not stable.");
        result != 0) {
        return result;
    }

    return expect(profiles[1].GetDeviceId() == "peer-device-b",
                  "Peer profile list order is not stable.");
}

int rejectsInvalidPeerProfileFile()
{
    const auto appPaths = makeAppPaths("invalid");
    const std::filesystem::path profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "peer-device-3");
    writeJsonFile(profilePath, nlohmann::json{
        {"schema_version", 1},
        {"device_id", "peer-device-3"},
    });

    try {
        static_cast<void>(relaydesk::storage::loadPeerProfile(appPaths, "peer-device-3"));
    } catch (const std::exception&) {
        return 0;
    }

    return fail("Invalid peer profile file was accepted.");
}

int rejectsMismatchedPeerProfileDeviceId()
{
    const auto appPaths = makeAppPaths("mismatch");
    const std::filesystem::path profilePath =
        relaydesk::storage::getPeerProfileFilePath(appPaths, "peer-device-4");
    writeJsonFile(
        profilePath,
        nlohmann::json{
            {"schema_version", 1},
            {"device_id", "other-device"},
            {"host_name", "DESKTOP-OFFICE-12"},
            {"display_name", "Alice-PC"},
            {"last_addresses", {"192.168.1.42"}},
            {"tcp_port", 39171},
            {"capabilities", {"text"}},
            {"first_seen_at", "2026-06-12T10:00:00Z"},
            {"last_seen_at", "2026-06-12T10:05:00Z"},
        });

    try {
        static_cast<void>(relaydesk::storage::loadPeerProfile(appPaths, "peer-device-4"));
    } catch (const std::exception&) {
        return 0;
    }

    return fail("Mismatched peer profile device ID was accepted.");
}

int rejectsUnsafePeerDeviceId()
{
    const auto appPaths = makeAppPaths("unsafe-id");
    try {
        static_cast<void>(relaydesk::storage::getPeerProfileFilePath(appPaths,
                                                                     "../escape"));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Unsafe peer device ID was accepted.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = savesAndLoadsPeerProfile(); result != 0) {
        return result;
    }

    if (const int result = loadsLegacyPeerProfileWithoutAppVersion(); result != 0) {
        return result;
    }

    if (const int result = updatesExistingPeerProfile(); result != 0) {
        return result;
    }

    if (const int result = listsSavedPeerProfilesInStableOrder(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidPeerProfileFile(); result != 0) {
        return result;
    }

    if (const int result = rejectsMismatchedPeerProfileDeviceId(); result != 0) {
        return result;
    }

    if (const int result = rejectsUnsafePeerDeviceId(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
