#pragma once

#include <filesystem>

namespace relaydesk::platform {

enum class StartupLaunchState {
    Enabled,
    DisabledBySystem,
    NotRegistered,
    DifferentExecutable,
};

[[nodiscard]] StartupLaunchState getStartupLaunchState(
    const std::filesystem::path& executablePath);
[[nodiscard]] bool synchronizeStartupLaunch(
    const std::filesystem::path& executablePath,
    bool configuredEnabled);
void setStartupLaunchEnabled(const std::filesystem::path& executablePath,
                             bool enabled);

}
