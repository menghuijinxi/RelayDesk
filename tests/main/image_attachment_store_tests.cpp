#include "main/image_attachment_store.h"
#include "platform/attachment_input.h"
#include "storage/app_paths.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <vector>

namespace {

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int expect(bool condition, const char* message)
{
    if (!condition) {
        return fail(message);
    }
    return 0;
}

std::filesystem::path testRoot()
{
    return std::filesystem::path(RELAYDESK_IMAGE_ATTACHMENT_STORE_TEST_WORK_DIR);
}

relaydesk::storage::AppPaths makeAppPaths()
{
    const std::filesystem::path workDirectory = testRoot() / "bin";
    std::filesystem::create_directories(workDirectory);
    return relaydesk::storage::AppPaths(workDirectory / "relaydesk.exe");
}

void appendU16(std::vector<unsigned char>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<unsigned char>(value & 0xFFu));
    bytes.push_back(static_cast<unsigned char>((value >> 8u) & 0xFFu));
}

void appendU32(std::vector<unsigned char>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<unsigned char>(value & 0xFFu));
    bytes.push_back(static_cast<unsigned char>((value >> 8u) & 0xFFu));
    bytes.push_back(static_cast<unsigned char>((value >> 16u) & 0xFFu));
    bytes.push_back(static_cast<unsigned char>((value >> 24u) & 0xFFu));
}

void writeTestBmp(const std::filesystem::path& filePath,
                  std::uint32_t width,
                  std::uint32_t height)
{
    constexpr std::uint32_t kFileHeaderSize = 14u;
    constexpr std::uint32_t kInfoHeaderSize = 40u;
    constexpr std::uint32_t kPixelDataOffset = kFileHeaderSize + kInfoHeaderSize;
    const std::uint32_t rowStride = ((width * 3u + 3u) / 4u) * 4u;
    const std::uint32_t pixelDataSize = rowStride * height;
    const std::uint32_t fileSize = kPixelDataOffset + pixelDataSize;
    const std::uint32_t widthDenominator = width > 1u ? width - 1u : 1u;
    const std::uint32_t heightDenominator = height > 1u ? height - 1u : 1u;

    std::vector<unsigned char> bytes;
    bytes.reserve(fileSize);
    appendU16(bytes, 0x4D42u);
    appendU32(bytes, fileSize);
    appendU16(bytes, 0u);
    appendU16(bytes, 0u);
    appendU32(bytes, kPixelDataOffset);

    appendU32(bytes, kInfoHeaderSize);
    appendU32(bytes, width);
    appendU32(bytes, height);
    appendU16(bytes, 1u);
    appendU16(bytes, 24u);
    appendU32(bytes, 0u);
    appendU32(bytes, pixelDataSize);
    appendU32(bytes, 2835u);
    appendU32(bytes, 2835u);
    appendU32(bytes, 0u);
    appendU32(bytes, 0u);

    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t y = height - 1u - row;
        const std::size_t rowOffset = bytes.size();
        for (std::uint32_t x = 0; x < width; ++x) {
            bytes.push_back(
                static_cast<unsigned char>((x * 255u) / widthDenominator));
            bytes.push_back(
                static_cast<unsigned char>((y * 255u) / heightDenominator));
            bytes.push_back(
                static_cast<unsigned char>(((x + y) * 127u)
                                           / (width + height)));
        }
        bytes.resize(rowOffset + rowStride, 0u);
    }

    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

bool fileIsNonEmpty(const std::filesystem::path& filePath)
{
    std::error_code error;
    return std::filesystem::is_regular_file(filePath, error)
        && !error
        && std::filesystem::file_size(filePath, error) > 0u
        && !error;
}

bool fileStartsWith(const std::filesystem::path& filePath,
                    const std::vector<unsigned char>& expectedBytes)
{
    std::ifstream input(filePath, std::ios::binary);
    std::vector<unsigned char> actualBytes(expectedBytes.size(), 0u);
    input.read(reinterpret_cast<char*>(actualBytes.data()),
               static_cast<std::streamsize>(actualBytes.size()));
    return input.gcount() == static_cast<std::streamsize>(expectedBytes.size())
        && actualBytes == expectedBytes;
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());
    const relaydesk::storage::AppPaths appPaths = makeAppPaths();
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::filesystem::path sourcePath = testRoot() / "source.bmp";
    writeTestBmp(sourcePath, 320u, 240u);

    const std::optional<relaydesk::runtime::StoredImageAttachment> storedImage =
        relaydesk::runtime::storePreviewableImageAttachment(appPaths,
                                                            sourcePath,
                                                            "source.bmp",
                                                            false);
    if (const int check = expect(storedImage.has_value(),
                                 "Previewable image was not stored.")) {
        return check;
    }
    if (const int check = expect(storedImage->GetSha256().size() == 64u,
                                 "Stored image SHA-256 length mismatch.")) {
        return check;
    }
    if (const int check = expect(fileIsNonEmpty(storedImage->GetImagePath()),
                                 "Stored image blob is empty.")) {
        return check;
    }
    if (const int check = expect(storedImage->GetThumbnailPath().has_value(),
                                 "Stored image thumbnail was not generated.")) {
        return check;
    }
    if (const int check = expect(
            fileIsNonEmpty(storedImage->GetThumbnailPath().value()),
            "Stored image thumbnail is empty.")) {
        return check;
    }
    const std::filesystem::path thumbnailPath =
        storedImage->GetThumbnailPath().value();
    if (const int check = expect(thumbnailPath.extension().wstring() == L".jpg",
                                 "Stored image thumbnail extension mismatch.")) {
        return check;
    }
    if (const int check = expect(fileStartsWith(thumbnailPath,
                                                {0xFFu, 0xD8u, 0xFFu}),
                                 "Stored image thumbnail is not a JPEG file.")) {
        return check;
    }
    const std::optional<relaydesk::platform::ImageSize> thumbnailSize =
        relaydesk::platform::probeImageSize(thumbnailPath);
    if (const int check = expect(thumbnailSize.has_value(),
                                 "Stored image thumbnail size was not readable.")) {
        return check;
    }
    if (const int check = expect(thumbnailSize->width == 192u
                                     && thumbnailSize->height == 144u,
                                 "Stored image thumbnail dimensions mismatch.")) {
        return check;
    }

    const std::filesystem::path duplicatePath = testRoot() / "duplicate.bmp";
    std::filesystem::copy_file(sourcePath, duplicatePath);
    const std::optional<relaydesk::runtime::StoredImageAttachment> duplicateImage =
        relaydesk::runtime::storePreviewableImageAttachment(appPaths,
                                                            duplicatePath,
                                                            "duplicate.bmp",
                                                            true);
    if (const int check = expect(duplicateImage.has_value(),
                                 "Duplicate image was not stored.")) {
        return check;
    }
    if (const int check = expect(duplicateImage->GetImagePath()
                                     == storedImage->GetImagePath(),
                                 "Duplicate image did not reuse the blob.")) {
        return check;
    }
    if (const int check = expect(!std::filesystem::exists(duplicatePath),
                                 "Moved duplicate image source still exists.")) {
        return check;
    }

    return 0;
}
