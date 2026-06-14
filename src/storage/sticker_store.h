#pragma once

#include "storage/app_paths.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace relaydesk::storage {

enum class StickerPackImportFormat {
    RelayDeskManifest,
    OwOJson,
    ImageFolder,
};

class StickerItem {
public:
    StickerItem(std::string itemId, std::string displayName, std::string relativePath);

    const std::string& GetItemId() const { return itemId_; }
    const std::string& GetDisplayName() const { return displayName_; }
    const std::string& GetRelativePath() const { return relativePath_; }

protected:
    std::string itemId_;
    std::string displayName_;
    std::string relativePath_;
};

class StickerPack {
public:
    StickerPack(std::string packId,
                std::string displayName,
                StickerPackImportFormat importFormat,
                std::vector<StickerItem> items);

    const std::string& GetPackId() const { return packId_; }
    const std::string& GetDisplayName() const { return displayName_; }
    StickerPackImportFormat GetImportFormat() const { return importFormat_; }
    const std::vector<StickerItem>& GetItems() const { return items_; }

protected:
    std::string packId_;
    std::string displayName_;
    StickerPackImportFormat importFormat_ = StickerPackImportFormat::ImageFolder;
    std::vector<StickerItem> items_;
};

StickerPack loadFavoriteStickerPack(const AppPaths& appPaths);
std::vector<StickerPack> loadStickerPacks(const AppPaths& appPaths);
StickerItem addFavoriteStickerFromImage(const AppPaths& appPaths,
                                        const std::filesystem::path& imagePath,
                                        const std::string& displayName);
StickerPack importStickerPack(const AppPaths& appPaths,
                              const std::filesystem::path& sourceDirectory,
                              const std::string& fallbackDisplayName);

}
