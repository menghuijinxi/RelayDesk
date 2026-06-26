#pragma once

#include <filesystem>

namespace relaydesk::platform {

void setStartupLaunchEnabled(const std::filesystem::path& executablePath,
                             bool enabled);

}
