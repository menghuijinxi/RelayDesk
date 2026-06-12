#include "core/time.h"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace relaydesk::core {

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

std::string currentUtcTimestamp()
{
    return formatUtcTimestamp(std::chrono::system_clock::now());
}

}
