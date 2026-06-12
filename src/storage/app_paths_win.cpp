#include "storage/app_paths.h"

#include <array>
#include <stdexcept>

#include <windows.h>

namespace relaydesk::storage {

AppPaths createAppPaths()
{
    std::array<wchar_t, 32768> executablePathBuffer{};
    const DWORD copiedSize = GetModuleFileNameW(
        nullptr,
        executablePathBuffer.data(),
        static_cast<DWORD>(executablePathBuffer.size()));

    if (copiedSize == 0 || copiedSize == executablePathBuffer.size()) {
        throw std::runtime_error("GetModuleFileNameW failed.");
    }

    return AppPaths(std::filesystem::path(executablePathBuffer.data()));
}

}

