#pragma once

#include <chrono>
#include <string>

namespace relaydesk::core {

std::string formatUtcTimestamp(std::chrono::system_clock::time_point timePoint);
std::chrono::system_clock::time_point parseUtcTimestamp(const std::string& timestamp);
std::string formatLocalTimeOfDay(std::chrono::system_clock::time_point timePoint);
std::string formatLocalMessageTimestamp(
    std::chrono::system_clock::time_point timePoint,
    std::chrono::system_clock::time_point currentTime);
std::string formatUtcTimestampAsLocalTimeOfDay(const std::string& timestamp);
std::string formatUtcTimestampAsLocalMessageTimestamp(const std::string& timestamp);
std::string currentUtcTimestamp();

}
