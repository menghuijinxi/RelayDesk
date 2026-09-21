#include "core/app_version.h"
#include "platform/crash_report.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace {

constexpr wchar_t kCrashReporterArgument[] = L"--relaydesk-crash-reporter";
constexpr wchar_t kCrashDirectoryArgument[] = L"--crash-directory";
constexpr wchar_t kCrashReportIdArgument[] = L"--crash-report-id";
constexpr wchar_t kSilentEnvironmentVariable[] =
    L"RELAYDESK_CRASH_DUMP_TEST_SILENT";

std::vector<std::wstring> currentProcessArguments()
{
    int argumentCount = 0;
    wchar_t** rawArguments =
        CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (rawArguments == nullptr) {
        return {};
    }

    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<std::size_t>(argumentCount));
    for (int index = 1; index < argumentCount; ++index) {
        arguments.emplace_back(rawArguments[index]);
    }
    LocalFree(rawArguments);
    return arguments;
}

std::filesystem::path currentExecutablePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::optional<std::wstring> argumentValue(
    const std::vector<std::wstring>& arguments,
    const std::wstring& name)
{
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == name) {
            return arguments[index + 1];
        }
    }
    return std::nullopt;
}

bool containsArgument(const std::vector<std::wstring>& arguments,
                      const std::wstring& name)
{
    for (const std::wstring& argument : arguments) {
        if (argument == name) {
            return true;
        }
    }
    return false;
}

bool silentReporterRequested()
{
    std::array<wchar_t, 2> value{};
    return GetEnvironmentVariableW(kSilentEnvironmentVariable,
                                   value.data(),
                                   static_cast<DWORD>(value.size()))
        != 0;
}

int showCrashReportReady(const std::vector<std::wstring>& arguments)
{
    if (silentReporterRequested()) {
        return 0;
    }

    const std::optional<std::wstring> crashDirectory =
        argumentValue(arguments, kCrashDirectoryArgument);
    const std::optional<std::wstring> reportId =
        argumentValue(arguments, kCrashReportIdArgument);

    std::filesystem::path reportDirectory =
        crashDirectory.has_value()
        ? std::filesystem::path(crashDirectory.value())
        : currentExecutablePath().parent_path()
            / "crash-dump-test-data"
            / "crashes";
    if (reportId.has_value()) {
        reportDirectory /= L"crash-" + reportId.value();
    }

    const std::wstring message =
        L"测试崩溃转储已经生成。\n\n"
        L"请把下面整个目录复制回开发电脑：\n"
        + reportDirectory.wstring()
        + L"\n\n远端电脑不需要也不应放置 PDB 文件。";
    MessageBoxW(nullptr,
                message.c_str(),
                L"RelayDesk DMP 符号测试",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    return 0;
}

void installTestCrashHandler()
{
    const std::filesystem::path executablePath = currentExecutablePath();
    const std::filesystem::path dataDirectory =
        executablePath.parent_path() / "crash-dump-test-data";

    relaydesk::platform::CrashReportConfiguration configuration;
    configuration.SetCrashDirectory(dataDirectory / "crashes");
    configuration.SetLogsDirectory(dataDirectory / "logs");
    configuration.SetReporterExecutablePath(executablePath);
    configuration.SetApplicationVersion(
        std::to_string(relaydesk::core::kAppVersion));
#if defined(RELAYDESK_CRASH_TEST_BUILD_CONFIGURATION)
    configuration.SetBuildConfiguration(
        RELAYDESK_CRASH_TEST_BUILD_CONFIGURATION);
#endif
#if defined(RELAYDESK_CRASH_TEST_BUILD_TIMESTAMP)
    configuration.SetBuildTimestamp(RELAYDESK_CRASH_TEST_BUILD_TIMESTAMP);
#endif
    configuration.SetMaximumRetainedCrashes(5);
    configuration.SetRecentLogLineCount(50);
    configuration.SetDumpType(relaydesk::platform::CrashDumpType::Full);
    relaydesk::platform::installCrashHandler(configuration);
}

__declspec(noinline) int crashDumpTestInnermostFrame(std::uintptr_t address)
{
    volatile int* invalidAddress = reinterpret_cast<volatile int*>(address);
    *invalidAddress = 0x52444D50;
    return *invalidAddress;
}

__declspec(noinline) int crashDumpTestMiddleFrame(std::uintptr_t address)
{
    const int value = crashDumpTestInnermostFrame(address);
    return value + 1;
}

__declspec(noinline) int crashDumpTestOutermostFrame()
{
    const std::uintptr_t invalidAddress = 1;
    const int value = crashDumpTestMiddleFrame(invalidAddress);
    return value + 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::vector<std::wstring> arguments = currentProcessArguments();
    if (containsArgument(arguments, kCrashReporterArgument)) {
        return showCrashReportReady(arguments);
    }

    installTestCrashHandler();
    relaydesk::platform::recordCrashActivity(
        "crash_dump_test.trigger expected_stack="
        "crashDumpTestOutermostFrame>crashDumpTestMiddleFrame>"
        "crashDumpTestInnermostFrame");
    return crashDumpTestOutermostFrame();
}
