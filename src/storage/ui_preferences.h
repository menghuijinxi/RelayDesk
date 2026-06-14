#pragma once

#include "storage/app_paths.h"

#include <string>
#include <vector>

namespace relaydesk::storage {

std::vector<std::string> loadRecentEmojis(const AppPaths& appPaths);
void saveRecentEmojis(const AppPaths& appPaths,
                      const std::vector<std::string>& recentEmojis);

}
