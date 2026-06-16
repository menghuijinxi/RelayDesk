#pragma once

#include "storage/app_paths.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace relaydesk::runtime {

class StoredImageAttachment {
public:
    const std::filesystem::path& GetImagePath() const { return imagePath_; }
    const std::optional<std::filesystem::path>& GetThumbnailPath() const
    {
        return thumbnailPath_;
    }
    const std::string& GetSha256() const { return sha256_; }

    void SetImagePath(std::filesystem::path imagePath)
    {
        imagePath_ = std::move(imagePath);
    }
    void SetThumbnailPath(std::filesystem::path thumbnailPath)
    {
        thumbnailPath_ = std::move(thumbnailPath);
    }
    void SetSha256(std::string sha256) { sha256_ = std::move(sha256); }

protected:
    std::filesystem::path imagePath_;
    std::optional<std::filesystem::path> thumbnailPath_;
    std::string sha256_;
};

std::optional<StoredImageAttachment> storePreviewableImageAttachment(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& sourcePath,
    const std::string& preferredFileName,
    bool removeSource);

}
