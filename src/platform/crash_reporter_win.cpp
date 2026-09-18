#include "platform/crash_reporter.h"

#include "core/app_version.h"
#include "core/time.h"
#include "platform/crash_report.h"
#include "platform/crash_uploader.h"
#include "platform/text_encoding.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace relaydesk::platform {
namespace {

constexpr wchar_t kCrashReporterArgument[] = L"--relaydesk-crash-reporter";
constexpr wchar_t kCrashConsoleArgument[] = L"--relaydesk-crash-console";
constexpr wchar_t kCrashAutoUploadArgument[] = L"--relaydesk-crash-auto-upload";
constexpr wchar_t kCrashDirectoryArgument[] = L"--crash-directory";
constexpr wchar_t kCrashReportIdArgument[] = L"--crash-report-id";
constexpr const char* kDumpFileName = "crash.dmp";
constexpr wchar_t kCrashConsoleFallbackArgument[] =
    L"--relaydesk-crash-console-fallback";

void appendQuotedArgument(std::wstring& commandLine, std::wstring_view value)
{
    if (!commandLine.empty()) {
        commandLine.push_back(L' ');
    }
    commandLine.push_back(L'"');
    for (const wchar_t character : value) {
        if (character == L'"') {
            commandLine.push_back(L'\\');
        }
        commandLine.push_back(character);
    }
    commandLine.push_back(L'"');
}

std::optional<std::wstring> currentExecutablePath()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size() - 1) {
        return std::nullopt;
    }
    return std::wstring(buffer.data(), length);
}

std::string formatBytes(std::uintmax_t bytes)
{
    constexpr double kKilobyte = 1024.0;
    constexpr double kMegabyte = kKilobyte * 1024.0;
    char buffer[64] = {};
    if (static_cast<double>(bytes) >= kMegabyte) {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%.1f MB",
                      static_cast<double>(bytes) / kMegabyte);
    } else if (static_cast<double>(bytes) >= kKilobyte) {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%.0f KB",
                      static_cast<double>(bytes) / kKilobyte);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::uintmax_t fileSize(const std::filesystem::path& path)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

struct PendingCrash {
    CrashReportEntry entry;
    std::filesystem::path dumpPath;
    std::uintmax_t dumpBytes = 0;
};

std::vector<PendingCrash> selectPendingCrashes(
    const std::filesystem::path& crashDirectory,
    const std::string& reportId)
{
    std::vector<PendingCrash> result;
    for (const CrashReportEntry& entry : findPendingCrashReports(crashDirectory)) {
        if (!reportId.empty() && entry.GetReportId() != reportId) {
            continue;
        }
        PendingCrash crash;
        crash.entry = entry;
        crash.dumpPath = entry.GetDirectory() / kDumpFileName;
        crash.dumpBytes = fileSize(crash.dumpPath);
        result.push_back(std::move(crash));
    }
    return result;
}

bool hasUploadConfiguration(const std::filesystem::path& crashDirectory)
{
    return parseCrashUploadOptions(
               {L"--relaydesk-upload-crash-reports",
                std::wstring(kCrashDirectoryArgument),
                crashDirectory.wstring()})
        .has_value();
}

bool uploadReports(const std::filesystem::path& crashDirectory,
                   const std::string& reportId)
{
    const std::optional<std::wstring> executablePath = currentExecutablePath();
    if (!executablePath.has_value()) {
        return false;
    }

    std::wstring commandLine;
    commandLine.reserve(1024);
    appendQuotedArgument(commandLine, *executablePath);
    appendQuotedArgument(commandLine, L"--relaydesk-upload-crash-reports");
    appendQuotedArgument(commandLine, kCrashDirectoryArgument);
    appendQuotedArgument(commandLine, crashDirectory.wstring());
    if (!reportId.empty()) {
        appendQuotedArgument(commandLine, kCrashReportIdArgument);
        appendQuotedArgument(commandLine, utf8ToWide(reportId));
    }

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(),
                                             commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(nullptr,
                         mutableCommandLine.data(),
                         nullptr,
                         nullptr,
                         FALSE,
                         0,
                         nullptr,
                         nullptr,
                         &startupInfo,
                         &processInfo)) {
        return false;
    }

    CloseHandle(processInfo.hThread);
    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    const bool readExitCode =
        waitResult == WAIT_OBJECT_0
        && GetExitCodeProcess(processInfo.hProcess, &exitCode) != FALSE;
    CloseHandle(processInfo.hProcess);
    return readExitCode && exitCode == 0;
}

void showConsoleFallbackMessage(const std::filesystem::path& crashDirectory,
                                std::string_view error)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        (void)AllocConsole();
    }
    std::FILE* output = nullptr;
    (void)freopen_s(&output, "CONOUT$", "w", stdout);
    (void)freopen_s(&output, "CONOUT$", "w", stderr);

    std::cout << "RelayDesk 崩溃报告处理失败。\n";
    std::cout << "崩溃目录: " << crashDirectory.string() << "\n";
    if (!error.empty()) {
        std::cout << "原因: " << error << "\n";
    }
    std::cout << "请将 data/crashes 下的崩溃目录交给开发者。\n";
}

bool promptUi(const std::filesystem::path& crashDirectory,
              const std::vector<PendingCrash>& crashes)
{
    if (crashes.empty()) {
        return false;
    }

    const PendingCrash& crash = crashes.front();
    const std::wstring message =
        L"RelayDesk 刚刚发生崩溃。\n\n"
        L"崩溃目录:\n"
        + crash.entry.GetDirectory().wstring()
        + L"\n\n"
          L"转储大小: "
        + utf8ToWide(formatBytes(crash.dumpBytes))
        + L"\n\n"
          L"是否上传崩溃文档到开发者服务器？";

    const int choice = MessageBoxW(nullptr,
                                   message.c_str(),
                                   L"RelayDesk 崩溃报告",
                                   MB_ICONERROR | MB_YESNOCANCEL | MB_DEFBUTTON2);
    if (choice == 0) {
        return false;
    }
    if (choice == IDCANCEL) {
        return true;
    }
    if (choice == IDNO) {
        markCrashReportUploaded(crash.entry.GetDirectory(),
                                relaydesk::core::currentUtcTimestamp());
        return true;
    }
    if (choice == IDYES) {
        if (!hasUploadConfiguration(crashDirectory)) {
            showConsoleFallbackMessage(
                crashDirectory,
                "未配置 crash_upload_url，无法上传；崩溃文档已保留在本地。");
            return true;
        }
        if (uploadReports(crashDirectory, crash.entry.GetReportId())) {
            return true;
        }
        showConsoleFallbackMessage(
            crashDirectory,
            "上传失败；崩溃文档已保留，可在网络恢复后再次启动重试。");
        return true;
    }
    return true;
}

bool promptConsole(const std::filesystem::path& crashDirectory,
                   const std::vector<PendingCrash>& crashes)
{
    if (crashes.empty()) {
        return false;
    }

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        (void)AllocConsole();
    }
    std::FILE* output = nullptr;
    (void)freopen_s(&output, "CONOUT$", "w", stdout);
    (void)freopen_s(&output, "CONOUT$", "w", stderr);
    std::FILE* input = nullptr;
    (void)freopen_s(&input, "CONIN$", "r", stdin);

    const PendingCrash& crash = crashes.front();
    std::cout << "\nRelayDesk 刚刚发生崩溃。\n";
    std::cout << "崩溃目录: " << crash.entry.GetDirectory().string() << "\n";
    std::cout << "转储大小: " << formatBytes(crash.dumpBytes) << "\n";
    std::cout << "请输入 y 上传，n 不上传，q 退出后下次再处理: ";
    std::cout.flush();

    std::string answer;
    std::getline(std::cin, answer);
    if (answer == "q" || answer == "Q") {
        return true;
    }
    if (answer == "n" || answer == "N") {
        markCrashReportUploaded(crash.entry.GetDirectory(),
                                relaydesk::core::currentUtcTimestamp());
        return true;
    }
    if (answer != "y" && answer != "Y") {
        return false;
    }
    if (!hasUploadConfiguration(crashDirectory)) {
        std::cout << "未配置 crash_upload_url，无法上传；崩溃文档保留在本地。\n";
        std::cout << "按回车继续。";
        std::getline(std::cin, answer);
        return true;
    }
    if (uploadReports(crashDirectory, crash.entry.GetReportId())) {
        return true;
    }
    std::cout << "上传失败；崩溃文档已保留，可在网络恢复后再次启动重试。\n";
    std::cout << "按回车继续。";
    std::getline(std::cin, answer);
    return true;
}

} // namespace

std::optional<CrashReporterOptions> parseCrashReporterOptions(
    const std::vector<std::wstring>& arguments)
{
    CrashReporterOptions options;
    bool requested = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == kCrashReporterArgument) {
            requested = true;
        } else if (argument == kCrashConsoleArgument
                   || argument == kCrashConsoleFallbackArgument) {
            options.SetPromptMode(CrashReporterPromptMode::Console);
        } else if (argument == kCrashAutoUploadArgument) {
            options.SetAutoUpload(true);
        } else if (argument == kCrashDirectoryArgument
                   && index + 1 < arguments.size()) {
            options.SetCrashDirectory(std::filesystem::path(arguments[++index]));
        } else if (argument == kCrashReportIdArgument
                   && index + 1 < arguments.size()) {
            options.SetReportId(wideToUtf8(arguments[++index]));
        }
    }
    if (!requested || options.GetCrashDirectory().empty()) {
        return std::nullopt;
    }
    return options;
}

bool runCrashReporterMode(const CrashReporterOptions& options)
{
    const std::vector<PendingCrash> crashes =
        selectPendingCrashes(options.GetCrashDirectory(), options.GetReportId());
    if (crashes.empty()) {
        return false;
    }
    if (options.GetAutoUpload()) {
        return uploadReports(options.GetCrashDirectory(), options.GetReportId());
    }
    if (options.GetPromptMode() == CrashReporterPromptMode::Console) {
        return promptConsole(options.GetCrashDirectory(), crashes);
    }
    if (promptUi(options.GetCrashDirectory(), crashes)) {
        return true;
    }
    // UI 或桌面不可用时不能把用户晾在一边：同一个报告模式继续用控制台菜单。
    return promptConsole(options.GetCrashDirectory(), crashes);
}

}
