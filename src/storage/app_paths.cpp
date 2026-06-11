#include "storage/app_paths.h"

#include <array>

namespace relaydesk::storage {

AppPaths::AppPaths(std::filesystem::path executablePath)
    : executablePath_(std::move(executablePath)),
      workDirectory_(executablePath_.parent_path()),
      dataDirectory_(workDirectory_ / "data"),
      logsDirectory_(dataDirectory_ / "logs"),
      peersDirectory_(dataDirectory_ / "peers"),
      transfersDirectory_(dataDirectory_ / "transfers"),
      inboxDirectory_(transfersDirectory_ / "inbox"),
      outboxDirectory_(transfersDirectory_ / "outbox"),
      tempTransfersDirectory_(transfersDirectory_ / "temp")
{
}

void ensureAppDirectories(const AppPaths& appPaths)
{
    const std::array directories{
        appPaths.GetDataDirectory(),
        appPaths.GetLogsDirectory(),
        appPaths.GetPeersDirectory(),
        appPaths.GetTransfersDirectory(),
        appPaths.GetInboxDirectory(),
        appPaths.GetOutboxDirectory(),
        appPaths.GetTempTransfersDirectory(),
    };

    for (const auto& directory : directories) {
        std::filesystem::create_directories(directory);
    }
}

}

