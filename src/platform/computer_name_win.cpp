#include "platform/computer_name.h"

#include <array>
#include <stdexcept>

#include <windows.h>

namespace relaydesk::platform {
namespace {

std::string toUtf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }

    const int requiredSize = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (requiredSize == 0) {
        throw std::runtime_error("WideCharToMultiByte failed.");
    }

    std::string result(static_cast<std::size_t>(requiredSize), '\0');
    const int copiedSize = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        requiredSize,
        nullptr,
        nullptr);
    if (copiedSize == 0) {
        throw std::runtime_error("WideCharToMultiByte failed.");
    }

    return result;
}

} // namespace

std::wstring getComputerName()
{
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> nameBuffer{};
    DWORD nameSize = static_cast<DWORD>(nameBuffer.size());

    if (GetComputerNameW(nameBuffer.data(), &nameSize) == 0) {
        throw std::runtime_error("GetComputerNameW failed.");
    }

    return std::wstring(nameBuffer.data(), nameSize);
}

std::string getComputerNameUtf8()
{
    return toUtf8(getComputerName());
}

}
