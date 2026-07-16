#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace relaydesk::platform {

struct ImageSize {
    unsigned int width = 0;
    unsigned int height = 0;
};

void initializeAttachmentDropTarget();
std::vector<std::filesystem::path> consumeDroppedAttachmentPaths();
std::vector<std::filesystem::path> collectClipboardAttachmentPaths();
std::vector<std::filesystem::path> collectClipboardImageAttachmentPaths();
std::uint32_t getClipboardSequenceNumber();
bool startScreenClipCapture();
std::optional<ImageSize> probeImageSize(const std::filesystem::path& sourcePath);
std::optional<std::filesystem::path> createImageThumbnail(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& targetPath,
    unsigned int maxSide);
std::optional<std::filesystem::path> selectSavePathFromDialog(
    const std::filesystem::path& initialDirectory,
    const std::string& suggestedFileName);
std::optional<std::filesystem::path> selectFolderFromDialog();
bool copyImageFileToClipboard(const std::filesystem::path& sourcePath);
bool copyAttachmentPathToClipboard(const std::filesystem::path& sourcePath);
bool revealPathInFileManager(const std::filesystem::path& sourcePath);
bool consumeBackspacePressed();

}
