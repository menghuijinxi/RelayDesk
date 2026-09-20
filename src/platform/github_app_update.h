#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace relaydesk::platform {

class GitHubAppUpdateRelease {
public:
    const std::string& GetTagName() const { return tagName_; }
    int GetAppVersion() const { return appVersion_; }
    const std::string& GetAssetName() const { return assetName_; }
    const std::string& GetDownloadUrl() const { return downloadUrl_; }
    std::uintmax_t GetAssetSize() const { return assetSize_; }
    const std::string& GetSha256() const { return sha256_; }

    void SetTagName(std::string tagName) { tagName_ = std::move(tagName); }
    void SetAppVersion(int appVersion) { appVersion_ = appVersion; }
    void SetAssetName(std::string assetName)
    {
        assetName_ = std::move(assetName);
    }
    void SetDownloadUrl(std::string downloadUrl)
    {
        downloadUrl_ = std::move(downloadUrl);
    }
    void SetAssetSize(std::uintmax_t assetSize) { assetSize_ = assetSize; }
    void SetSha256(std::string sha256) { sha256_ = std::move(sha256); }

protected:
    std::string tagName_;
    int appVersion_ = 0;
    std::string assetName_;
    std::string downloadUrl_;
    std::uintmax_t assetSize_ = 0;
    std::string sha256_;
};

std::optional<GitHubAppUpdateRelease> parseGitHubAppUpdateReleaseJson(
    std::string_view jsonText,
    std::string_view assetName);

GitHubAppUpdateRelease fetchGitHubAppUpdateRelease(
    std::string_view repository,
    std::string_view assetName);

void downloadGitHubAppUpdate(
    const GitHubAppUpdateRelease& release,
    const std::filesystem::path& destinationPath,
    const std::function<bool()>& isCanceled,
    const std::function<void(std::uintmax_t)>& onProgress);

}
