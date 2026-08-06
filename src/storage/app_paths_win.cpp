#include "storage/app_paths.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>

#include <windows.h>

namespace relaydesk::storage {

namespace {

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
        throw std::runtime_error("Failed to read test environment variable.");
    }
    value.resize(copiedSize);
    return value;
}

std::filesystem::path normalizedAbsolutePath(
    const std::filesystem::path& path,
    const char* description)
{
    if (!path.is_absolute()) {
        throw std::runtime_error(std::string(description)
                                 + " must be absolute.");
    }

    std::error_code error;
    const std::filesystem::path normalizedPath =
        std::filesystem::weakly_canonical(path, error);
    if (error || normalizedPath.empty()) {
        throw std::runtime_error(std::string("Failed to normalize ")
                                 + description + ".");
    }
    return normalizedPath.lexically_normal();
}

bool isStrictDescendant(const std::filesystem::path& path,
                        const std::filesystem::path& root)
{
    const std::filesystem::path relativePath = path.lexically_relative(root);
    if (relativePath.empty() || relativePath == "."
        || relativePath.is_absolute()) {
        return false;
    }
    for (const auto& component : relativePath) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

std::filesystem::path currentExecutablePath()
{
    std::array<wchar_t, 32768> executablePathBuffer{};
    const DWORD copiedSize = GetModuleFileNameW(
        nullptr,
        executablePathBuffer.data(),
        static_cast<DWORD>(executablePathBuffer.size()));

    if (copiedSize == 0 || copiedSize == executablePathBuffer.size()) {
        throw std::runtime_error("GetModuleFileNameW failed.");
    }
    return std::filesystem::path(executablePathBuffer.data());
}

AppPaths createTestAppPaths(const std::filesystem::path& executablePath,
                            const std::optional<std::wstring>& testRootValue,
                            const std::optional<std::wstring>& dataValue)
{
#if !defined(RELAYDESK_TEST_SANDBOX_PARENT)
    (void)executablePath;
    (void)testRootValue;
    (void)dataValue;
    throw std::runtime_error(
        "This build does not permit RelayDesk test data sandboxes.");
#else
    if (!testRootValue.has_value() || testRootValue->empty()) {
        throw std::runtime_error("RelayDesk test root is required.");
    }

    const std::filesystem::path allowedParent = normalizedAbsolutePath(
        std::filesystem::path(RELAYDESK_TEST_SANDBOX_PARENT),
        "RelayDesk test sandbox parent");
    const std::filesystem::path testRoot = normalizedAbsolutePath(
        std::filesystem::path(*testRootValue), "RelayDesk test root");
    if (!isStrictDescendant(testRoot, allowedParent)) {
        throw std::runtime_error(
            "RelayDesk test root is outside the permitted sandbox parent.");
    }

    const std::filesystem::path requestedDataDirectory =
        dataValue.has_value() && !dataValue->empty()
            ? std::filesystem::path(*dataValue)
            : executablePath.parent_path() / "data";
    const std::filesystem::path dataDirectory = normalizedAbsolutePath(
        requestedDataDirectory, "RelayDesk test data directory");
    if (!isStrictDescendant(dataDirectory, testRoot)) {
        throw std::runtime_error(
            "RelayDesk test data directory is outside the test root.");
    }
    return AppPaths(executablePath, dataDirectory);
#endif
}

} // namespace

AppPaths createAppPaths()
{
    const std::filesystem::path executablePath = currentExecutablePath();
    const std::optional<std::wstring> testMode =
        readEnvironmentVariable(kTestModeEnvironment);
    const std::optional<std::wstring> testRoot =
        readEnvironmentVariable(kTestRootEnvironment);
    const std::optional<std::wstring> testDataDirectory =
        readEnvironmentVariable(kTestDataDirectoryEnvironment);
    const bool hasTestEnvironment = testMode.has_value()
        || testRoot.has_value() || testDataDirectory.has_value();

    if (!hasTestEnvironment) {
        return AppPaths(executablePath);
    }
    if (!testMode.has_value() || testMode.value() != L"1") {
        throw std::runtime_error(
            "RelayDesk test paths require RELAYDESK_TEST_MODE=1.");
    }

    return createTestAppPaths(executablePath, testRoot, testDataDirectory);
}

bool isTestDataSandboxEnabled()
{
    const std::optional<std::wstring> testMode =
        readEnvironmentVariable(kTestModeEnvironment);
    return testMode.has_value() && testMode.value() == L"1";
}

}
