#pragma once

#include <string>

namespace relaydesk::platform {

std::string wideToUtf8(const std::wstring& value);
std::wstring utf8ToWide(const std::string& value);

}
