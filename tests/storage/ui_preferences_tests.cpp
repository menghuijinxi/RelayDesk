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
    relaydesk::storage::saveScreenShakeCooldownMilliseconds(appPaths, 17000);

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

    if (const int result = expect(
            value.at("recent_emojis").get<std::vector<std::string>>()
                == recentEmojis,
            "Recent emojis were not saved.");
        result != 0) {
        return result;
    }

    return expect(value.value("screen_shake_cooldown_seconds", 0) == 17,
                  "Screen shake cooldown was not saved.");
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

int defaultsDarkModeToDisabled()
{
    const auto appPaths = makeAppPaths("dark-mode-default");
    return expect(!relaydesk::storage::loadDarkModeEnabled(appPaths),
                  "Missing dark mode preference should default to disabled.");
}

int roundTripsDarkMode()
{
    const auto appPaths = makeAppPaths("dark-mode-round-trip");
    relaydesk::storage::saveDarkModeEnabled(appPaths, true);
    if (const int result =
            expect(relaydesk::storage::loadDarkModeEnabled(appPaths),
                   "Enabled dark mode preference did not round-trip.");
        result != 0) {
        return result;
    }

    relaydesk::storage::saveDarkModeEnabled(appPaths, false);
    return expect(!relaydesk::storage::loadDarkModeEnabled(appPaths),
                  "Disabled dark mode preference did not round-trip.");
}

int defaultsAutoReceiveFilesToDisabled()
{
    const auto appPaths = makeAppPaths("auto-receive-default");
    return expect(!relaydesk::storage::loadAutoReceiveFilesEnabled(appPaths),
                  "Missing auto receive preference should default to disabled.");
}

int roundTripsAutoReceiveFiles()
{
    const auto appPaths = makeAppPaths("auto-receive-round-trip");
    relaydesk::storage::saveAutoReceiveFilesEnabled(appPaths, true);
    if (const int result =
            expect(relaydesk::storage::loadAutoReceiveFilesEnabled(appPaths),
                   "Enabled auto receive preference did not round-trip.");
        result != 0) {
        return result;
    }

    relaydesk::storage::saveAutoReceiveFilesEnabled(appPaths, false);
    return expect(!relaydesk::storage::loadAutoReceiveFilesEnabled(appPaths),
                  "Disabled auto receive preference did not round-trip.");
}

int defaultsScreenShakeCooldownToTenSeconds()
{
    const auto appPaths = makeAppPaths("screen-shake-default");
    return expect(
        relaydesk::storage::loadScreenShakeCooldownMilliseconds(appPaths)
            == 10000,
        "Missing screen shake cooldown should default to ten seconds.");
}

int roundTripsScreenShakeCooldown()
{
    const auto appPaths = makeAppPaths("screen-shake-round-trip");
    relaydesk::storage::saveScreenShakeCooldownMilliseconds(appPaths, 0);
    if (const int result = expect(
            relaydesk::storage::loadScreenShakeCooldownMilliseconds(appPaths)
                == 0,
            "Minimum screen shake cooldown did not round-trip.");
        result != 0) {
        return result;
    }

    relaydesk::storage::saveScreenShakeCooldownMilliseconds(appPaths, 500);
    if (const int result = expect(
            relaydesk::storage::loadScreenShakeCooldownMilliseconds(appPaths)
                == 500,
            "Half-second screen shake cooldown did not round-trip.");
        result != 0) {
        return result;
    }

    relaydesk::storage::saveScreenShakeCooldownMilliseconds(appPaths, 1500);
    if (const int result = expect(
            relaydesk::storage::loadScreenShakeCooldownMilliseconds(appPaths)
                == 1500,
            "One-and-a-half-second screen shake cooldown did not round-trip.");
        result != 0) {
        return result;
    }

    relaydesk::storage::saveScreenShakeCooldownMilliseconds(appPaths, 60000);
    const int loadedMaximum =
        relaydesk::storage::loadScreenShakeCooldownMilliseconds(appPaths);
    return expect(loadedMaximum == 60000,
                  "Maximum screen shake cooldown did not round-trip.");
}

int rejectsInvalidScreenShakeCooldown()
{
    const auto appPaths = makeAppPaths("screen-shake-invalid");
    try {
        relaydesk::storage::saveScreenShakeCooldownMilliseconds(appPaths, -500);
        return fail("Invalid screen shake cooldown was saved.");
    } catch (const std::invalid_argument&) {
    }

    writeJsonFile(appPaths.GetConfigFilePath(),
                  nlohmann::json{
                      {"schema_version", 1},
                      {"screen_shake_cooldown_seconds", 1.25},
                  });
    try {
        (void)relaydesk::storage::loadScreenShakeCooldownMilliseconds(appPaths);
        return fail("Invalid screen shake cooldown step was accepted.");
    } catch (const std::runtime_error&) {
    }

    return 0;
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

    if (const int result = defaultsDarkModeToDisabled(); result != 0) {
        return result;
    }

    if (const int result = roundTripsDarkMode(); result != 0) {
        return result;
    }

    if (const int result = defaultsAutoReceiveFilesToDisabled(); result != 0) {
        return result;
    }

    if (const int result = roundTripsAutoReceiveFiles(); result != 0) {
        return result;
    }

    if (const int result = defaultsScreenShakeCooldownToTenSeconds();
        result != 0) {
        return result;
    }

    if (const int result = roundTripsScreenShakeCooldown(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidScreenShakeCooldown(); result != 0) {
        return result;
    }

    return 0;
}
