#include "storage/app_paths.h"
#include "storage/ui_preferences.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int expect(bool condition, const char* message)
{
    if (!condition) {
        return fail(message);
    }
    return 0;
}

std::filesystem::path testRoot()
{
    return std::filesystem::path(RELAYDESK_TEST_WORK_DIR);
}

relaydesk::storage::AppPaths makeAppPaths(const std::filesystem::path& caseName)
{
    const std::filesystem::path workDirectory = testRoot() / caseName / "bin";
    std::filesystem::create_directories(workDirectory);
    return relaydesk::storage::AppPaths(workDirectory / "relaydesk.exe");
}

void writeJsonFile(const std::filesystem::path& filePath, const nlohmann::json& content)
{
    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to write test file.");
    }
    output << content.dump(4) << '\n';
}

int returnsEmptyWhenConfigIsMissing()
{
    const auto appPaths = makeAppPaths("missing");
    const std::vector<std::string> recentEmojis =
        relaydesk::storage::loadRecentEmojis(appPaths);
    return expect(recentEmojis.empty(), "Missing config should yield empty recent emojis.");
}

int roundTripsRecentEmojis()
{
    const auto appPaths = makeAppPaths("round-trip");
    const std::vector<std::string> recentEmojis{
        "😀",
        "👍",
        "😂",
    };
    relaydesk::storage::saveRecentEmojis(appPaths, recentEmojis);

    const auto loaded = relaydesk::storage::loadRecentEmojis(appPaths);
    return expect(loaded == recentEmojis, "Recent emojis did not round-trip.");
}

int preservesOtherConfigFields()
{
    const auto appPaths = makeAppPaths("preserve-fields");
    writeJsonFile(appPaths.GetConfigFilePath(),
                  nlohmann::json{
                      {"schema_version", 1},
                      {"theme", "light"},
                      {"window_width", 1440},
                  });

    const std::vector<std::string> recentEmojis{"😎", "🥳"};
    relaydesk::storage::saveRecentEmojis(appPaths, recentEmojis);

    std::ifstream input(appPaths.GetConfigFilePath(), std::ios::binary);
    const nlohmann::json value = nlohmann::json::parse(input);
    if (const int result = expect(value.value("theme", "") == "light",
                                  "Existing config field was removed.");
        result != 0) {
        return result;
    }

    if (const int result = expect(value.value("window_width", 0) == 1440,
                                  "Existing numeric config field was removed.");
        result != 0) {
        return result;
    }

    return expect(value.at("recent_emojis").get<std::vector<std::string>>() == recentEmojis,
                  "Recent emojis were not saved.");
}

int defaultsLaunchAtStartupToEnabled()
{
    const auto appPaths = makeAppPaths("startup-default");
    return expect(relaydesk::storage::loadLaunchAtStartupEnabled(appPaths),
                  "Missing startup preference should default to enabled.");
}

int roundTripsLaunchAtStartup()
{
    const auto appPaths = makeAppPaths("startup-round-trip");
    relaydesk::storage::saveLaunchAtStartupEnabled(appPaths, false);
    if (const int result =
            expect(!relaydesk::storage::loadLaunchAtStartupEnabled(appPaths),
                   "Disabled startup preference did not round-trip.");
        result != 0) {
        return result;
    }

    relaydesk::storage::saveLaunchAtStartupEnabled(appPaths, true);
    return expect(relaydesk::storage::loadLaunchAtStartupEnabled(appPaths),
                  "Enabled startup preference did not round-trip.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = returnsEmptyWhenConfigIsMissing(); result != 0) {
        return result;
    }

    if (const int result = roundTripsRecentEmojis(); result != 0) {
        return result;
    }

    if (const int result = preservesOtherConfigFields(); result != 0) {
        return result;
    }

    if (const int result = defaultsLaunchAtStartupToEnabled(); result != 0) {
        return result;
    }

    if (const int result = roundTripsLaunchAtStartup(); result != 0) {
        return result;
    }

    return 0;
}
