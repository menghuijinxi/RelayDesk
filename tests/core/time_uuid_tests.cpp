#include "core/time.h"
#include "core/uuid.h"

#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
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

std::string expectedLocalTime(const std::string& timestamp, const char* format)
{
    const std::chrono::system_clock::time_point timePoint =
        relaydesk::core::parseUtcTimestamp(timestamp);
    const std::time_t rawTime =
        std::chrono::system_clock::to_time_t(timePoint);
    std::tm localTime{};
#if defined(_WIN32)
    if (localtime_s(&localTime, &rawTime) != 0) {
        throw std::runtime_error("Failed to convert timestamp to local time.");
    }
#else
    if (localtime_r(&rawTime, &localTime) == nullptr) {
        throw std::runtime_error("Failed to convert timestamp to local time.");
    }
#endif
    std::ostringstream output;
    output << std::put_time(&localTime, format);
    return output.str();
}

std::string expectedLocalTimeOfDay(const std::string& timestamp)
{
    return expectedLocalTime(timestamp, "%H:%M");
}

int formatsUnixEpochAsUtc()
{
    const auto epoch = std::chrono::system_clock::from_time_t(0);
    return expect(relaydesk::core::formatUtcTimestamp(epoch) == "1970-01-01T00:00:00Z",
                  "Unix epoch timestamp format mismatch.");
}

int parsesUtcTimestamp()
{
    const auto parsed =
        relaydesk::core::parseUtcTimestamp("1970-01-02T03:04:05Z");
    return expect(relaydesk::core::formatUtcTimestamp(parsed)
                      == "1970-01-02T03:04:05Z",
                  "UTC timestamp parse mismatch.");
}

int formatsUtcTimestampAsLocalTimeOfDay()
{
    constexpr const char* kTimestamp = "2026-08-24T07:49:00Z";
    return expect(relaydesk::core::formatUtcTimestampAsLocalTimeOfDay(kTimestamp)
                      == expectedLocalTimeOfDay(kTimestamp),
                  "UTC timestamp local time-of-day format mismatch.");
}

int formatsMessageTimestampWithRelativeAndYearAwareDate()
{
    const std::chrono::system_clock::time_point currentTime =
        relaydesk::core::parseUtcTimestamp("2026-09-14T12:00:00Z");
    constexpr const char* kTodayTimestamp = "2026-09-14T12:34:00Z";
    constexpr const char* kYesterdayTimestamp = "2026-09-13T12:34:00Z";
    constexpr const char* kDayBeforeYesterdayTimestamp =
        "2026-09-12T12:34:00Z";
    constexpr const char* kSameYearTimestamp = "2026-02-03T12:34:00Z";
    constexpr const char* kPreviousYearTimestamp = "2025-07-09T12:34:00Z";

    const auto today = relaydesk::core::parseUtcTimestamp(kTodayTimestamp);
    const auto yesterday = relaydesk::core::parseUtcTimestamp(
        kYesterdayTimestamp);
    const auto dayBeforeYesterday = relaydesk::core::parseUtcTimestamp(
        kDayBeforeYesterdayTimestamp);
    const auto sameYear = relaydesk::core::parseUtcTimestamp(
        kSameYearTimestamp);
    const auto previousYear = relaydesk::core::parseUtcTimestamp(
        kPreviousYearTimestamp);

    if (const int result = expect(
            relaydesk::core::formatLocalMessageTimestamp(
                today, currentTime) ==
                "今天 " + expectedLocalTimeOfDay(kTodayTimestamp),
            "Today message timestamp format mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(
            relaydesk::core::formatLocalMessageTimestamp(
                yesterday, currentTime) ==
                "昨天 " + expectedLocalTimeOfDay(kYesterdayTimestamp),
            "Yesterday message timestamp format mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(
            relaydesk::core::formatLocalMessageTimestamp(
                dayBeforeYesterday, currentTime) ==
                "前天 " + expectedLocalTimeOfDay(
                    kDayBeforeYesterdayTimestamp),
            "Day-before-yesterday message timestamp format mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(
            relaydesk::core::formatLocalMessageTimestamp(
                sameYear, currentTime) ==
                expectedLocalTime(kSameYearTimestamp, "%m-%d %H:%M"),
            "Same-year message timestamp format mismatch.");
        result != 0) {
        return result;
    }

    return expect(relaydesk::core::formatLocalMessageTimestamp(
                      previousYear, currentTime) ==
                      expectedLocalTime(kPreviousYearTimestamp,
                                        "%Y-%m-%d %H:%M"),
                  "Previous-year message timestamp format mismatch.");
}

int rejectsInvalidUtcTimestamp()
{
    try {
        static_cast<void>(
            relaydesk::core::parseUtcTimestamp("1970-13-02T03:04:05Z"));
    } catch (const std::runtime_error&) {
        return 0;
    }

    return fail("Invalid UTC timestamp was accepted.");
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

    if (const int result = parsesUtcTimestamp(); result != 0) {
        return result;
    }

    if (const int result = formatsUtcTimestampAsLocalTimeOfDay(); result != 0) {
        return result;
    }

    if (const int result = formatsMessageTimestampWithRelativeAndYearAwareDate();
        result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidUtcTimestamp(); result != 0) {
        return result;
    }

    if (const int result = createsUuidV4(); result != 0) {
        return result;
    }

    return 0;
}
