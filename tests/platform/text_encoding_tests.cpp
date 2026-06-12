#include "platform/text_encoding.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int expect(bool condition, const char* message)
{
    if (!condition) {
        return fail(message);
    }
    return 0;
}

int roundTripsUtf8AndUtf16()
{
    const std::wstring wideValue = L"RelayDesk 中文";
    const std::string utf8Value = "RelayDesk 中文";

    if (const int result = expect(relaydesk::platform::wideToUtf8(wideValue) == utf8Value,
                                  "UTF-16 to UTF-8 conversion mismatch.");
        result != 0) {
        return result;
    }

    return expect(relaydesk::platform::utf8ToWide(utf8Value) == wideValue,
                  "UTF-8 to UTF-16 conversion mismatch.");
}

int convertsEmptyStrings()
{
    if (const int result = expect(relaydesk::platform::wideToUtf8(L"").empty(),
                                  "Empty UTF-16 string did not convert to empty UTF-8.");
        result != 0) {
        return result;
    }

    return expect(relaydesk::platform::utf8ToWide("").empty(),
                  "Empty UTF-8 string did not convert to empty UTF-16.");
}

int rejectsInvalidUtf8()
{
    std::string invalidUtf8;
    invalidUtf8.push_back(static_cast<char>(128));

    try {
        static_cast<void>(relaydesk::platform::utf8ToWide(invalidUtf8));
    } catch (const std::runtime_error&) {
        return 0;
    }

    return fail("Invalid UTF-8 string was accepted.");
}

} // namespace

int main()
{
    if (const int result = roundTripsUtf8AndUtf16(); result != 0) {
        return result;
    }

    if (const int result = convertsEmptyStrings(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidUtf8(); result != 0) {
        return result;
    }

    return 0;
}
