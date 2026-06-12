#include "storage/app_paths.h"
#include "storage/local_identity.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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

void writeJsonFile(const std::filesystem::path& filePath,
                   const nlohmann::json& content,
                   bool escapeNonAscii = false)
{
    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to write test file.");
    }
    output << content.dump(4, ' ', escapeNonAscii) << '\n';
}

int createsIdentityWhenFileIsMissing()
{
    const auto appPaths = makeAppPaths("create");
    const auto identity = relaydesk::storage::loadOrCreateLocalIdentity(appPaths, "HOST-A");

    if (const int result = expect(std::filesystem::exists(appPaths.GetIdentityFilePath()),
                                  "Identity file was not created.");
        result != 0) {
        return result;
    }

    if (const int result = expect(identity.GetSchemaVersion() == 1,
                                  "Schema version mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(!identity.GetDeviceId().empty(),
                                  "Device ID was not generated.");
        result != 0) {
        return result;
    }

    if (const int result = expect(!identity.GetInstallId().empty(),
                                  "Install ID was not generated.");
        result != 0) {
        return result;
    }

    if (const int result = expect(identity.GetDeviceId() != identity.GetInstallId(),
                                  "Device ID and install ID should be distinct.");
        result != 0) {
        return result;
    }

    if (const int result = expect(identity.GetHostName() == "HOST-A",
                                  "Host name was not saved.");
        result != 0) {
        return result;
    }

    return expect(identity.GetDisplayName() == "HOST-A",
                  "Default display name should match host name.");
}

int preservesIdsAndDisplayNameWhenHostChanges()
{
    const auto appPaths = makeAppPaths("preserve");
    const auto created = relaydesk::storage::loadOrCreateLocalIdentity(appPaths, "HOST-A");
    const auto renamed = relaydesk::storage::updateLocalDisplayName(appPaths, "Custom Name");
    if (const int result = expect(renamed.GetDeviceId() == created.GetDeviceId(),
                                  "Device ID changed after display name update.");
        result != 0) {
        return result;
    }

    if (const int result = expect(renamed.GetInstallId() == created.GetInstallId(),
                                  "Install ID changed after display name update.");
        result != 0) {
        return result;
    }

    const auto loaded = relaydesk::storage::loadOrCreateLocalIdentity(appPaths, "HOST-B");
    if (const int result = expect(loaded.GetDeviceId() == created.GetDeviceId(),
                                  "Device ID changed after reload.");
        result != 0) {
        return result;
    }

    if (const int result = expect(loaded.GetInstallId() == created.GetInstallId(),
                                  "Install ID changed after reload.");
        result != 0) {
        return result;
    }

    if (const int result = expect(loaded.GetHostName() == "HOST-B",
                                  "Host name was not refreshed.");
        result != 0) {
        return result;
    }

    return expect(loaded.GetDisplayName() == "Custom Name",
                  "Display name should not be overwritten by host refresh.");
}

int migratesDeviceIdToStableInstallId()
{
    const auto appPaths = makeAppPaths("stable-device");
    const auto created = relaydesk::storage::loadOrCreateLocalIdentity(
        appPaths,
        "HOST-A");
    const auto renamed = relaydesk::storage::updateLocalDisplayName(
        appPaths,
        "Custom Name");
    const auto migrated = relaydesk::storage::loadOrCreateLocalIdentity(
        appPaths,
        "HOST-B",
        "windows-install-guid");

    if (const int result = expect(migrated.GetDeviceId() == "windows-install-guid",
                                  "Stable device ID was not applied.");
        result != 0) {
        return result;
    }

    if (const int result = expect(migrated.GetDeviceId() != created.GetDeviceId(),
                                  "Random device ID was not migrated.");
        result != 0) {
        return result;
    }

    if (const int result = expect(migrated.GetInstallId() == renamed.GetInstallId(),
                                  "Install ID should be preserved during migration.");
        result != 0) {
        return result;
    }

    if (const int result = expect(migrated.GetHostName() == "HOST-B",
                                  "Host name should still refresh during migration.");
        result != 0) {
        return result;
    }

    return expect(migrated.GetDisplayName() == "Custom Name",
                  "Display name should survive stable ID migration.");
}

int rejectsEmptyDisplayNameUpdate()
{
    const auto appPaths = makeAppPaths("empty-display-name");
    static_cast<void>(relaydesk::storage::loadOrCreateLocalIdentity(appPaths, "HOST-A"));

    try {
        static_cast<void>(relaydesk::storage::updateLocalDisplayName(appPaths, ""));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Empty display name update was accepted.");
}

int roundTripsEscapedDisplayName()
{
    const auto appPaths = makeAppPaths("escaped");
    const std::string displayName = "Name \"Quoted\" \\ Path\nLine";
    const relaydesk::storage::LocalIdentity identity(
        "device-id",
        "install-id",
        "2026-06-12T00:00:00Z",
        "HOST-A",
        displayName);
    relaydesk::storage::saveLocalIdentity(appPaths, identity);

    const auto loaded = relaydesk::storage::loadLocalIdentity(appPaths);
    return expect(loaded.GetDisplayName() == displayName,
                  "Escaped display name did not round-trip.");
}

int loadsUnicodeEscapedIdentityFile()
{
    const auto appPaths = makeAppPaths("unicode-escape");
    writeJsonFile(
        appPaths.GetIdentityFilePath(),
        nlohmann::json{
            {"schema_version", 1},
            {"device_id", "device-id"},
            {"install_id", "install-id"},
            {"created_at", "2026-06-12T00:00:00Z"},
            {"host_name", "HOST-A"},
            {"display_name", "中文"},
        },
        true);

    const auto loaded = relaydesk::storage::loadLocalIdentity(appPaths);
    const std::string expectedDisplayName = "中文";
    return expect(loaded.GetDisplayName() == expectedDisplayName,
                  "Unicode escaped display name was not decoded.");
}

int rejectsInvalidIdentityFile()
{
    const auto appPaths = makeAppPaths("invalid");
    writeJsonFile(appPaths.GetIdentityFilePath(), nlohmann::json{
        {"schema_version", 1},
    });

    try {
        static_cast<void>(relaydesk::storage::loadLocalIdentity(appPaths));
    } catch (const std::exception&) {
        return 0;
    }

    return fail("Invalid identity file was accepted.");
}

} // namespace

int main()
{
    std::filesystem::remove_all(testRoot());

    if (const int result = createsIdentityWhenFileIsMissing(); result != 0) {
        return result;
    }

    if (const int result = preservesIdsAndDisplayNameWhenHostChanges(); result != 0) {
        return result;
    }

    if (const int result = migratesDeviceIdToStableInstallId(); result != 0) {
        return result;
    }

    if (const int result = rejectsEmptyDisplayNameUpdate(); result != 0) {
        return result;
    }

    if (const int result = roundTripsEscapedDisplayName(); result != 0) {
        return result;
    }

    if (const int result = loadsUnicodeEscapedIdentityFile(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidIdentityFile(); result != 0) {
        return result;
    }

    std::filesystem::remove_all(testRoot());
    return 0;
}
