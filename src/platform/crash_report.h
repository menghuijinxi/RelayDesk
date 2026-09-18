#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace relaydesk::platform {

// minidump 的详细程度。发布版本默认记录完整内存，因为 RelayDesk 的崩溃大多发生在
// 渲染线程和工作线程中，缺少堆内存时很难还原当时的数据状态。
enum class CrashDumpType {
    Normal,
    WithDataSegments,
    Full,
};

class CrashReportConfiguration {
public:
    const std::filesystem::path& GetCrashDirectory() const
    {
        return crashDirectory_;
    }
    const std::filesystem::path& GetLogsDirectory() const { return logsDirectory_; }
    const std::filesystem::path& GetReporterExecutablePath() const
    {
        return reporterExecutablePath_;
    }
    const std::string& GetApplicationVersion() const { return applicationVersion_; }
    const std::string& GetBuildConfiguration() const { return buildConfiguration_; }
    const std::string& GetBuildTimestamp() const { return buildTimestamp_; }
    int GetMaximumRetainedCrashes() const { return maximumRetainedCrashes_; }
    int GetRecentLogLineCount() const { return recentLogLineCount_; }
    bool GetWriteHelperDump() const { return writeHelperDump_; }
    CrashDumpType GetDumpType() const { return dumpType_; }

    void SetCrashDirectory(std::filesystem::path crashDirectory)
    {
        crashDirectory_ = std::move(crashDirectory);
    }
    void SetLogsDirectory(std::filesystem::path logsDirectory)
    {
        logsDirectory_ = std::move(logsDirectory);
    }
    // 崩溃采集完成后要拉起的“崩溃处理程序”。它必须是与聊天程序同一个 exe：
    // 采集侧只负责落盘和启动，是否上传由该进程弹窗询问用户后决定。
    void SetReporterExecutablePath(std::filesystem::path reporterExecutablePath)
    {
        reporterExecutablePath_ = std::move(reporterExecutablePath);
    }
    void SetApplicationVersion(std::string applicationVersion)
    {
        applicationVersion_ = std::move(applicationVersion);
    }
    void SetBuildConfiguration(std::string buildConfiguration)
    {
        buildConfiguration_ = std::move(buildConfiguration);
    }
    void SetBuildTimestamp(std::string buildTimestamp)
    {
        buildTimestamp_ = std::move(buildTimestamp);
    }
    void SetMaximumRetainedCrashes(int maximumRetainedCrashes)
    {
        maximumRetainedCrashes_ = maximumRetainedCrashes;
    }
    void SetRecentLogLineCount(int recentLogLineCount)
    {
        recentLogLineCount_ = recentLogLineCount;
    }
    void SetWriteHelperDump(bool writeHelperDump)
    {
        writeHelperDump_ = writeHelperDump;
    }
    void SetDumpType(CrashDumpType dumpType) { dumpType_ = dumpType; }

protected:
    std::filesystem::path crashDirectory_;
    std::filesystem::path logsDirectory_;
    std::filesystem::path reporterExecutablePath_;
    std::string applicationVersion_;
    std::string buildConfiguration_;
    std::string buildTimestamp_;
    int maximumRetainedCrashes_ = 10;
    int recentLogLineCount_ = 200;
    // Windows 错误报告的 LocalDumps 兜底转储。默认关闭：它会在同一进程写第二份完整
    // 内存转储，占用额外磁盘和时间，只在排查“崩溃发生在采集模块内部”这类极端情况时开启。
    bool writeHelperDump_ = false;
    CrashDumpType dumpType_ = CrashDumpType::Full;
};

// 上一次运行留下的崩溃报告。崩溃目录名本身就带时间与版本，无需再解析目录内容。
class CrashReportEntry {
public:
    const std::filesystem::path& GetDirectory() const { return directory_; }
    const std::string& GetReportId() const { return reportId_; }

    void SetDirectory(std::filesystem::path directory)
    {
        directory_ = std::move(directory);
    }
    void SetReportId(std::string reportId) { reportId_ = std::move(reportId); }

protected:
    std::filesystem::path directory_;
    std::string reportId_;
};

void installCrashHandler(const CrashReportConfiguration& configuration);
bool isCrashHandlerInstalled();

// 把当前正在做的事记进固定大小的环形缓冲区，崩溃报告会带上这段上下文。
// 该函数在任意线程、任意时刻都必须可安全调用，绝不分配内存、不加锁、不抛异常。
void recordCrashActivity(const std::string& activity) noexcept;

// 扫描崩溃目录里尚未处理的报告，按时间从旧到新返回。异常路径之外的普通函数，
// 可以与标准库容器和异常一起使用。
std::vector<CrashReportEntry> findPendingCrashReports(
    const std::filesystem::path& crashDirectory);

// 把报告标记为已处理：删除转储与报告内容，只保留处理标记，避免下次启动重复提示。
void markCrashReportUploaded(const std::filesystem::path& reportDirectory,
                             const std::string& uploadedAt);
void clearCrashReport(const std::filesystem::path& reportDirectory);

}
