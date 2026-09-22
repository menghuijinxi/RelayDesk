#include "storage/local_identity.h"

#include "storage/avatar_store.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/time.h"
#include "core/uuid.h"

namespace relaydesk::storage {
namespace {

constexpr int kSchemaVersion = 1;

int readSchemaVersion(const nlohmann::json& value)
{
    if (!value.is_object()
        || !value.contains("schema_version")
        || !value["schema_version"].is_number_integer()) {
        throw std::runtime_error("Local identity schema version is invalid.");
    }

    return value["schema_version"].get<int>();
}

std::string readRequiredString(const nlohmann::json& value, const char* fieldName)
{
    if (!value.contains(fieldName) || !value[fieldName].is_string()
        || value[fieldName].get<std::string>().empty()) {
        throw std::runtime_error("Local identity file is missing a required field.");
    }

    return value[fieldName].get<std::string>();
}

std::string readOptionalAvatarSha256(const nlohmann::json& value)
{
    if (!value.contains("avatar_sha256")) {
        return {};
    }
    if (!value["avatar_sha256"].is_string()) {
        throw std::runtime_error("Local identity avatar hash is invalid.");
    }

    const std::string avatarSha256 = value["avatar_sha256"].get<std::string>();
    if (!avatarSha256.empty() && !isAvatarSha256(avatarSha256)) {
        throw std::runtime_error("Local identity avatar hash is invalid.");
    }
    return avatarSha256;
}

nlohmann::json readIdentityJson(const std::filesystem::path& filePath)
{
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open local identity file for reading.");
    }

    try {
        return nlohmann::json::parse(input);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Local identity file contains invalid JSON.");
    }
}

void validateIdentity(const LocalIdentity& identity)
{
    if (identity.GetSchemaVersion() != kSchemaVersion
        || identity.GetDeviceId().empty()
        || identity.GetInstallId().empty()
        || identity.GetCreatedAt().empty()
        || identity.GetHostName().empty()
        || identity.GetDisplayName().empty()
        || (!identity.GetAvatarSha256().empty()
            && !isAvatarSha256(identity.GetAvatarSha256()))) {
        throw std::runtime_error("Local identity file contains invalid required fields.");
    }
}

std::string chooseDeviceId(const std::string& stableDeviceId)
{
    if (!stableDeviceId.empty()) {
        return stableDeviceId;
    }

    return relaydesk::core::createUuidV4();
}

LocalIdentity createLocalIdentity(const std::string& hostName,
                                  const std::string& stableDeviceId)
{
    if (hostName.empty()) {
        throw std::invalid_argument("Local identity host name cannot be empty.");
    }

    return LocalIdentity(chooseDeviceId(stableDeviceId),
                         relaydesk::core::createUuidV4(),
                         relaydesk::core::currentUtcTimestamp(),
                         hostName,
                         hostName);
}

} // namespace

LocalIdentity::LocalIdentity(std::string deviceId,
                             std::string installId,
                             std::string createdAt,
                             std::string hostName,
                             std::string displayName)
    : deviceId_(std::move(deviceId)),
      installId_(std::move(installId)),
      createdAt_(std::move(createdAt)),
      hostName_(std::move(hostName)),
      displayName_(std::move(displayName))
{
}

LocalIdentity loadLocalIdentity(const AppPaths& appPaths)
{
    const nlohmann::json value = readIdentityJson(appPaths.GetIdentityFilePath());
    const int schemaVersion = readSchemaVersion(value);
    if (schemaVersion != kSchemaVersion) {
        throw std::runtime_error("Local identity schema version is unsupported.");
    }

    LocalIdentity identity(
        readRequiredString(value, "device_id"),
        readRequiredString(value, "install_id"),
        readRequiredString(value, "created_at"),
        readRequiredString(value, "host_name"),
        readRequiredString(value, "display_name"));
    identity.SetAvatarSha256(readOptionalAvatarSha256(value));
    validateIdentity(identity);
    return identity;
}

void saveLocalIdentity(const AppPaths& appPaths, const LocalIdentity& identity)
{
    validateIdentity(identity);
    std::filesystem::create_directories(appPaths.GetDataDirectory());

    std::ofstream output(appPaths.GetIdentityFilePath(),
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open local identity file for writing.");
    }

    nlohmann::json value{
        {"schema_version", kSchemaVersion},
        {"device_id", identity.GetDeviceId()},
        {"install_id", identity.GetInstallId()},
        {"created_at", identity.GetCreatedAt()},
        {"host_name", identity.GetHostName()},
        {"display_name", identity.GetDisplayName()},
    };
    if (!identity.GetAvatarSha256().empty()) {
        value["avatar_sha256"] = identity.GetAvatarSha256();
    }
    output << value.dump(4) << '\n';

    if (!output) {
        throw std::runtime_error("Failed to write local identity file.");
    }
}

LocalIdentity loadOrCreateLocalIdentity(const AppPaths& appPaths, const std::string& hostName)
{
    return loadOrCreateLocalIdentity(appPaths, hostName, {});
}

LocalIdentity loadOrCreateLocalIdentity(const AppPaths& appPaths,
                                        const std::string& hostName,
                                        const std::string& stableDeviceId)
{
    if (!std::filesystem::exists(appPaths.GetIdentityFilePath())) {
        LocalIdentity identity = createLocalIdentity(hostName, stableDeviceId);
        saveLocalIdentity(appPaths, identity);
        return identity;
    }

    LocalIdentity identity = loadLocalIdentity(appPaths);
    bool changed = false;
    if (!stableDeviceId.empty() && identity.GetDeviceId() != stableDeviceId) {
        identity.SetDeviceId(stableDeviceId);
        changed = true;
    }

    if (identity.GetHostName() != hostName) {
        identity.SetHostName(hostName);
        changed = true;
    }

    if (changed) {
        saveLocalIdentity(appPaths, identity);
    }
    return identity;
}

LocalIdentity updateLocalDisplayName(const AppPaths& appPaths,
                                     const std::string& displayName)
{
    if (displayName.empty()) {
        throw std::invalid_argument("Local identity display name cannot be empty.");
    }

    LocalIdentity identity = loadLocalIdentity(appPaths);
    identity.SetDisplayName(displayName);
    saveLocalIdentity(appPaths, identity);
    return identity;
}

LocalIdentity updateLocalAvatarSha256(const AppPaths& appPaths,
                                      const std::string& avatarSha256)
{
    if (!avatarSha256.empty() && !isAvatarSha256(avatarSha256)) {
        throw std::invalid_argument("Local avatar hash is invalid.");
    }

    LocalIdentity identity = loadLocalIdentity(appPaths);
    identity.SetAvatarSha256(avatarSha256);
    saveLocalIdentity(appPaths, identity);
    return identity;
}

}
