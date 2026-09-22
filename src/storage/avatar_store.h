#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace relaydesk::storage {

inline constexpr unsigned int kAvatarImageSide = 96;
inline constexpr std::size_t kMaxAvatarImageBytes = 64u * 1024u;

inline bool isAvatarSha256(std::string_view value)
{
    if (value.size() != 64) {
        return false;
    }

    for (const char character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool hex = character >= 'a' && character <= 'f';
        if (!digit && !hex) {
            return false;
        }
    }
    return true;
}

inline std::filesystem::path localAvatarDirectory(
    const std::filesystem::path& dataDirectory)
{
    return dataDirectory / "avatars";
}

inline std::filesystem::path peerAvatarDirectory(
    const std::filesystem::path& peerDirectory)
{
    return peerDirectory / "avatars";
}

inline std::filesystem::path avatarImagePath(
    const std::filesystem::path& directory,
    std::string_view avatarSha256)
{
    return directory / (std::string(avatarSha256) + ".jpg");
}

inline bool isHashedAvatarFileName(std::string_view filename)
{
    constexpr std::string_view kSuffix = ".jpg";
    if (filename.size() != 68 || !filename.ends_with(kSuffix)) {
        return false;
    }
    return isAvatarSha256(filename.substr(0, filename.size() - kSuffix.size()));
}

// 只删除哈希命名的 JPEG，避免误删正在写入的临时文件或其他资料。
inline void removeUnmatchedAvatarFiles(
    const std::filesystem::path& directory,
    std::string_view keepSha256)
{
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) {
        return;
    }

    const std::filesystem::directory_iterator end;
    std::filesystem::directory_iterator entry(directory, error);
    if (error) {
        return;
    }

    const std::string keepName = std::string(keepSha256) + ".jpg";
    std::vector<std::filesystem::path> stalePaths;
    for (; entry != end; entry.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }

        std::error_code fileError;
        if (!entry->is_regular_file(fileError) || fileError) {
            continue;
        }

        const std::string filename = entry->path().filename().string();
        if (!isHashedAvatarFileName(filename)) {
            continue;
        }
        if (!keepSha256.empty() && filename == keepName) {
            continue;
        }
        stalePaths.push_back(entry->path());
    }

    for (const std::filesystem::path& path : stalePaths) {
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
    }
}

// 当前哈希文件不存在时，返回目录里仍保留的上一张头像。
inline std::filesystem::path findStoredAvatarImage(
    const std::filesystem::path& directory)
{
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) {
        return {};
    }

    const std::filesystem::directory_iterator end;
    std::filesystem::directory_iterator entry(directory, error);
    if (error) {
        return {};
    }

    for (; entry != end; entry.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }

        std::error_code fileError;
        if (!entry->is_regular_file(fileError) || fileError) {
            continue;
        }
        if (!isHashedAvatarFileName(entry->path().filename().string())) {
            continue;
        }
        return entry->path();
    }
    return {};
}

}
