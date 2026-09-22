#include "net/discovery_message.h"

#include "storage/avatar_store.h"

#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace relaydesk::net {
namespace {

constexpr const char* kProtocol = "relaydesk.discovery";
constexpr int kVersion = 1;
constexpr std::uint16_t kMinTcpPort = 1;

void validateStringList(const std::vector<std::string>& values,
                        const char* fieldName)
{
    for (const auto& value : values) {
        if (value.empty()) {
            throw std::runtime_error(std::string("Discovery field contains empty value: ")
                                     + fieldName);
        }
    }
}

void validateAnnouncement(const DiscoveryAnnouncement& announcement)
{
    if (announcement.GetVersion() != kVersion
        || announcement.GetType().empty()
        || announcement.GetDeviceId().empty()
        || announcement.GetHostName().empty()
        || announcement.GetDisplayName().empty()
        || announcement.GetTcpPort() < kMinTcpPort
        || announcement.GetAppVersion() < 0
        || announcement.GetTimestamp().empty()) {
        throw std::runtime_error("Discovery announcement contains invalid fields.");
    }

    validateStringList(announcement.GetCapabilities(), "capabilities");
    if (announcement.GetAvatarSha256Specified()
        && !announcement.GetAvatarSha256().empty()
        && !relaydesk::storage::isAvatarSha256(announcement.GetAvatarSha256())) {
        throw std::runtime_error("Discovery announcement avatar hash is invalid.");
    }
}

std::string readRequiredString(const nlohmann::json& value, const char* fieldName)
{
    if (!value.contains(fieldName) || !value[fieldName].is_string()
        || value[fieldName].get<std::string>().empty()) {
        throw std::runtime_error("Discovery announcement is missing a string field.");
    }

    return value[fieldName].get<std::string>();
}

std::uint16_t readRequiredTcpPort(const nlohmann::json& value)
{
    if (!value.contains("tcp_port") || !value["tcp_port"].is_number_unsigned()) {
        throw std::runtime_error("Discovery announcement is missing tcp_port.");
    }

    const auto tcpPort = value["tcp_port"].get<unsigned int>();
    if (tcpPort > 65535u || tcpPort < kMinTcpPort) {
        throw std::runtime_error("Discovery announcement tcp_port is out of range.");
    }

    return static_cast<std::uint16_t>(tcpPort);
}

int readOptionalAppVersion(const nlohmann::json& value)
{
    if (!value.contains("app_version")) {
        return 0;
    }

    if (!value["app_version"].is_number_integer()) {
        throw std::runtime_error("Discovery announcement app_version is invalid.");
    }

    const int appVersion = value["app_version"].get<int>();
    if (appVersion < 0) {
        throw std::runtime_error("Discovery announcement app_version is invalid.");
    }
    return appVersion;
}

std::vector<std::string> readCapabilities(const nlohmann::json& value)
{
    if (!value.contains("capabilities") || !value["capabilities"].is_array()) {
        throw std::runtime_error("Discovery announcement is missing capabilities.");
    }

    std::vector<std::string> capabilities;
    for (const auto& capability : value["capabilities"]) {
        if (!capability.is_string() || capability.get<std::string>().empty()) {
            throw std::runtime_error("Discovery announcement capability is invalid.");
        }
        capabilities.push_back(capability.get<std::string>());
    }
    return capabilities;
}

nlohmann::json toJson(const DiscoveryAnnouncement& announcement)
{
    validateAnnouncement(announcement);
    nlohmann::json value{
        {"protocol", kProtocol},
        {"version", announcement.GetVersion()},
        {"type", announcement.GetType()},
        {"device_id", announcement.GetDeviceId()},
        {"host_name", announcement.GetHostName()},
        {"display_name", announcement.GetDisplayName()},
        {"tcp_port", announcement.GetTcpPort()},
        {"app_version", announcement.GetAppVersion()},
        {"capabilities", announcement.GetCapabilities()},
        {"timestamp", announcement.GetTimestamp()},
    };
    // 字段缺失表示旧版本；空字符串表示新版本主动恢复文字头像。
    if (announcement.GetAvatarSha256Specified()) {
        value["avatar_sha256"] = announcement.GetAvatarSha256();
    }
    return value;
}

DiscoveryAnnouncement fromJson(const nlohmann::json& value)
{
    if (!value.is_object()
        || value.value("protocol", "") != kProtocol
        || value.value("version", 0) != kVersion) {
        throw std::runtime_error("Discovery announcement has unsupported schema.");
    }

    DiscoveryAnnouncement announcement;
    announcement.SetVersion(value["version"].get<int>());
    announcement.SetType(readRequiredString(value, "type"));
    announcement.SetDeviceId(readRequiredString(value, "device_id"));
    announcement.SetHostName(readRequiredString(value, "host_name"));
    announcement.SetDisplayName(readRequiredString(value, "display_name"));
    announcement.SetTcpPort(readRequiredTcpPort(value));
    announcement.SetAppVersion(readOptionalAppVersion(value));
    announcement.SetCapabilities(readCapabilities(value));
    announcement.SetTimestamp(readRequiredString(value, "timestamp"));
    if (value.contains("avatar_sha256")) {
        if (!value["avatar_sha256"].is_string()) {
            throw std::runtime_error("Discovery announcement avatar hash is invalid.");
        }
        const std::string avatarSha256 = value["avatar_sha256"].get<std::string>();
        if (!avatarSha256.empty()
            && !relaydesk::storage::isAvatarSha256(avatarSha256)) {
            throw std::runtime_error("Discovery announcement avatar hash is invalid.");
        }
        announcement.SetAvatarSha256(avatarSha256);
    }
    validateAnnouncement(announcement);
    return announcement;
}

} // namespace

std::string serializeDiscoveryAnnouncement(const DiscoveryAnnouncement& announcement)
{
    return toJson(announcement).dump();
}

DiscoveryAnnouncement parseDiscoveryAnnouncement(const std::string& payload)
{
    try {
        return fromJson(nlohmann::json::parse(payload));
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Discovery announcement payload is invalid JSON.");
    }
}

}
