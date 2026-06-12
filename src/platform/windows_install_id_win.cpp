#include "platform/windows_install_id.h"

#include "platform/text_encoding.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string>

#include <windows.h>

namespace relaydesk::platform {
namespace {

std::string normalizeInstallId(std::string value)
{
    const auto isSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };

    value.erase(value.begin(),
                std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(),
                value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

} // namespace

std::string getWindowsInstallId()
{
    std::array<wchar_t, 256> buffer{};
    DWORD bufferSize = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        LR"(SOFTWARE\Microsoft\Cryptography)",
        L"MachineGuid",
        RRF_RT_REG_SZ,
        nullptr,
        buffer.data(),
        &bufferSize);
    if (status != ERROR_SUCCESS) {
        throw std::runtime_error("Failed to read Windows install ID.");
    }

    const std::string installId = normalizeInstallId(wideToUtf8(buffer.data()));
    if (installId.empty()) {
        throw std::runtime_error("Windows install ID is empty.");
    }
    return installId;
}

}
