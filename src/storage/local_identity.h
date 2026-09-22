#pragma once

#include "storage/app_paths.h"

#include <string>
#include <utility>

namespace relaydesk::storage {

class LocalIdentity {
public:
    LocalIdentity(std::string deviceId,
                  std::string installId,
                  std::string createdAt,
                  std::string hostName,
                  std::string displayName);

    int GetSchemaVersion() const { return schemaVersion_; }
    const std::string& GetDeviceId() const { return deviceId_; }
    const std::string& GetInstallId() const { return installId_; }
    const std::string& GetCreatedAt() const { return createdAt_; }
    const std::string& GetHostName() const { return hostName_; }
    const std::string& GetDisplayName() const { return displayName_; }
    const std::string& GetAvatarSha256() const { return avatarSha256_; }
    void SetDeviceId(std::string deviceId) { deviceId_ = std::move(deviceId); }
    void SetHostName(std::string hostName) { hostName_ = std::move(hostName); }
    void SetDisplayName(std::string displayName) { displayName_ = std::move(displayName); }
    void SetAvatarSha256(std::string avatarSha256)
    {
        avatarSha256_ = std::move(avatarSha256);
    }

protected:
    int schemaVersion_ = 1;
    std::string deviceId_;
    std::string installId_;
    std::string createdAt_;
    std::string hostName_;
    std::string displayName_;
    std::string avatarSha256_;
};

LocalIdentity loadLocalIdentity(const AppPaths& appPaths);
void saveLocalIdentity(const AppPaths& appPaths, const LocalIdentity& identity);
LocalIdentity loadOrCreateLocalIdentity(const AppPaths& appPaths, const std::string& hostName);
LocalIdentity loadOrCreateLocalIdentity(const AppPaths& appPaths,
                                        const std::string& hostName,
                                        const std::string& stableDeviceId);
LocalIdentity updateLocalDisplayName(const AppPaths& appPaths,
                                     const std::string& displayName);
LocalIdentity updateLocalAvatarSha256(const AppPaths& appPaths,
                                      const std::string& avatarSha256);

}
