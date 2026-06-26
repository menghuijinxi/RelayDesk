#include "platform/startup_launch.h"

#include <stdexcept>
#include <string>

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
constexpr wchar_t kRunValueName[] = L"RelayDesk";

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

    HKEY Get() const { return key_; }

protected:
    HKEY key_ = nullptr;
};

RegistryKey openCurrentUserRunKey()
{
    HKEY key = nullptr;
    const LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER,
                                           kRunKeyPath,
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

std::wstring makeStartupCommand(const std::filesystem::path& executablePath)
{
    if (executablePath.empty()) {
        throw std::runtime_error("Startup executable path is empty.");
    }

    return L"\"" + executablePath.wstring() + L"\"";
}

void enableStartupLaunch(const std::filesystem::path& executablePath)
{
    const RegistryKey key = openCurrentUserRunKey();
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
}

void disableStartupLaunch()
{
    const RegistryKey key = openCurrentUserRunKey();
    const LSTATUS result = RegDeleteValueW(key.Get(), kRunValueName);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        throw std::runtime_error("Failed to remove Windows startup setting.");
    }
}

} // namespace

void setStartupLaunchEnabled(const std::filesystem::path& executablePath,
                             bool enabled)
{
    if (enabled) {
        enableStartupLaunch(executablePath);
        return;
    }

    disableStartupLaunch();
}

}
