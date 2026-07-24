#pragma once

#include "platform/startup_launch.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace relaydesk::platform::detail {

StartupLaunchState determineStartupLaunchState(
    const std::optional<std::wstring>& startupCommand,
    const std::optional<std::vector<std::uint8_t>>& startupApproval,
    const std::filesystem::path& executablePath);

}
