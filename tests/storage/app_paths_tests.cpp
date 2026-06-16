#include "storage/app_paths.h"

#include <filesystem>
#include <iostream>

namespace {

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

}

int main()
{
    const std::filesystem::path executablePath =
        LR"(C:\RelayDesk\bin\relaydesk.exe)";
    const relaydesk::storage::AppPaths appPaths(executablePath);

    if (appPaths.GetExecutablePath() != executablePath) {
        return fail("Executable path mismatch.");
    }

    if (appPaths.GetWorkDirectory() != LR"(C:\RelayDesk\bin)") {
        return fail("Work directory mismatch.");
    }

    if (appPaths.GetDataDirectory() != LR"(C:\RelayDesk\bin\data)") {
        return fail("Data directory mismatch.");
    }

    if (appPaths.GetConfigFilePath() != LR"(C:\RelayDesk\bin\data\config.json)") {
        return fail("Config file path mismatch.");
    }

    if (appPaths.GetIdentityFilePath() != LR"(C:\RelayDesk\bin\data\identity.json)") {
        return fail("Identity file path mismatch.");
    }

    if (appPaths.GetStickersDirectory() != LR"(C:\RelayDesk\bin\data\stickers)") {
        return fail("Stickers directory mismatch.");
    }

    if (appPaths.GetFavoriteStickersDirectory()
        != LR"(C:\RelayDesk\bin\data\stickers\favorites)") {
        return fail("Favorite stickers directory mismatch.");
    }

    if (appPaths.GetStickerPacksDirectory()
        != LR"(C:\RelayDesk\bin\data\stickers\packs)") {
        return fail("Sticker packs directory mismatch.");
    }

    if (appPaths.GetInboxDirectory() != LR"(C:\RelayDesk\bin\data\transfers\inbox)") {
        return fail("Inbox directory mismatch.");
    }

    if (appPaths.GetOutboxDirectory() != LR"(C:\RelayDesk\bin\data\transfers\outbox)") {
        return fail("Outbox directory mismatch.");
    }

    if (appPaths.GetTempTransfersDirectory() != LR"(C:\RelayDesk\bin\data\transfers\temp)") {
        return fail("Temporary transfer directory mismatch.");
    }

    if (appPaths.GetImagesDirectory() != LR"(C:\RelayDesk\bin\data\images)") {
        return fail("Images directory mismatch.");
    }

    if (appPaths.GetImageBlobsDirectory() != LR"(C:\RelayDesk\bin\data\images\blobs)") {
        return fail("Image blobs directory mismatch.");
    }

    if (appPaths.GetImageThumbnailsDirectory()
        != LR"(C:\RelayDesk\bin\data\images\thumbnails)") {
        return fail("Image thumbnails directory mismatch.");
    }

    return 0;
}
