#include "storage/app_paths.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

#if defined(_WIN32)
constexpr wchar_t kTestModeEnvironment[] = L"RELAYDESK_TEST_MODE";
constexpr wchar_t kTestRootEnvironment[] = L"RELAYDESK_TEST_ROOT";
constexpr wchar_t kTestDataDirectoryEnvironment[] =
    L"RELAYDESK_TEST_DATA_DIRECTORY";

std::optional<std::wstring> readEnvironmentVariable(const wchar_t* name)
{
    SetLastError(ERROR_SUCCESS);
    const DWORD requiredSize = GetEnvironmentVariableW(name, nullptr, 0);
    if (requiredSize == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return std::wstring{};
    }

    std::wstring value(requiredSize, L'\0');
    const DWORD copiedSize =
        GetEnvironmentVariableW(name, value.data(), requiredSize);
    if (copiedSize == 0 || copiedSize >= requiredSize) {
        throw std::runtime_error("Failed to read environment variable.");
    }
    value.resize(copiedSize);
    return value;
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const wchar_t* name,
                              std::optional<std::wstring> value)
        : name_(name), previousValue_(readEnvironmentVariable(name))
    {
        set(value);
    }

    ~ScopedEnvironmentVariable()
    {
        set(previousValue_);
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) =
        delete;

protected:
    void set(const std::optional<std::wstring>& value) const
    {
        if (!SetEnvironmentVariableW(
                name_.c_str(), value.has_value() ? value->c_str() : nullptr)) {
            std::terminate();
        }
    }

    std::wstring name_;
    std::optional<std::wstring> previousValue_;
};

bool createAppPathsThrows()
{
    try {
        (void)relaydesk::storage::createAppPaths();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

int verifiesTestDataSandbox()
{
    const std::filesystem::path sandboxParent =
        std::filesystem::path(RELAYDESK_TEST_SANDBOX_PARENT);
    const std::filesystem::path testRoot = sandboxParent / "app_paths";
    const std::filesystem::path testDataDirectory = testRoot / "case" / "data";

    const ScopedEnvironmentVariable testMode(kTestModeEnvironment, L"1");
    const ScopedEnvironmentVariable testRootVariable(
        kTestRootEnvironment, testRoot.wstring());
    const ScopedEnvironmentVariable testDataDirectoryVariable(
        kTestDataDirectoryEnvironment, testDataDirectory.wstring());

    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    if (appPaths.GetDataDirectory() != testDataDirectory) {
        return fail("Test data directory override was ignored.");
    }
    return 0;
}

int rejectsPortableUserDataInTestMode()
{
    const std::filesystem::path sandboxParent =
        std::filesystem::path(RELAYDESK_TEST_SANDBOX_PARENT);
    const std::filesystem::path testRoot = sandboxParent / "app_paths";

    const ScopedEnvironmentVariable clearedMode(kTestModeEnvironment,
                                                 std::nullopt);
    const ScopedEnvironmentVariable clearedRoot(kTestRootEnvironment,
                                                 std::nullopt);
    const ScopedEnvironmentVariable clearedData(
        kTestDataDirectoryEnvironment, std::nullopt);
    const relaydesk::storage::AppPaths portablePaths =
        relaydesk::storage::createAppPaths();

    const ScopedEnvironmentVariable testMode(kTestModeEnvironment, L"1");
    const ScopedEnvironmentVariable testRootVariable(
        kTestRootEnvironment, testRoot.wstring());
    const ScopedEnvironmentVariable testDataDirectoryVariable(
        kTestDataDirectoryEnvironment,
        portablePaths.GetDataDirectory().wstring());

    return createAppPathsThrows()
        ? 0
        : fail("Test mode accepted the portable user data directory.");
}
#endif

}

int main()
{
    const std::filesystem::path executablePath =
        LR"(C:\RelayDesk\bin\relaydesk.exe)";
    const relaydesk::storage::AppPaths appPaths(executablePath);

    if (appPaths.GetExecutablePath() != executablePath) {
        return fail("Executable path mismatch.");
    }

    if (appPaths.GetWorkDirectory() != LR"(C:\RelayDesk\bin)") {
        return fail("Work directory mismatch.");
    }

    if (appPaths.GetDataDirectory() != LR"(C:\RelayDesk\bin\data)") {
        return fail("Data directory mismatch.");
    }

    if (appPaths.GetConfigFilePath() != LR"(C:\RelayDesk\bin\data\config.json)") {
        return fail("Config file path mismatch.");
    }

    if (appPaths.GetIdentityFilePath() != LR"(C:\RelayDesk\bin\data\identity.json)") {
        return fail("Identity file path mismatch.");
    }

    if (appPaths.GetStickersDirectory() != LR"(C:\RelayDesk\bin\data\stickers)") {
        return fail("Stickers directory mismatch.");
    }

    if (appPaths.GetFavoriteStickersDirectory()
        != LR"(C:\RelayDesk\bin\data\stickers\favorites)") {
        return fail("Favorite stickers directory mismatch.");
    }

    if (appPaths.GetStickerPacksDirectory()
        != LR"(C:\RelayDesk\bin\data\stickers\packs)") {
        return fail("Sticker packs directory mismatch.");
    }

    if (appPaths.GetInboxDirectory() != LR"(C:\RelayDesk\bin\data\transfers\inbox)") {
        return fail("Inbox directory mismatch.");
    }

    if (appPaths.GetOutboxDirectory() != LR"(C:\RelayDesk\bin\data\transfers\outbox)") {
        return fail("Outbox directory mismatch.");
    }

    if (appPaths.GetTempTransfersDirectory() != LR"(C:\RelayDesk\bin\data\transfers\temp)") {
        return fail("Temporary transfer directory mismatch.");
    }

    if (appPaths.GetImagesDirectory() != LR"(C:\RelayDesk\bin\data\images)") {
        return fail("Images directory mismatch.");
    }

    if (appPaths.GetImageBlobsDirectory() != LR"(C:\RelayDesk\bin\data\images\blobs)") {
        return fail("Image blobs directory mismatch.");
    }

    if (appPaths.GetImageThumbnailsDirectory()
        != LR"(C:\RelayDesk\bin\data\images\thumbnails)") {
        return fail("Image thumbnails directory mismatch.");
    }

#if defined(_WIN32)
    if (const int result = verifiesTestDataSandbox(); result != 0) {
        return result;
    }
    if (const int result = rejectsPortableUserDataInTestMode(); result != 0) {
        return result;
    }
#endif

    return 0;
}
