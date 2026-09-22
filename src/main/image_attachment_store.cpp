#include "main/image_attachment_store.h"

#include "platform/attachment_input.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace relaydesk::runtime {
namespace {

constexpr ULONG kFileHashBufferSize = 64u * 1024u;
constexpr unsigned int kStoredImageThumbnailMaxSide = 192u;
constexpr std::array<const char*, 6> kPreviewableImageExtensions{
    ".bmp",
    ".gif",
    ".jpeg",
    ".jpg",
    ".png",
    ".webp",
};

bool ntSuccess(NTSTATUS status)
{
    return status >= 0;
}

class BCryptAlgorithmHandle {
public:
    explicit BCryptAlgorithmHandle(BCRYPT_ALG_HANDLE handle)
        : handle_(handle)
    {
    }

    ~BCryptAlgorithmHandle()
    {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    BCRYPT_ALG_HANDLE Get() const { return handle_; }

protected:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class BCryptHashHandle {
public:
    explicit BCryptHashHandle(BCRYPT_HASH_HANDLE handle)
        : handle_(handle)
    {
    }

    ~BCryptHashHandle()
    {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }

    BCRYPT_HASH_HANDLE Get() const { return handle_; }

protected:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

std::string bytesToLowerHex(const std::vector<unsigned char>& bytes)
{
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2u);
    for (const unsigned char byte : bytes) {
        result.push_back(kHexDigits[(byte >> 4u) & 0x0Fu]);
        result.push_back(kHexDigits[byte & 0x0Fu]);
    }
    return result;
}

void checkBCryptStatus(NTSTATUS status, const char* message)
{
    if (!ntSuccess(status)) {
        throw std::runtime_error(message);
    }
}

DWORD getBCryptDwordProperty(BCRYPT_ALG_HANDLE algorithm,
                             const wchar_t* propertyName)
{
    DWORD value = 0;
    DWORD copiedSize = 0;
    checkBCryptStatus(
        BCryptGetProperty(algorithm,
                          propertyName,
                          reinterpret_cast<PUCHAR>(&value),
                          sizeof(value),
                          &copiedSize,
                          0),
        "Failed to query BCrypt property.");
    return value;
}

std::string computeFileSha256Hex(const std::filesystem::path& sourcePath)
{
    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    checkBCryptStatus(
        BCryptOpenAlgorithmProvider(&rawAlgorithm,
                                    BCRYPT_SHA256_ALGORITHM,
                                    nullptr,
                                    0),
        "Failed to open SHA-256 provider.");
    BCryptAlgorithmHandle algorithm(rawAlgorithm);

    const DWORD objectLength =
        getBCryptDwordProperty(algorithm.Get(), BCRYPT_OBJECT_LENGTH);
    const DWORD hashLength =
        getBCryptDwordProperty(algorithm.Get(), BCRYPT_HASH_LENGTH);
    std::vector<unsigned char> hashObject(objectLength);

    BCRYPT_HASH_HANDLE rawHash = nullptr;
    checkBCryptStatus(
        BCryptCreateHash(algorithm.Get(),
                         &rawHash,
                         hashObject.data(),
                         static_cast<ULONG>(hashObject.size()),
                         nullptr,
                         0,
                         0),
        "Failed to create SHA-256 hash.");
    BCryptHashHandle hash(rawHash);

    std::ifstream input(sourcePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open file for SHA-256.");
    }

    std::vector<char> buffer(kFileHashBufferSize);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize readSize = input.gcount();
        if (readSize <= 0) {
            break;
        }

        checkBCryptStatus(
            BCryptHashData(hash.Get(),
                           reinterpret_cast<PUCHAR>(buffer.data()),
                           static_cast<ULONG>(readSize),
                           0),
            "Failed to update SHA-256 hash.");
    }
    if (input.bad()) {
        throw std::runtime_error("Failed to read file for SHA-256.");
    }

    std::vector<unsigned char> digest(hashLength);
    checkBCryptStatus(
        BCryptFinishHash(hash.Get(),
                         digest.data(),
                         static_cast<ULONG>(digest.size()),
                         0),
        "Failed to finish SHA-256 hash.");
    return bytesToLowerHex(digest);
}

std::string lowerAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

std::string fileNameFromPathText(const std::string& pathText)
{
    const std::size_t position = pathText.find_last_of("/\\");
    if (position == std::string::npos) {
        return pathText;
    }
    return pathText.substr(position + 1u);
}

std::string extensionFromFileNameText(const std::string& fileName)
{
    const std::string leafName = fileNameFromPathText(fileName);
    const std::size_t position = leafName.find_last_of('.');
    if (position == std::string::npos || position + 1u >= leafName.size()) {
        return {};
    }
    return lowerAscii(leafName.substr(position));
}

bool isPreviewableImageExtension(const std::string& extension)
{
    return std::find(kPreviewableImageExtensions.begin(),
                     kPreviewableImageExtensions.end(),
                     extension)
        != kPreviewableImageExtensions.end();
}

std::string chooseStoredImageExtension(const std::filesystem::path& sourcePath,
                                       const std::string& preferredFileName)
{
    std::string extension = extensionFromFileNameText(preferredFileName);
    if (!isPreviewableImageExtension(extension)) {
        extension = lowerAscii(sourcePath.extension().string());
    }
    if (!isPreviewableImageExtension(extension)) {
        return ".img";
    }
    return extension;
}

bool pathsEquivalent(const std::filesystem::path& left,
                     const std::filesystem::path& right)
{
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    return !error && equivalent;
}

std::optional<std::filesystem::path> findExistingImageBlob(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& sha256)
{
    std::error_code error;
    if (!std::filesystem::is_directory(appPaths.GetImageBlobsDirectory(), error)
        || error) {
        return std::nullopt;
    }

    for (const auto& entry :
         std::filesystem::directory_iterator(appPaths.GetImageBlobsDirectory(),
                                             error)) {
        if (error) {
            return std::nullopt;
        }
        if (!entry.is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        if (entry.path().stem().string() == sha256) {
            return entry.path().lexically_normal();
        }
    }
    return std::nullopt;
}

void removeFileAndEmptyParent(const std::filesystem::path& sourcePath)
{
    std::error_code error;
    std::filesystem::remove(sourcePath, error);
    error.clear();
    std::filesystem::remove(sourcePath.parent_path(), error);
}

std::filesystem::path copyOrMoveImageBlob(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& sourcePath,
    const std::string& sha256,
    const std::string& extension,
    bool removeSource)
{
    if (const auto existingPath = findExistingImageBlob(appPaths, sha256)) {
        if (removeSource && !pathsEquivalent(sourcePath, existingPath.value())) {
            removeFileAndEmptyParent(sourcePath);
        }
        return existingPath.value();
    }

    std::filesystem::create_directories(appPaths.GetImageBlobsDirectory());
    const std::filesystem::path targetPath =
        appPaths.GetImageBlobsDirectory() / (sha256 + extension);
    if (pathsEquivalent(sourcePath, targetPath)) {
        return targetPath.lexically_normal();
    }

    if (removeSource) {
        std::error_code error;
        std::filesystem::rename(sourcePath, targetPath, error);
        if (!error) {
            std::filesystem::remove(sourcePath.parent_path(), error);
            return targetPath.lexically_normal();
        }
        error.clear();
        std::filesystem::copy_file(sourcePath,
                                   targetPath,
                                   std::filesystem::copy_options::overwrite_existing);
        removeFileAndEmptyParent(sourcePath);
        return targetPath.lexically_normal();
    }

    std::filesystem::copy_file(sourcePath,
                               targetPath,
                               std::filesystem::copy_options::overwrite_existing);
    return targetPath.lexically_normal();
}

bool fileIsNonEmpty(const std::filesystem::path& filePath)
{
    std::error_code error;
    return std::filesystem::is_regular_file(filePath, error)
        && !error
        && std::filesystem::file_size(filePath, error) > 0u
        && !error;
}

std::optional<std::filesystem::path> ensureImageThumbnail(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& imagePath,
    const std::string& sha256)
{
    std::filesystem::create_directories(appPaths.GetImageThumbnailsDirectory());
    const std::filesystem::path thumbnailPath =
        appPaths.GetImageThumbnailsDirectory() / (sha256 + ".jpg");
    const std::filesystem::path legacyPngThumbnailPath =
        appPaths.GetImageThumbnailsDirectory() / (sha256 + ".png");
    if (fileIsNonEmpty(thumbnailPath)) {
        std::error_code error;
        std::filesystem::remove(legacyPngThumbnailPath, error);
        return thumbnailPath.lexically_normal();
    }

    std::error_code error;
    std::filesystem::remove(thumbnailPath, error);
    error.clear();

    const std::optional<std::filesystem::path> generatedPath =
        relaydesk::platform::createImageThumbnail(
            imagePath,
            thumbnailPath,
            kStoredImageThumbnailMaxSide);
    if (generatedPath.has_value() && fileIsNonEmpty(generatedPath.value())) {
        std::filesystem::remove(legacyPngThumbnailPath, error);
        return generatedPath.value().lexically_normal();
    }

    std::filesystem::remove(thumbnailPath, error);
    return std::nullopt;
}

} // namespace

std::string sha256FileHex(const std::filesystem::path& sourcePath)
{
    return computeFileSha256Hex(sourcePath);
}

std::optional<StoredImageAttachment> storePreviewableImageAttachment(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& sourcePath,
    const std::string& preferredFileName,
    bool removeSource)
{
    if (!relaydesk::platform::probeImageSize(sourcePath).has_value()) {
        return std::nullopt;
    }

    const std::string sha256 = sha256FileHex(sourcePath);
    const std::string extension =
        chooseStoredImageExtension(sourcePath, preferredFileName);
    const std::filesystem::path imagePath =
        copyOrMoveImageBlob(appPaths, sourcePath, sha256, extension, removeSource);

    StoredImageAttachment storedImage;
    storedImage.SetImagePath(imagePath);
    storedImage.SetSha256(sha256);
    if (const auto thumbnailPath = ensureImageThumbnail(appPaths, imagePath, sha256)) {
        storedImage.SetThumbnailPath(thumbnailPath.value());
    }
    return storedImage;
}

}
