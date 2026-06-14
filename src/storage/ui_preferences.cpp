#include "storage/ui_preferences.h"

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

}
