#pragma once

#include <filesystem>

namespace relaydesk::platform {

bool createCrashReportArchive(const std::filesystem::path& reportDirectory,
                              const std::filesystem::path& archivePath);

}
