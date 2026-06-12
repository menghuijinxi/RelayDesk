#pragma once

#include <filesystem>

namespace relaydesk::storage {

class AppPaths {
public:
    explicit AppPaths(std::filesystem::path executablePath);

    const std::filesystem::path& GetExecutablePath() const { return executablePath_; }
    const std::filesystem::path& GetWorkDirectory() const { return workDirectory_; }
    const std::filesystem::path& GetDataDirectory() const { return dataDirectory_; }
    const std::filesystem::path& GetIdentityFilePath() const { return identityFilePath_; }
    const std::filesystem::path& GetLogsDirectory() const { return logsDirectory_; }
    const std::filesystem::path& GetPeersDirectory() const { return peersDirectory_; }
    const std::filesystem::path& GetTransfersDirectory() const { return transfersDirectory_; }
    const std::filesystem::path& GetInboxDirectory() const { return inboxDirectory_; }
    const std::filesystem::path& GetOutboxDirectory() const { return outboxDirectory_; }
    const std::filesystem::path& GetTempTransfersDirectory() const
    {
        return tempTransfersDirectory_;
    }

protected:
    std::filesystem::path executablePath_;
    std::filesystem::path workDirectory_;
    std::filesystem::path dataDirectory_;
    std::filesystem::path identityFilePath_;
    std::filesystem::path logsDirectory_;
    std::filesystem::path peersDirectory_;
    std::filesystem::path transfersDirectory_;
    std::filesystem::path inboxDirectory_;
    std::filesystem::path outboxDirectory_;
    std::filesystem::path tempTransfersDirectory_;
};

AppPaths createAppPaths();
void ensureAppDirectories(const AppPaths& appPaths);

}
