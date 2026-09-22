#include "storage/peer_profile.h"

#include "storage/avatar_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace relaydesk::storage {
namespace {

constexpr int kSchemaVersion = 1;
constexpr std::uint16_t kMinTcpPort = 1;

void validatePeerDeviceId(const std::string& peerDeviceId)
{
    const std::filesystem::path peerPath(peerDeviceId);
    if (peerDeviceId.empty()
        || peerDeviceId == "."
        || peerDeviceId == ".."
        || peerPath.is_absolute()
        || peerPath.has_root_name()
        || peerPath.has_root_directory()
        || peerPath.has_parent_path()) {
        throw std::invalid_argument("Peer device ID is not a safe directory name.");
    }
}

void validateStringList(const std::vector<std::string>& values,
                        const char* fieldName)
{
    for (const auto& value : values) {
        if (value.empty()) {
            throw std::runtime_error(std::string("Peer profile field contains empty value: ")
                                     + fieldName);
        }
    }
}

void validatePeerProfile(const PeerProfile& profile)
{
    validatePeerDeviceId(profile.GetDeviceId());
    if (profile.GetSchemaVersion() != kSchemaVersion
        || profile.GetHostName().empty()
        || profile.GetDisplayName().empty()
        || profile.GetTcpPort() < kMinTcpPort
        || profile.GetAppVersion() < 0
        || profile.GetUnreadMessageCount() < 0
        || profile.GetFirstSeenAt().empty()
        || profile.GetLastSeenAt().empty()
        || (!profile.GetAvatarSha256().empty()
            && !isAvatarSha256(profile.GetAvatarSha256()))) {
        throw std::runtime_error("Peer profile contains invalid required fields.");
    }

    validateStringList(profile.GetLastAddresses(), "last_addresses");
    validateStringList(profile.GetCapabilities(), "capabilities");
}

std::string readRequiredString(const nlohmann::json& value, const char* fieldName)
{
    if (!value.contains(fieldName) || !value[fieldName].is_string()
        || value[fieldName].get<std::string>().empty()) {
        throw std::runtime_error("Peer profile is missing a required string field.");
    }

    return value[fieldName].get<std::string>();
}

std::uint16_t readRequiredTcpPort(const nlohmann::json& value)
{
    if (!value.contains("tcp_port") || !value["tcp_port"].is_number_unsigned()) {
        throw std::runtime_error("Peer profile is missing tcp_port.");
    }

    const auto tcpPort = value["tcp_port"].get<unsigned int>();
    if (tcpPort > 65535u || tcpPort < kMinTcpPort) {
        throw std::runtime_error("Peer profile tcp_port is out of range.");
    }

    return static_cast<std::uint16_t>(tcpPort);
}

int readOptionalAppVersion(const nlohmann::json& value)
{
    if (!value.contains("app_version")) {
        return 0;
    }

    if (!value["app_version"].is_number_integer()) {
        throw std::runtime_error("Peer profile app_version is invalid.");
    }

    const int appVersion = value["app_version"].get<int>();
    if (appVersion < 0) {
        throw std::runtime_error("Peer profile app_version is invalid.");
    }
    return appVersion;
}

int readOptionalUnreadMessageCount(const nlohmann::json& value)
{
    if (!value.contains("unread_count")) {
        return 0;
    }

    if (!value["unread_count"].is_number_integer()) {
        throw std::runtime_error("Peer profile unread_count is invalid.");
    }

    const int unreadMessageCount = value["unread_count"].get<int>();
    if (unreadMessageCount < 0) {
        throw std::runtime_error("Peer profile unread_count is invalid.");
    }
    return unreadMessageCount;
}

std::string readOptionalAvatarSha256(const nlohmann::json& value)
{
    if (!value.contains("avatar_sha256")) {
        return {};
    }
    if (!value["avatar_sha256"].is_string()) {
        throw std::runtime_error("Peer profile avatar hash is invalid.");
    }

    const std::string avatarSha256 = value["avatar_sha256"].get<std::string>();
    if (!avatarSha256.empty() && !isAvatarSha256(avatarSha256)) {
        throw std::runtime_error("Peer profile avatar hash is invalid.");
    }
    return avatarSha256;
}

std::vector<std::string> readStringList(const nlohmann::json& value,
                                        const char* fieldName)
{
    if (!value.contains(fieldName) || !value[fieldName].is_array()) {
        throw std::runtime_error("Peer profile is missing a string list field.");
    }

    std::vector<std::string> result;
    for (const auto& item : value[fieldName]) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            throw std::runtime_error("Peer profile string list contains invalid value.");
        }
        result.push_back(item.get<std::string>());
    }
    return result;
}

nlohmann::json readProfileJson(const std::filesystem::path& filePath)
{
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open peer profile file for reading.");
    }

    try {
        return nlohmann::json::parse(input);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Peer profile file contains invalid JSON.");
    }
}

nlohmann::json toJson(const PeerProfile& profile)
{
    validatePeerProfile(profile);
    nlohmann::json value{
        {"schema_version", kSchemaVersion},
        {"device_id", profile.GetDeviceId()},
        {"host_name", profile.GetHostName()},
        {"display_name", profile.GetDisplayName()},
        {"last_addresses", profile.GetLastAddresses()},
        {"tcp_port", profile.GetTcpPort()},
        {"app_version", profile.GetAppVersion()},
        {"unread_count", profile.GetUnreadMessageCount()},
        {"capabilities", profile.GetCapabilities()},
        {"first_seen_at", profile.GetFirstSeenAt()},
        {"last_seen_at", profile.GetLastSeenAt()},
    };
    if (!profile.GetAvatarSha256().empty()) {
        value["avatar_sha256"] = profile.GetAvatarSha256();
    }
    return value;
}

PeerProfile fromJson(const nlohmann::json& value)
{
    if (!value.is_object()
        || value.value("schema_version", 0) != kSchemaVersion) {
        throw std::runtime_error("Peer profile has unsupported schema.");
    }

    PeerProfile profile;
    profile.SetDeviceId(readRequiredString(value, "device_id"));
    profile.SetHostName(readRequiredString(value, "host_name"));
    profile.SetDisplayName(readRequiredString(value, "display_name"));
    profile.SetLastAddresses(readStringList(value, "last_addresses"));
    profile.SetTcpPort(readRequiredTcpPort(value));
    profile.SetAppVersion(readOptionalAppVersion(value));
    profile.SetUnreadMessageCount(readOptionalUnreadMessageCount(value));
    profile.SetCapabilities(readStringList(value, "capabilities"));
    profile.SetFirstSeenAt(readRequiredString(value, "first_seen_at"));
    profile.SetLastSeenAt(readRequiredString(value, "last_seen_at"));
    profile.SetAvatarSha256(readOptionalAvatarSha256(value));
    validatePeerProfile(profile);
    return profile;
}

} // namespace

std::filesystem::path getPeerProfileFilePath(const AppPaths& appPaths,
                                             const std::string& peerDeviceId)
{
    validatePeerDeviceId(peerDeviceId);
    return appPaths.GetPeersDirectory() / peerDeviceId / "profile.json";
}

void savePeerProfile(const AppPaths& appPaths, const PeerProfile& profile)
{
    const std::filesystem::path profileFilePath =
        getPeerProfileFilePath(appPaths, profile.GetDeviceId());
    std::filesystem::create_directories(profileFilePath.parent_path());

    std::ofstream output(profileFilePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open peer profile file for writing.");
    }

    output << toJson(profile).dump(4) << '\n';
    if (!output) {
        throw std::runtime_error("Failed to write peer profile file.");
    }
}

PeerProfile loadPeerProfile(const AppPaths& appPaths, const std::string& peerDeviceId)
{
    const PeerProfile profile =
        fromJson(readProfileJson(getPeerProfileFilePath(appPaths, peerDeviceId)));
    if (profile.GetDeviceId() != peerDeviceId) {
        throw std::runtime_error("Peer profile device ID does not match its directory.");
    }

    return profile;
}

std::vector<PeerProfile> loadPeerProfiles(const AppPaths& appPaths)
{
    if (!std::filesystem::exists(appPaths.GetPeersDirectory())) {
        return {};
    }

    std::vector<std::filesystem::path> profilePaths;
    for (const auto& entry : std::filesystem::directory_iterator(appPaths.GetPeersDirectory())) {
        if (entry.is_directory()) {
            const std::filesystem::path profilePath = entry.path() / "profile.json";
            if (std::filesystem::exists(profilePath)) {
                profilePaths.push_back(profilePath);
            }
        }
    }
    std::sort(profilePaths.begin(), profilePaths.end());

    std::vector<PeerProfile> profiles;
    for (const auto& profilePath : profilePaths) {
        try {
            profiles.push_back(loadPeerProfile(
                appPaths, profilePath.parent_path().filename().string()));
        } catch (const std::exception&) {
            continue;
        }
    }
    return profiles;
}

}
