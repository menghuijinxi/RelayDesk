#pragma once

#include "storage/app_paths.h"

#include <string>
#include <vector>

namespace relaydesk::storage {

constexpr int kDefaultScreenShakeCooldownMilliseconds = 10000;
constexpr int kMinimumScreenShakeCooldownMilliseconds = 0;
constexpr int kMaximumScreenShakeCooldownMilliseconds = 60000;
constexpr int kScreenShakeCooldownStepMilliseconds = 500;

std::vector<std::string> loadRecentEmojis(const AppPaths& appPaths);
void saveRecentEmojis(const AppPaths& appPaths,
                      const std::vector<std::string>& recentEmojis);
bool loadLaunchAtStartupEnabled(const AppPaths& appPaths);
void saveLaunchAtStartupEnabled(const AppPaths& appPaths, bool enabled);
bool loadDarkModeEnabled(const AppPaths& appPaths);
void saveDarkModeEnabled(const AppPaths& appPaths, bool enabled);
bool loadAutoReceiveFilesEnabled(const AppPaths& appPaths);
void saveAutoReceiveFilesEnabled(const AppPaths& appPaths, bool enabled);
int loadScreenShakeCooldownMilliseconds(const AppPaths& appPaths);
void saveScreenShakeCooldownMilliseconds(const AppPaths& appPaths,
                                         int milliseconds);

}
