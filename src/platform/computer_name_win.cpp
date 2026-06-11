#include "platform/computer_name.h"

#include <array>
#include <stdexcept>

#include <windows.h>

namespace relaydesk::platform {

std::wstring getComputerName()
{
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> nameBuffer{};
    DWORD nameSize = static_cast<DWORD>(nameBuffer.size());

    if (GetComputerNameW(nameBuffer.data(), &nameSize) == 0) {
        throw std::runtime_error("GetComputerNameW failed.");
    }

    return std::wstring(nameBuffer.data(), nameSize);
}

}

