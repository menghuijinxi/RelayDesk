#pragma once

#include <filesystem>
#include <string>

namespace relaydesk::core {

void appendDiagnosticLogLine(const std::filesystem::path& filePath,
                             const std::string& message) noexcept;

}
