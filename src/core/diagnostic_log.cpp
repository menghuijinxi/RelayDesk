#include "core/diagnostic_log.h"

#include "core/time.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace relaydesk::core {
namespace {

std::mutex& diagnosticLogMutex()
{
    static std::mutex mutex;
    return mutex;
}

} // namespace

void appendDiagnosticLogLine(const std::filesystem::path& filePath,
                             const std::string& message) noexcept
{
    try {
        std::lock_guard lock(diagnosticLogMutex());
        std::filesystem::create_directories(filePath.parent_path());
        std::ofstream output(filePath, std::ios::binary | std::ios::app);
        output << currentUtcTimestamp() << ' ' << message << '\n';
    } catch (...) {
    }
}

}
