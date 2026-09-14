#include "core/time.h"

#include <charconv>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace relaydesk::core {
namespace {

int parseFixedNumber(const std::string& value,
                     std::size_t offset,
                     std::size_t length,
                     const char* fieldName)
{
    const char* first = value.data() + offset;
    const char* last = first + length;
    int result = 0;
    const auto parsed = std::from_chars(first, last, result);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        throw std::runtime_error(std::string("Invalid UTC timestamp field: ")
                                 + fieldName);
    }
    return result;
}

void validateTimestampShape(const std::string& value)
{
    if (value.size() != 20
        || value[4] != '-'
        || value[7] != '-'
        || value[10] != 'T'
        || value[13] != ':'
        || value[16] != ':'
        || value[19] != 'Z') {
        throw std::runtime_error("UTC timestamp must use YYYY-MM-DDTHH:MM:SSZ.");
    }
}

std::tm toLocalTime(std::chrono::system_clock::time_point timePoint)
{
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(timePoint);
    std::tm localTime{};

#if defined(_WIN32)
    if (localtime_s(&localTime, &timestamp) != 0) {
        throw std::runtime_error("Failed to convert timestamp to local time.");
    }
#else
    if (localtime_r(&timestamp, &localTime) == nullptr) {
        throw std::runtime_error("Failed to convert timestamp to local time.");
    }
#endif

    return localTime;
}

std::chrono::sys_days localDate(const std::tm& localTime)
{
    const std::chrono::year_month_day date{
        std::chrono::year{localTime.tm_year + 1900},
        std::chrono::month{static_cast<unsigned int>(localTime.tm_mon + 1)},
        std::chrono::day{static_cast<unsigned int>(localTime.tm_mday)},
    };
    return std::chrono::sys_days{date};
}

std::string formatLocalTime(const std::tm& localTime, const char* format)
{
    std::ostringstream output;
    output << std::put_time(&localTime, format);
    return output.str();
}

} // namespace

std::string formatUtcTimestamp(std::chrono::system_clock::time_point timePoint)
{
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(timePoint);
    std::tm utcTime{};

#if defined(_WIN32)
    if (gmtime_s(&utcTime, &timestamp) != 0) {
        throw std::runtime_error("Failed to convert timestamp to UTC.");
    }
#else
    if (gmtime_r(&timestamp, &utcTime) == nullptr) {
        throw std::runtime_error("Failed to convert timestamp to UTC.");
    }
#endif

    std::ostringstream output;
    output << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::chrono::system_clock::time_point parseUtcTimestamp(const std::string& timestamp)
{
    validateTimestampShape(timestamp);

    const int yearValue = parseFixedNumber(timestamp, 0, 4, "year");
    const int monthValue = parseFixedNumber(timestamp, 5, 2, "month");
    const int dayValue = parseFixedNumber(timestamp, 8, 2, "day");
    const int hourValue = parseFixedNumber(timestamp, 11, 2, "hour");
    const int minuteValue = parseFixedNumber(timestamp, 14, 2, "minute");
    const int secondValue = parseFixedNumber(timestamp, 17, 2, "second");

    if (hourValue < 0 || hourValue > 23
        || minuteValue < 0 || minuteValue > 59
        || secondValue < 0 || secondValue > 59) {
        throw std::runtime_error("UTC timestamp time fields are out of range.");
    }

    const std::chrono::year_month_day date{
        std::chrono::year{yearValue},
        std::chrono::month{static_cast<unsigned int>(monthValue)},
        std::chrono::day{static_cast<unsigned int>(dayValue)},
    };
    if (!date.ok()) {
        throw std::runtime_error("UTC timestamp date fields are out of range.");
    }

    const auto timePoint = std::chrono::sys_days{date}
        + std::chrono::hours{hourValue}
        + std::chrono::minutes{minuteValue}
        + std::chrono::seconds{secondValue};
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        timePoint);
}

std::string formatLocalTimeOfDay(std::chrono::system_clock::time_point timePoint)
{
    const std::tm localTime = toLocalTime(timePoint);
    return formatLocalTime(localTime, "%H:%M");
}

std::string formatLocalMessageTimestamp(
    std::chrono::system_clock::time_point timePoint,
    std::chrono::system_clock::time_point currentTime)
{
    const std::tm messageLocalTime = toLocalTime(timePoint);
    const std::tm currentLocalTime = toLocalTime(currentTime);
    const std::chrono::sys_days messageDate = localDate(messageLocalTime);
    const std::chrono::sys_days currentDate = localDate(currentLocalTime);

    if (messageDate == currentDate) {
        return "今天 " + formatLocalTime(messageLocalTime, "%H:%M");
    }
    if (messageDate == currentDate - std::chrono::days{1}) {
        return "昨天 " + formatLocalTime(messageLocalTime, "%H:%M");
    }
    if (messageDate == currentDate - std::chrono::days{2}) {
        return "前天 " + formatLocalTime(messageLocalTime, "%H:%M");
    }
    if (messageLocalTime.tm_year == currentLocalTime.tm_year) {
        return formatLocalTime(messageLocalTime, "%m-%d %H:%M");
    }
    return formatLocalTime(messageLocalTime, "%Y-%m-%d %H:%M");
}

std::string formatUtcTimestampAsLocalTimeOfDay(const std::string& timestamp)
{
    return formatLocalTimeOfDay(parseUtcTimestamp(timestamp));
}

std::string formatUtcTimestampAsLocalMessageTimestamp(
    const std::string& timestamp)
{
    return formatLocalMessageTimestamp(parseUtcTimestamp(timestamp),
                                       std::chrono::system_clock::now());
}

std::string currentUtcTimestamp()
{
    return formatUtcTimestamp(std::chrono::system_clock::now());
}

}
