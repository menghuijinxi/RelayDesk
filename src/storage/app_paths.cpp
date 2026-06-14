#include "storage/app_paths.h"

#include <array>
#include <utility>

namespace relaydesk::storage {

AppPaths::AppPaths(std::filesystem::path executablePath)
    : executablePath_(std::move(executablePath)),
      workDirectory_(executablePath_.parent_path()),
      dataDirectory_(workDirectory_ / "data"),
      configFilePath_(dataDirectory_ / "config.json"),
      identityFilePath_(dataDirectory_ / "identity.json"),
      logsDirectory_(dataDirectory_ / "logs"),
      peersDirectory_(dataDirectory_ / "peers"),
      stickersDirectory_(dataDirectory_ / "stickers"),
      favoriteStickersDirectory_(stickersDirectory_ / "favorites"),
      stickerPacksDirectory_(stickersDirectory_ / "packs"),
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
        appPaths.GetStickersDirectory(),
        appPaths.GetFavoriteStickersDirectory(),
        appPaths.GetStickerPacksDirectory(),
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
