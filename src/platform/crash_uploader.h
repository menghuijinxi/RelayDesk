#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace relaydesk::platform {

class CrashUploadOptions {
public:
    const std::filesystem::path& GetCrashDirectory() const
    {
        return crashDirectory_;
    }
    const std::string& GetServerUrl() const { return serverUrl_; }
    const std::string& GetApiKey() const { return apiKey_; }
    const std::string& GetReportId() const { return reportId_; }

    void SetCrashDirectory(std::filesystem::path crashDirectory)
    {
        crashDirectory_ = std::move(crashDirectory);
    }
    void SetServerUrl(std::string serverUrl) { serverUrl_ = std::move(serverUrl); }
    void SetApiKey(std::string apiKey) { apiKey_ = std::move(apiKey); }
    void SetReportId(std::string reportId) { reportId_ = std::move(reportId); }

protected:
    std::filesystem::path crashDirectory_;
    std::string serverUrl_;
    std::string apiKey_;
    std::string reportId_;
};

std::optional<CrashUploadOptions> parseCrashUploadOptions(
    const std::vector<std::wstring>& arguments);
int runCrashUploadMode(const CrashUploadOptions& options) noexcept;

}
