#include "platform/startup_launch.h"

#include "platform/startup_launch_detail.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace relaydesk::platform {
namespace {

constexpr wchar_t kRunKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupApprovedRunKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\"
    L"StartupApproved\\Run";
constexpr wchar_t kRunValueName[] = L"RelayDesk";

using RegistryValue = std::pair<DWORD, std::vector<std::uint8_t>>;

bool isMissingRegistryEntry(LSTATUS result)
{
    return result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
}

class RegistryKey {
public:
    explicit RegistryKey(HKEY key)
        : key_(key)
    {
    }

    ~RegistryKey()
    {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    [[nodiscard]] HKEY Get() const { return key_; }

protected:
    HKEY key_ = nullptr;
};

RegistryKey createCurrentUserKey(const wchar_t* keyPath)
{
    HKEY key = nullptr;
    const LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER,
                                           keyPath,
                                           0,
                                           nullptr,
                                           REG_OPTION_NON_VOLATILE,
                                           KEY_SET_VALUE,
                                           nullptr,
                                           &key,
                                           nullptr);
    if (result != ERROR_SUCCESS) {
        throw std::runtime_error("Failed to open Windows startup registry key.");
    }

    return RegistryKey(key);
}

std::optional<RegistryValue> readCurrentUserRegistryValue(
    const wchar_t* keyPath,
    const wchar_t* valueName)
{
    HKEY rawKey = nullptr;
    const LSTATUS openResult = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        keyPath,
        0,
        KEY_QUERY_VALUE,
        &rawKey);
    if (isMissingRegistryEntry(openResult)) {
        return std::nullopt;
    }
    if (openResult != ERROR_SUCCESS) {
        throw std::runtime_error("Failed to read Windows startup registry key.");
    }
    const RegistryKey key(rawKey);

    while (true) {
        DWORD valueType = REG_NONE;
        DWORD byteSize = 0;
        const LSTATUS sizeResult = RegQueryValueExW(
            key.Get(), valueName, nullptr, &valueType, nullptr, &byteSize);
        if (isMissingRegistryEntry(sizeResult)) {
            return std::nullopt;
        }
        if (sizeResult != ERROR_SUCCESS) {
            throw std::runtime_error("Failed to read Windows startup setting.");
        }

        std::vector<std::uint8_t> bytes(byteSize);
        DWORD actualByteSize = byteSize;
        const LSTATUS valueResult = RegQueryValueExW(
            key.Get(),
            valueName,
            nullptr,
            &valueType,
            bytes.empty() ? nullptr : bytes.data(),
            &actualByteSize);
        if (valueResult == ERROR_MORE_DATA) {
            continue;
        }
        if (isMissingRegistryEntry(valueResult)) {
            return std::nullopt;
        }
        if (valueResult != ERROR_SUCCESS) {
            throw std::runtime_error("Failed to read Windows startup setting.");
        }
        bytes.resize(actualByteSize);
        return RegistryValue{valueType, std::move(bytes)};
    }
}

std::wstring expandEnvironmentVariables(const std::wstring& value)
{
    const DWORD requiredSize = ExpandEnvironmentStringsW(
        value.c_str(), nullptr, 0);
    if (requiredSize == 0) {
        throw std::runtime_error(
            "Failed to expand Windows startup environment variables.");
    }

    std::wstring expandedValue(requiredSize, L'\0');
    const DWORD writtenSize = ExpandEnvironmentStringsW(
        value.c_str(), expandedValue.data(), requiredSize);
    if (writtenSize == 0 || writtenSize > requiredSize) {
        throw std::runtime_error(
            "Failed to expand Windows startup environment variables.");
    }
    expandedValue.resize(writtenSize - 1u);
    return expandedValue;
}

std::optional<std::wstring> readStartupCommand()
{
    const std::optional<RegistryValue> registryValue =
        readCurrentUserRegistryValue(kRunKeyPath, kRunValueName);
    if (!registryValue.has_value()) {
        return std::nullopt;
    }
    if (registryValue->first != REG_SZ
        && registryValue->first != REG_EXPAND_SZ) {
        throw std::runtime_error("Windows startup setting has an invalid type.");
    }
    if (registryValue->second.size() % sizeof(wchar_t) != 0u) {
        throw std::runtime_error("Windows startup setting has an invalid size.");
    }

    std::vector<wchar_t> characters(
        registryValue->second.size() / sizeof(wchar_t) + 1u,
        L'\0');
    if (!registryValue->second.empty()) {
        std::memcpy(characters.data(),
                    registryValue->second.data(),
                    registryValue->second.size());
    }
    const std::wstring command(characters.data());
    if (registryValue->first == REG_EXPAND_SZ) {
        return expandEnvironmentVariables(command);
    }
    return command;
}

std::optional<std::vector<std::uint8_t>> readStartupApproval()
{
    const std::optional<RegistryValue> registryValue =
        readCurrentUserRegistryValue(
            kStartupApprovedRunKeyPath,
            kRunValueName);
    if (!registryValue.has_value()) {
        return std::nullopt;
    }
    if (registryValue->first != REG_BINARY) {
        return std::vector<std::uint8_t>{};
    }
    return registryValue->second;
}

void deleteCurrentUserRegistryValue(const wchar_t* keyPath,
                                    const wchar_t* valueName)
{
    HKEY rawKey = nullptr;
    const LSTATUS openResult = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        keyPath,
        0,
        KEY_SET_VALUE,
        &rawKey);
    if (isMissingRegistryEntry(openResult)) {
        return;
    }
    if (openResult != ERROR_SUCCESS) {
        throw std::runtime_error("Failed to open Windows startup registry key.");
    }
    const RegistryKey key(rawKey);

    const LSTATUS deleteResult = RegDeleteValueW(key.Get(), valueName);
    if (deleteResult != ERROR_SUCCESS
        && !isMissingRegistryEntry(deleteResult)) {
        throw std::runtime_error("Failed to remove Windows startup setting.");
    }
}

std::wstring makeStartupCommand(const std::filesystem::path& executablePath)
{
    if (executablePath.empty()) {
        throw std::runtime_error("Startup executable path is empty.");
    }

    return L"\"" + executablePath.wstring() + L"\"";
}

void enableStartupLaunch(const std::filesystem::path& executablePath)
{
    const RegistryKey key = createCurrentUserKey(kRunKeyPath);
    const std::wstring command = makeStartupCommand(executablePath);
    const DWORD byteSize =
        static_cast<DWORD>((command.size() + 1u) * sizeof(wchar_t));
    const LSTATUS result = RegSetValueExW(
        key.Get(),
        kRunValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        byteSize);
    if (result != ERROR_SUCCESS) {
        throw std::runtime_error("Failed to save Windows startup setting.");
    }

    deleteCurrentUserRegistryValue(
        kStartupApprovedRunKeyPath,
        kRunValueName);
}

void disableStartupLaunch()
{
    deleteCurrentUserRegistryValue(
        kStartupApprovedRunKeyPath,
        kRunValueName);
    deleteCurrentUserRegistryValue(kRunKeyPath, kRunValueName);
}

} // namespace

StartupLaunchState getStartupLaunchState(
    const std::filesystem::path& executablePath)
{
    if (executablePath.empty()) {
        throw std::runtime_error("Startup executable path is empty.");
    }

    return detail::determineStartupLaunchState(
        readStartupCommand(),
        readStartupApproval(),
        executablePath);
}

bool synchronizeStartupLaunch(const std::filesystem::path& executablePath,
                              bool configuredEnabled)
{
    switch (getStartupLaunchState(executablePath)) {
    case StartupLaunchState::Enabled:
        return true;
    case StartupLaunchState::DisabledBySystem:
        return false;
    case StartupLaunchState::NotRegistered:
    case StartupLaunchState::DifferentExecutable:
        setStartupLaunchEnabled(executablePath, configuredEnabled);
        return configuredEnabled;
    }

    throw std::runtime_error("Windows startup state is invalid.");
}

void setStartupLaunchEnabled(const std::filesystem::path& executablePath,
                             bool enabled)
{
    if (enabled) {
        enableStartupLaunch(executablePath);
    } else {
        disableStartupLaunch();
    }

    const StartupLaunchState expectedState = enabled
        ? StartupLaunchState::Enabled
        : StartupLaunchState::NotRegistered;
    if (getStartupLaunchState(executablePath) != expectedState) {
        throw std::runtime_error("Failed to verify Windows startup setting.");
    }
}

}
