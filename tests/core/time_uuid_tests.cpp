#include "core/time.h"
#include "core/uuid.h"

#include <chrono>
#include <cctype>
#include <iostream>
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

bool isUuidHexDigit(char value)
{
    return std::isxdigit(static_cast<unsigned char>(value)) != 0;
}

bool hasUuidV4Shape(const std::string& value)
{
    if (value.size() != 36) {
        return false;
    }

    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') {
                return false;
            }
            continue;
        }

        if (!isUuidHexDigit(value[index])) {
            return false;
        }
    }

    const char variant = value[19];
    return value[14] == '4'
        && (variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
}

int formatsUnixEpochAsUtc()
{
    const auto epoch = std::chrono::system_clock::from_time_t(0);
    return expect(relaydesk::core::formatUtcTimestamp(epoch) == "1970-01-01T00:00:00Z",
                  "Unix epoch timestamp format mismatch.");
}

int createsUuidV4()
{
    const std::string first = relaydesk::core::createUuidV4();
    const std::string second = relaydesk::core::createUuidV4();

    if (const int result = expect(hasUuidV4Shape(first), "First UUID v4 shape mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(hasUuidV4Shape(second), "Second UUID v4 shape mismatch.");
        result != 0) {
        return result;
    }

    return expect(first != second, "Two generated UUID v4 values should differ.");
}

} // namespace

int main()
{
    if (const int result = formatsUnixEpochAsUtc(); result != 0) {
        return result;
    }

    if (const int result = createsUuidV4(); result != 0) {
        return result;
    }

    return 0;
}
