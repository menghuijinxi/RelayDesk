#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace relaydesk::platform {

// 询问用户是否上传时使用的界面：优先桌面弹窗，没有桌面时回退控制台菜单。
enum class CrashReporterPromptMode {
    Ui,
    Console,
};

class CrashReporterOptions {
public:
    const std::filesystem::path& GetCrashDirectory() const
    {
        return crashDirectory_;
    }
    CrashReporterPromptMode GetPromptMode() const { return promptMode_; }
    bool GetAutoUpload() const { return autoUpload_; }
    const std::string& GetReportId() const { return reportId_; }

    void SetCrashDirectory(std::filesystem::path crashDirectory)
    {
        crashDirectory_ = std::move(crashDirectory);
    }
    void SetPromptMode(CrashReporterPromptMode promptMode)
    {
        promptMode_ = promptMode;
    }
    void SetAutoUpload(bool autoUpload) { autoUpload_ = autoUpload; }
    void SetReportId(std::string reportId) { reportId_ = std::move(reportId); }

protected:
    std::filesystem::path crashDirectory_;
    CrashReporterPromptMode promptMode_ = CrashReporterPromptMode::Ui;
    bool autoUpload_ = false;
    std::string reportId_;
};

// 崩溃上报是互联网分发能力：只向 MinidumpServer 上传 crash.dmp。
// 局域网对端不是可靠的崩溃收集通道，因此这里不依赖聊天会话或局域网状态。
std::optional<CrashReporterOptions> parseCrashReporterOptions(
    const std::vector<std::wstring>& arguments);

// 崩溃处理模式：由崩溃采集侧在写好转储与日志后立刻用启动参数拉起。它读取指定
// 崩溃目录，弹窗（无桌面时回退控制台）询问用户是否上传，再把 crash.dmp 发到
// MinidumpServer。返回本次是否成功处理；没有匹配报告时返回 false。
bool runCrashReporterMode(const CrashReporterOptions& options);

}
