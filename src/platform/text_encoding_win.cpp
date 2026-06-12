#include "platform/text_encoding.h"

#include <stdexcept>

#include <windows.h>

namespace relaydesk::platform {

std::string wideToUtf8(const std::wstring& value)
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

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }

    const int requiredSize = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (requiredSize == 0) {
        throw std::runtime_error("MultiByteToWideChar failed.");
    }

    std::wstring result(static_cast<std::size_t>(requiredSize), L'\0');
    const int copiedSize = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        requiredSize);
    if (copiedSize == 0) {
        throw std::runtime_error("MultiByteToWideChar failed.");
    }

    return result;
}

}
