#include "storage/ui_preferences.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace relaydesk::storage {
namespace {

constexpr int kSchemaVersion = 1;

nlohmann::json readConfigJson(const std::filesystem::path& filePath)
{
    if (!std::filesystem::exists(filePath)) {
        return nlohmann::json::object();
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open UI config file for reading.");
    }

    try {
        nlohmann::json value = nlohmann::json::parse(input);
        if (!value.is_object()) {
            throw std::runtime_error("UI config file root must be an object.");
        }
        return value;
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("UI config file contains invalid JSON.");
    }
}

void validateSchemaVersion(const nlohmann::json& value)
{
    if (!value.contains("schema_version")) {
        return;
    }

    if (!value["schema_version"].is_number_integer()
        || value["schema_version"].get<int>() != kSchemaVersion) {
        throw std::runtime_error("UI config schema version is unsupported.");
    }
}

std::vector<std::string> readRecentEmojiArray(const nlohmann::json& value)
{
    if (!value.contains("recent_emojis")) {
        return {};
    }

    if (!value["recent_emojis"].is_array()) {
        throw std::runtime_error("UI config recent emojis field is invalid.");
    }

    std::vector<std::string> result;
    for (const auto& item : value["recent_emojis"]) {
        if (!item.is_string()) {
            throw std::runtime_error("UI config recent emojis field is invalid.");
        }
        const std::string emoji = item.get<std::string>();
        if (!emoji.empty()) {
            result.push_back(emoji);
        }
    }
    return result;
}

bool readLaunchAtStartupEnabled(const nlohmann::json& value)
{
    if (!value.contains("launch_at_startup")) {
        return true;
    }

    if (!value["launch_at_startup"].is_boolean()) {
        throw std::runtime_error("UI config launch at startup field is invalid.");
    }

    return value["launch_at_startup"].get<bool>();
}

bool readDarkModeEnabled(const nlohmann::json& value)
{
    if (!value.contains("dark_mode")) {
        return false;
    }

    if (!value["dark_mode"].is_boolean()) {
        throw std::runtime_error("UI config dark mode field is invalid.");
    }

    return value["dark_mode"].get<bool>();
}

bool readAutoReceiveFilesEnabled(const nlohmann::json& value)
{
    if (!value.contains("auto_receive_files")) {
        return false;
    }

    if (!value["auto_receive_files"].is_boolean()) {
        throw std::runtime_error(
            "UI config auto receive files field is invalid.");
    }

    return value["auto_receive_files"].get<bool>();
}

int readScreenShakeCooldownMilliseconds(const nlohmann::json& value)
{
    if (!value.contains("screen_shake_cooldown_seconds")) {
        return kDefaultScreenShakeCooldownMilliseconds;
    }

    if (!value["screen_shake_cooldown_seconds"].is_number()) {
        throw std::runtime_error(
            "UI config screen shake cooldown field is invalid.");
    }

    const double seconds =
        value["screen_shake_cooldown_seconds"].get<double>();
    const double minimumSeconds =
        static_cast<double>(kMinimumScreenShakeCooldownMilliseconds) / 1000.0;
    const double maximumSeconds =
        static_cast<double>(kMaximumScreenShakeCooldownMilliseconds) / 1000.0;
    if (!std::isfinite(seconds) || seconds < minimumSeconds
        || seconds > maximumSeconds) {
        throw std::runtime_error(
            "UI config screen shake cooldown field is out of range.");
    }

    const double halfSecondSteps = seconds * 2.0;
    if (std::abs(halfSecondSteps - std::round(halfSecondSteps)) > 1e-9) {
        throw std::runtime_error(
            "UI config screen shake cooldown field has an invalid step.");
    }

    return static_cast<int>(std::lround(seconds * 1000.0));
}

void writeConfigJson(const std::filesystem::path& filePath,
                     const nlohmann::json& value)
{
    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open UI config file for writing.");
    }

    output << value.dump(4) << '\n';
    if (!output) {
        throw std::runtime_error("Failed to write UI config file.");
    }
}

} // namespace

std::vector<std::string> loadRecentEmojis(const AppPaths& appPaths)
{
    const nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    return readRecentEmojiArray(value);
}

void saveRecentEmojis(const AppPaths& appPaths,
                      const std::vector<std::string>& recentEmojis)
{
    nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    value["schema_version"] = kSchemaVersion;
    value["recent_emojis"] = recentEmojis;
    writeConfigJson(appPaths.GetConfigFilePath(), value);
}

bool loadLaunchAtStartupEnabled(const AppPaths& appPaths)
{
    const nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    return readLaunchAtStartupEnabled(value);
}

void saveLaunchAtStartupEnabled(const AppPaths& appPaths, bool enabled)
{
    nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    value["schema_version"] = kSchemaVersion;
    value["launch_at_startup"] = enabled;
    writeConfigJson(appPaths.GetConfigFilePath(), value);
}

bool loadDarkModeEnabled(const AppPaths& appPaths)
{
    const nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    return readDarkModeEnabled(value);
}

void saveDarkModeEnabled(const AppPaths& appPaths, bool enabled)
{
    nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    value["schema_version"] = kSchemaVersion;
    value["dark_mode"] = enabled;
    writeConfigJson(appPaths.GetConfigFilePath(), value);
}

bool loadAutoReceiveFilesEnabled(const AppPaths& appPaths)
{
    const nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    return readAutoReceiveFilesEnabled(value);
}

void saveAutoReceiveFilesEnabled(const AppPaths& appPaths, bool enabled)
{
    nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    value["schema_version"] = kSchemaVersion;
    value["auto_receive_files"] = enabled;
    writeConfigJson(appPaths.GetConfigFilePath(), value);
}

int loadScreenShakeCooldownMilliseconds(const AppPaths& appPaths)
{
    const nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    return readScreenShakeCooldownMilliseconds(value);
}

void saveScreenShakeCooldownMilliseconds(const AppPaths& appPaths,
                                         int milliseconds)
{
    if (milliseconds < kMinimumScreenShakeCooldownMilliseconds
        || milliseconds > kMaximumScreenShakeCooldownMilliseconds
        || milliseconds % kScreenShakeCooldownStepMilliseconds != 0) {
        throw std::invalid_argument(
            "Screen shake cooldown preference is out of range.");
    }

    nlohmann::json value = readConfigJson(appPaths.GetConfigFilePath());
    validateSchemaVersion(value);
    value["schema_version"] = kSchemaVersion;
    if (milliseconds % 1000 == 0) {
        value["screen_shake_cooldown_seconds"] = milliseconds / 1000;
    } else {
        value["screen_shake_cooldown_seconds"] =
            static_cast<double>(milliseconds) / 1000.0;
    }
    writeConfigJson(appPaths.GetConfigFilePath(), value);
}

}
