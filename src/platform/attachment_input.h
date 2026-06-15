#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace relaydesk::platform {

struct ImageSize {
    unsigned int width = 0;
    unsigned int height = 0;
};

void initializeAttachmentDropTarget();
std::vector<std::filesystem::path> consumeDroppedAttachmentPaths();
std::vector<std::filesystem::path> collectClipboardAttachmentPaths();
std::optional<ImageSize> probeImageSize(const std::filesystem::path& sourcePath);
std::optional<std::filesystem::path> createImageThumbnail(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& targetPath,
    unsigned int maxSide);
bool isPasteShortcutDown();
bool consumeBackspacePressed();

}
