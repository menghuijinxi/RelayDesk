#pragma once

#include <filesystem>
#include <vector>

namespace relaydesk::platform {

void initializeAttachmentDropTarget();
std::vector<std::filesystem::path> consumeDroppedAttachmentPaths();
std::vector<std::filesystem::path> collectClipboardAttachmentPaths();
bool isPasteShortcutDown();

}
