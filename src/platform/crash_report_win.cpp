#include "platform/crash_report.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <fstream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#include <signal.h>

// RtlCaptureStackBackTrace 在 winnt.h 中声明，但部分 SDK 组合下并不会随
// windows.h 一起暴露出来，这里显式声明以免依赖头文件内部细节。
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
    unsigned long framesToSkip,
    unsigned long framesToCapture,
    void** backTrace,
    unsigned long* backTraceHash);

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace relaydesk::platform {
namespace {

using MiniDumpWriteDumpFunction = BOOL(WINAPI*)(HANDLE,
                                                DWORD,
                                                HANDLE,
                                                MINIDUMP_TYPE,
                                                PMINIDUMP_EXCEPTION_INFORMATION,
                                                PMINIDUMP_USER_STREAM_INFORMATION,
                                                PMINIDUMP_CALLBACK_INFORMATION);

// 崩溃处理器运行在已经损坏的进程里，所有状态都必须是预先分配好的固定内存：不能用
// 标准容器，也不能临时分配，否则分配失败会演变成二次崩溃。下面这些全局量在安装
// 阶段写好，崩溃路径只读不写。
constexpr std::size_t kMaximumPathTextLength = 1024;
constexpr std::size_t kMaximumBuildTextLength = 128;
constexpr std::size_t kMaximumDetailTextLength = 512;
constexpr std::size_t kMaximumActivityBytes = 32 * 1024;
constexpr std::size_t kMaximumActivityEntryBytes = 512;
constexpr std::size_t kMaximumModuleCount = 512;
constexpr std::size_t kMaximumLogSnapshotFiles = 64;
constexpr std::size_t kMaximumFileCopyBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumTailReadBytes = 1024 * 1024;
constexpr std::size_t kMaximumReportBytes = 512 * 1024;
constexpr unsigned short kMaximumCaptureFrames = 96;
constexpr const char* kReportIdPrefix = "crash-";
constexpr const char* kReportFileName = "report.txt";
constexpr const char* kMetadataFileName = "metadata.txt";
constexpr const char* kDumpFileName = "crash.dmp";
constexpr const char* kLogSnapshotDirectoryName = "logs";
constexpr const char* kDiscoveryLogFileName = "discovery.log";
constexpr const char* kSymbolizationHintFileName = "symbolize.txt";
constexpr const char* kUploadMarkerFileName = "uploaded.txt";

// 崩溃采集完成后拉起同一个 exe 的崩溃处理模式。参数名必须与
// crash_reporter_win.cpp 的解析逻辑保持一致。
constexpr wchar_t kCrashReporterArgument[] = L"--relaydesk-crash-reporter";
constexpr wchar_t kCrashDirectoryArgument[] = L"--crash-directory";
constexpr wchar_t kCrashReportIdArgument[] = L"--crash-report-id";

// 自定义异常码：这些路径没有真正的结构化异常记录，用 0xE00000xx 段标记来源。
constexpr DWORD kUncaughtCxxExceptionCode = 0xE0000002;
constexpr DWORD kPureVirtualCallCode = 0xE0000003;
constexpr DWORD kInvalidParameterCode = 0xE0000004;
constexpr DWORD kCrtAssertOrErrorCode = 0xE0000005;
constexpr DWORD kCRuntimeSignalCode = 0xE0000006;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_captureInProgress{false};
char g_crashDirectory[kMaximumPathTextLength] = {};
char g_logsDirectory[kMaximumPathTextLength] = {};
char g_applicationVersion[kMaximumBuildTextLength] = {};
char g_buildConfiguration[kMaximumBuildTextLength] = {};
char g_buildTimestamp[kMaximumBuildTextLength] = {};
// 崩溃处理程序（同一个 exe）的绝对路径，安装阶段预先取好。
wchar_t g_reporterExecutablePath[kMaximumPathTextLength] = {};
int g_maximumRetainedCrashes = 10;
int g_recentLogLineCount = 200;
MINIDUMP_TYPE g_dumpType = MiniDumpNormal;
bool g_writeHelperDump = false;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
std::terminate_handler g_previousTerminateHandler = nullptr;
_invalid_parameter_handler g_previousInvalidParameterHandler = nullptr;
#if defined(_MSC_VER)
_purecall_handler g_previousPurecallHandler = nullptr;
#endif
PVOID g_vectoredHandler = nullptr;

// 崩溃线程的异常上下文，供拿不到 EXCEPTION_POINTERS 的回调使用。它只用于补充
// 说明性信息，不参与崩溃判定的正确性。
EXCEPTION_POINTERS* volatile g_pendingException = nullptr;

struct CrashCaptureContext {
    EXCEPTION_RECORD exceptionRecord{};
    CONTEXT threadContext{};
    DWORD processId = 0;
    DWORD threadId = 0;
};

struct ReportWriteContext {
    CrashCaptureContext capture{};
    char detail[kMaximumDetailTextLength] = {};
};

// ---------------------------------------------------------------------------
// SEH 保护壳
// ---------------------------------------------------------------------------
//
// 采集过程中任何一个辅助步骤再出错，都必须安静退出，而不是让进程陷入反复重入。
// MSVC 禁止在含 C++ 析构对象的函数里直接使用 __try（C2712），因此把 __try 收敛到
// 下面这个纯 POD 的 thunk：需要保护的逻辑以函数指针传入，内部可以正常使用标准库。

struct ProtectedCall {
    void (*invoke)(void*) = nullptr;
    void* context = nullptr;
};

int runProtectedThunk(void* rawCall)
{
    const auto* call = static_cast<const ProtectedCall*>(rawCall);
    __try {
        call->invoke(call->context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

template <typename Context>
bool runProtected(void (*invoke)(Context*), Context* context)
{
    static_assert(std::is_trivially_copyable_v<Context>,
                  "受 SEH 保护的上下文必须是平凡可复制类型，不能带析构逻辑");
    ProtectedCall call;
    call.invoke = reinterpret_cast<void (*)(void*)>(invoke);
    call.context = context;
    return runProtectedThunk(&call) == 0;
}

// ---------------------------------------------------------------------------
// 崩溃活动上下文
// ---------------------------------------------------------------------------

CRITICAL_SECTION& activityCriticalSection()
{
    static CRITICAL_SECTION section{};
    static const bool initialized = []() {
        InitializeCriticalSection(&section);
        return true;
    }();
    (void)initialized;
    return section;
}

char g_activityBuffer[kMaximumActivityBytes] = {};
std::atomic<std::size_t> g_activitySize{0};

// ---------------------------------------------------------------------------
// 文本工具
// ---------------------------------------------------------------------------

std::wstring utf8ToWide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int requiredSize = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (requiredSize <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(requiredSize), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        requiredSize);
    return result;
}

std::string wideToUtf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int requiredSize = WideCharToMultiByte(CP_UTF8,
                                                 0,
                                                 text.data(),
                                                 static_cast<int>(text.size()),
                                                 nullptr,
                                                 0,
                                                 nullptr,
                                                 nullptr);
    if (requiredSize <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(requiredSize), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        requiredSize,
                        nullptr,
                        nullptr);
    return result;
}

bool writeFileContents(const std::filesystem::path& filePath,
                       const std::string& contents)
{
    HANDLE file = CreateFileW(filePath.c_str(),
                              GENERIC_WRITE,
                              FILE_SHARE_READ,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool succeeded = true;
    std::size_t offset = 0;
    while (offset < contents.size() && succeeded) {
        const std::size_t chunk =
            std::min<std::size_t>(contents.size() - offset, 64 * 1024);
        DWORD written = 0;
        succeeded = WriteFile(file,
                              contents.data() + offset,
                              static_cast<DWORD>(chunk),
                              &written,
                              nullptr)
            != FALSE
            && written == chunk;
        offset += chunk;
    }
    CloseHandle(file);
    return succeeded;
}

std::string formatReportId()
{
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);

    char buffer[32] = {};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04u%02u%02u-%02u%02u%02u",
                  static_cast<unsigned int>(localTime.wYear),
                  static_cast<unsigned int>(localTime.wMonth),
                  static_cast<unsigned int>(localTime.wDay),
                  static_cast<unsigned int>(localTime.wHour),
                  static_cast<unsigned int>(localTime.wMinute),
                  static_cast<unsigned int>(localTime.wSecond));
    return buffer;
}

std::string formatSystemTime(const SYSTEMTIME& value)
{
    char buffer[40] = {};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04u-%02u-%02uT%02u:%02u:%02u",
                  static_cast<unsigned int>(value.wYear),
                  static_cast<unsigned int>(value.wMonth),
                  static_cast<unsigned int>(value.wDay),
                  static_cast<unsigned int>(value.wHour),
                  static_cast<unsigned int>(value.wMinute),
                  static_cast<unsigned int>(value.wSecond));
    return buffer;
}

std::string currentUtcTimestampText()
{
    SYSTEMTIME utcTime{};
    GetSystemTime(&utcTime);
    return formatSystemTime(utcTime) + "Z";
}

std::string currentLocalTimestampText()
{
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    return formatSystemTime(localTime);
}

std::string formatHex(std::uint64_t value)
{
    char buffer[32] = {};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "0x%llX",
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::string formatDecimal(std::uint64_t value)
{
    char buffer[32] = {};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%llu",
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::string describeExceptionCode(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:
        return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:
        return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:
        return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:
        return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_GUARD_PAGE:
        return "EXCEPTION_GUARD_PAGE";
    case 0xE06D7363:
        return "CXX_EXCEPTION";
    case kUncaughtCxxExceptionCode:
        return "UNCAUGHT_CXX_EXCEPTION";
    case kPureVirtualCallCode:
        return "PURE_VIRTUAL_CALL";
    case kInvalidParameterCode:
        return "INVALID_PARAMETER";
    case kCrtAssertOrErrorCode:
        return "CRT_ASSERT_OR_ERROR";
    case kCRuntimeSignalCode:
        return "C_RUNTIME_SIGNAL";
    default:
        return "UNKNOWN";
    }
}

std::string describeAccessViolation(const EXCEPTION_RECORD& record)
{
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION
        || record.NumberParameters < 2) {
        return {};
    }
    const ULONG_PTR operation = record.ExceptionInformation[0];
    const char* operationName = "unknown";
    if (operation == 0) {
        operationName = "read";
    } else if (operation == 1) {
        operationName = "write";
    } else if (operation == 8) {
        operationName = "execute";
    }
    return std::string("access_violation_operation=") + operationName
        + " access_violation_address=" + formatHex(record.ExceptionInformation[1]);
}

// 只处理真正让进程无法继续的结构化异常。断点、单步等正常控制流异常必须放行，
// 否则调试和 CRT 内部机制会被打断。
bool isFatalExceptionCode(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_INVALID_DISPOSITION:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
        return true;
    default:
        return false;
    }
}

bool isCaptureInProgress()
{
    return g_captureInProgress.load(std::memory_order_relaxed);
}

bool tryBeginCapture()
{
    bool expected = false;
    return g_captureInProgress.compare_exchange_strong(expected, true);
}

// ---------------------------------------------------------------------------
// 模块信息
// ---------------------------------------------------------------------------

class ModuleSnapshot {
public:
    ModuleSnapshot()
    {
        DWORD neededBytes = 0;
        if (EnumProcessModules(GetCurrentProcess(),
                               modules_.data(),
                               static_cast<DWORD>(modules_.size() * sizeof(HMODULE)),
                               &neededBytes)
            == FALSE) {
            return;
        }
        count_ = std::min<std::size_t>(modules_.size(),
                                       neededBytes / sizeof(HMODULE));
        totalCount_ = neededBytes / sizeof(HMODULE);
    }

    std::size_t GetCount() const { return count_; }
    std::size_t GetTotalCount() const { return totalCount_; }
    HMODULE GetModule(std::size_t index) const { return modules_[index]; }

    // 返回覆盖该地址的模块路径，以及相对模块基址的偏移。偏移是后续用 PDB 精确定位
    // 函数的关键：ASLR 会改变基址，而偏移在每次运行中都保持稳定。
    std::optional<std::pair<std::filesystem::path, std::uintptr_t>> locate(
        std::uintptr_t address) const
    {
        for (std::size_t index = 0; index < count_; ++index) {
            MODULEINFO moduleInfo{};
            if (GetModuleInformation(GetCurrentProcess(),
                                     modules_[index],
                                     &moduleInfo,
                                     sizeof(moduleInfo))
                == FALSE) {
                continue;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(moduleInfo.lpBaseOfDll);
            if (address < base || address >= base + moduleInfo.SizeOfImage) {
                continue;
            }
            std::array<wchar_t, MAX_PATH> pathBuffer{};
            if (GetModuleFileNameW(modules_[index],
                                   pathBuffer.data(),
                                   static_cast<DWORD>(pathBuffer.size()))
                == 0) {
                continue;
            }
            return std::make_pair(std::filesystem::path(pathBuffer.data()),
                                  address - base);
        }
        return std::nullopt;
    }

protected:
    std::array<HMODULE, kMaximumModuleCount> modules_{};
    std::size_t count_ = 0;
    std::size_t totalCount_ = 0;
};

std::optional<ModuleSnapshot> captureModuleSnapshot()
{
    ModuleSnapshot snapshot;
    if (snapshot.GetCount() == 0) {
        return std::nullopt;
    }
    return snapshot;
}

std::string describeModule(HMODULE module)
{
    std::array<wchar_t, MAX_PATH> modulePath{};
    const DWORD length = GetModuleFileNameW(
        module, modulePath.data(), static_cast<DWORD>(modulePath.size()));

    std::string line = "base=" + formatHex(reinterpret_cast<std::uint64_t>(module));
    MODULEINFO moduleInfo{};
    if (GetModuleInformation(
            GetCurrentProcess(), module, &moduleInfo, sizeof(moduleInfo))) {
        const auto base = reinterpret_cast<std::uintptr_t>(moduleInfo.lpBaseOfDll);
        line += " size=" + formatDecimal(moduleInfo.SizeOfImage);
        line += " end=" + formatHex(base + moduleInfo.SizeOfImage);
        line += " entry="
            + formatHex(reinterpret_cast<std::uint64_t>(moduleInfo.EntryPoint));
    }
    if (length != 0) {
        line += " path=" + wideToUtf8(modulePath.data());
    }
    return line;
}

std::string collectModuleLines()
{
    const std::optional<ModuleSnapshot> snapshot = captureModuleSnapshot();
    if (!snapshot.has_value()) {
        return "module_enumeration_failed\n";
    }

    std::string result =
        "module_count=" + formatDecimal(snapshot->GetTotalCount()) + "\n";
    for (std::size_t index = 0; index < snapshot->GetCount(); ++index) {
        result += "module[" + formatDecimal(index) + "]="
            + describeModule(snapshot->GetModule(index)) + "\n";
    }
    return result;
}

// 崩溃线程此刻就在本函数里，RtlCaptureStackBackTrace 拿到的是采集路径自己的栈，
// 只能作为参考；真正的调用栈由 minidump 还原。保留它是因为纯文本报告在没有调试
// 工具时也能提供一点线索。
std::string collectCaptureThreadBacktrace()
{
    std::array<void*, kMaximumCaptureFrames> frames{};
    const USHORT frameCount = RtlCaptureStackBackTrace(
        0, kMaximumCaptureFrames, frames.data(), nullptr);

    const std::optional<ModuleSnapshot> snapshot = captureModuleSnapshot();
    std::string result = "note=这是采集线程自身的栈，仅供无调试器时参考\n";
    result += "frames=" + formatDecimal(frameCount) + "\n";
    for (USHORT index = 0; index < frameCount; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[index]);
        result += "frame[" + formatDecimal(index) + "]=" + formatHex(address);
        if (snapshot.has_value()) {
            const auto located = snapshot->locate(address);
            if (located.has_value()) {
                result += " module=" + located->first.filename().string();
                result += " offset=" + formatHex(located->second);
            }
        }
        result += "\n";
    }
    return result;
}

// ---------------------------------------------------------------------------
// 应用上下文与日志
// ---------------------------------------------------------------------------

std::string readCrashActivity()
{
    const std::size_t size = std::min(
        g_activitySize.load(std::memory_order_acquire), kMaximumActivityBytes);
    if (size == 0) {
        return "activity_log_empty\n";
    }
    std::string result("activity_log_entries_follow\n");
    result.append(g_activityBuffer, size);
    if (result.back() != '\n') {
        result.push_back('\n');
    }
    return result;
}

std::string readRecentLogLines(const std::filesystem::path& logsDirectory,
                               int maximumLines)
{
    const std::filesystem::path logPath = logsDirectory / kDiscoveryLogFileName;
    std::error_code error;
    const std::uintmax_t fileSize = std::filesystem::file_size(logPath, error);
    if (error || fileSize == 0) {
        return "recent_diagnostic_log_unavailable\n";
    }

    // 按行数上限估算读取窗口，避免把整个日志读进已经出问题的进程。
    const std::uintmax_t windowSize = std::min<std::uintmax_t>(
        fileSize,
        std::min<std::uintmax_t>(kMaximumTailReadBytes,
                                 static_cast<std::uintmax_t>(maximumLines) * 512u
                                     + 8192u));
    std::ifstream input(logPath, std::ios::binary);
    if (!input) {
        return "recent_diagnostic_log_unavailable\n";
    }
    input.seekg(static_cast<std::streamoff>(fileSize - windowSize));

    std::string tail(static_cast<std::size_t>(windowSize), '\0');
    input.read(tail.data(), static_cast<std::streamsize>(windowSize));
    tail.resize(static_cast<std::size_t>(input.gcount()));

    std::vector<std::string> lines;
    std::size_t lineStart = 0;
    while (lineStart < tail.size()) {
        const std::size_t lineEnd = tail.find('\n', lineStart);
        if (lineEnd == std::string::npos) {
            break;
        }
        lines.emplace_back(tail.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;
    }
    if (lines.size() > static_cast<std::size_t>(maximumLines)) {
        lines.erase(lines.begin(),
                    lines.end() - static_cast<std::ptrdiff_t>(maximumLines));
    }

    std::string result =
        "recent_diagnostic_log_lines=" + formatDecimal(lines.size()) + "\n";
    for (const std::string& line : lines) {
        result += line + "\n";
    }
    return result;
}

// 崩溃后程序通常很快会被重启，日志随即被新内容覆盖。把本次日志整体复制进崩溃
// 目录，才能在事后还原用户现场。
void copyLogDirectorySnapshot(const std::filesystem::path& logsDirectory,
                              const std::filesystem::path& reportDirectory)
{
    std::error_code error;
    if (!std::filesystem::is_directory(logsDirectory, error)) {
        return;
    }
    const std::filesystem::path snapshotDirectory =
        reportDirectory / kLogSnapshotDirectoryName;
    std::filesystem::create_directories(snapshotDirectory, error);

    std::filesystem::directory_iterator iterator(
        logsDirectory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error) {
        return;
    }

    std::size_t copied = 0;
    for (const std::filesystem::directory_entry& entry : iterator) {
        if (copied >= kMaximumLogSnapshotFiles) {
            break;
        }
        std::error_code entryError;
        if (!entry.is_regular_file(entryError)) {
            continue;
        }
        const std::uintmax_t size = entry.file_size(entryError);
        if (entryError || size > kMaximumFileCopyBytes) {
            continue;
        }
        std::filesystem::copy_file(entry.path(),
                                   snapshotDirectory / entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing,
                                   entryError);
        if (!entryError) {
            ++copied;
        }
    }
}

// ---------------------------------------------------------------------------
// 转储与报告写出
// ---------------------------------------------------------------------------

bool writeMiniDumpFile(const std::filesystem::path& dumpPath,
                       const CrashCaptureContext& capture)
{
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (dbghelp == nullptr) {
        return false;
    }
    const auto writeDump = reinterpret_cast<MiniDumpWriteDumpFunction>(
        GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (writeDump == nullptr) {
        return false;
    }

    HANDLE file = CreateFileW(dumpPath.c_str(),
                              GENERIC_WRITE,
                              FILE_SHARE_READ,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    // 异常信息由调用方复制进来，不能把捕获时栈上的 EXCEPTION_POINTERS 直接传下去：
    // 那个对象可能在过滤器返回之后就失效。
    EXCEPTION_POINTERS exceptionPointers{};
    exceptionPointers.ExceptionRecord =
        const_cast<EXCEPTION_RECORD*>(&capture.exceptionRecord);
    exceptionPointers.ContextRecord = const_cast<CONTEXT*>(&capture.threadContext);

    MINIDUMP_EXCEPTION_INFORMATION exceptionInformation{};
    exceptionInformation.ThreadId = capture.threadId;
    exceptionInformation.ExceptionPointers = &exceptionPointers;
    exceptionInformation.ClientPointers = TRUE;

    const BOOL succeeded = writeDump(GetCurrentProcess(),
                                     capture.processId,
                                     file,
                                     g_dumpType,
                                     &exceptionInformation,
                                     nullptr,
                                     nullptr);
    CloseHandle(file);
    return succeeded != FALSE;
}

void pruneOldCrashReports(const std::filesystem::path& crashDirectory)
{
    if (g_maximumRetainedCrashes <= 0) {
        return;
    }

    std::error_code error;
    std::vector<std::filesystem::path> reports;
    std::filesystem::directory_iterator iterator(
        crashDirectory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error) {
        return;
    }
    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entryError;
        if (!entry.is_directory(entryError)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind(kReportIdPrefix, 0) != 0) {
            continue;
        }
        reports.push_back(entry.path());
    }

    // 目录名以本地时间戳开头，字典序即时间序，无需再读文件属性。
    std::sort(reports.begin(), reports.end());
    if (reports.size() <= static_cast<std::size_t>(g_maximumRetainedCrashes)) {
        return;
    }
    const std::size_t removeCount =
        reports.size() - static_cast<std::size_t>(g_maximumRetainedCrashes);
    for (std::size_t index = 0; index < removeCount; ++index) {
        std::error_code removeError;
        std::filesystem::remove_all(reports[index], removeError);
    }
}

void writeSymbolizationHint(const std::filesystem::path& reportDirectory)
{
    std::string hint;
    hint += "本目录包含一次崩溃的完整现场。\n";
    hint += "\n";
    hint += "crash.dmp     minidump，含全部线程栈与内存\n";
    hint += "report.txt    人类可读摘要（异常码、出错模块+偏移、最近操作、模块列表）\n";
    hint += "metadata.txt  键值对形式，便于脚本解析与上报\n";
    hint += "logs/         崩溃时刻 data/logs 的快照，重启不会覆盖\n";
    hint += "\n";
    hint += "=== 如何还原调用栈 ===\n";
    hint += "\n";
    hint += "需要与本次 exe 同一次构建产出的 relaydesk_skiaui.pdb。\n";
    hint += "\n";
    hint += "方式一（推荐，在项目仓库里执行）：\n";
    hint += "  powershell -NoProfile -ExecutionPolicy Bypass -File tools\\symbolize-crash.ps1 "
            "-CrashDirectory \"<本目录>\"\n";
    hint += "  脚本会自动从 out/symbols 找到匹配版本的 pdb，并打印调用栈。\n";
    hint += "\n";
    hint += "方式二（命令行，需要 Windows SDK 的 Debugging Tools for Windows）：\n";
    hint += "  cdb.exe -z crash.dmp -y \"<pdb 目录>\" -c \".ecxr; kv; ~*kv; !analyze -v; q\"\n";
    hint += "\n";
    hint += "方式三（图形界面，无需额外安装）：\n";
    hint += "  1. 把 relaydesk_skiaui.exe 和 relaydesk_skiaui.pdb 复制到本目录；\n";
    hint += "  2. 双击 crash.dmp，用 Visual Studio 打开；\n";
    hint += "  3. 在“并行堆栈”窗口查看所有线程，当前线程停在异常点。\n";
    hint += "\n";
    hint += "注意：pdb 必须与 exe 严格来自同一次构建，换过版本会给出错误行号。\n";
    writeFileContents(reportDirectory / kSymbolizationHintFileName, hint);
}

// 崩溃路径的全部脏活都集中在这里。它由 runProtected 通过 SEH 保护调用，
// 因此可以正常使用标准库。
//
// 上报时机：转储、日志快照、report.txt/metadata.txt/symbolize.txt 全部落盘后，
// 立即拉起同一个 exe 的崩溃处理模式。是否上传由那个独立进程弹窗询问用户决定，
// 崩溃进程自身不做任何网络操作，也不等待子进程。

// 把宽字符串规整成合法的命令行参数（含空格时加引号）。只在崩溃路径使用固定内存，
// 不分配、不抛异常。
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

// 崩溃采集收尾时调用。失败时安静返回：采集本身已经成功，拉不起处理程序不该
// 影响进程退出，更不该在崩溃路径上再抛异常。
struct ReporterLaunchContext {
    std::filesystem::path reportDirectory;
    std::string reportId;
};

void launchCrashReporterProcessImpl(void* rawContext);

int runReporterLaunchThunk(void* rawContext)
{
    __try {
        launchCrashReporterProcessImpl(rawContext);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

void runReporterLaunch(const std::filesystem::path& reportDirectory,
                       const std::string& reportId)
{
    ReporterLaunchContext context;
    context.reportDirectory = reportDirectory;
    context.reportId = reportId;
    (void)runReporterLaunchThunk(&context);
}

void launchCrashReporterProcessImpl(void* rawContext)
{
    const auto* context =
        static_cast<const ReporterLaunchContext*>(rawContext);
    if (context == nullptr) {
        return;
    }
    const std::filesystem::path& reportDirectory = context->reportDirectory;
    const std::string& reportId = context->reportId;

    if (g_reporterExecutablePath[0] == L'\0') {
        return;
    }

    std::wstring commandLine;
    commandLine.reserve(1024);
    appendQuotedArgument(commandLine, g_reporterExecutablePath);
    appendQuotedArgument(commandLine, kCrashReporterArgument);
    // 传崩溃根目录而不是报告目录：崩溃处理模式会扫描根目录并用 report-id 过滤。
    appendQuotedArgument(commandLine, kCrashDirectoryArgument);
    appendQuotedArgument(commandLine, reportDirectory.parent_path().wstring());
    appendQuotedArgument(commandLine, kCrashReportIdArgument);
    appendQuotedArgument(commandLine, utf8ToWide(reportId));

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(),
                                            commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(nullptr,
                                        mutableCommandLine.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        0,
                                        nullptr,
                                        nullptr,
                                        &startupInfo,
                                        &processInfo);
    if (!created) {
        return;
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
}

void writeCrashReport(ReportWriteContext* context)
{
    if (g_crashDirectory[0] == '\0') {
        return;
    }

    const std::filesystem::path crashDirectory(g_crashDirectory);
    const std::string reportId = formatReportId();
    const std::filesystem::path reportDirectory =
        crashDirectory / (std::string(kReportIdPrefix) + reportId);
    std::error_code error;
    std::filesystem::create_directories(reportDirectory, error);
    if (error) {
        return;
    }

    // 先落盘转储：它需要进程仍保持崩溃瞬间的线程与内存状态。
    writeMiniDumpFile(reportDirectory / kDumpFileName, context->capture);

    const std::filesystem::path logsDirectory(g_logsDirectory);
    copyLogDirectorySnapshot(logsDirectory, reportDirectory);

    std::string failingModuleName;
    std::string failingModulePath;
    std::string failingOffset;
    const auto exceptionAddress = reinterpret_cast<std::uintptr_t>(
        context->capture.exceptionRecord.ExceptionAddress);
    if (exceptionAddress != 0) {
        const std::optional<ModuleSnapshot> snapshot = captureModuleSnapshot();
        if (snapshot.has_value()) {
            const auto located = snapshot->locate(exceptionAddress);
            if (located.has_value()) {
                failingModuleName = located->first.filename().string();
                failingModulePath = located->first.string();
                failingOffset = formatHex(located->second);
            }
        }
    }

    std::string report;
    report += "=== RelayDesk 崩溃报告 ===\n";
    report += std::string("version=") + g_applicationVersion + "\n";
    report += std::string("build_configuration=") + g_buildConfiguration + "\n";
    report += std::string("build_timestamp=") + g_buildTimestamp + "\n";
    report += "crashed_at_local=" + currentLocalTimestampText() + "\n";
    report += "crashed_at_utc=" + currentUtcTimestampText() + "\n";
    report += "process_id=" + formatDecimal(context->capture.processId) + "\n";
    report += "thread_id=" + formatDecimal(context->capture.threadId) + "\n";
    report += "exception_code="
        + formatHex(context->capture.exceptionRecord.ExceptionCode) + "\n";
    report += "exception_name="
        + describeExceptionCode(context->capture.exceptionRecord.ExceptionCode) + "\n";
    report += "exception_address=" + formatHex(exceptionAddress) + "\n";
    report += "exception_flags="
        + formatHex(context->capture.exceptionRecord.ExceptionFlags) + "\n";
    report += "instruction_pointer="
        + formatHex(static_cast<std::uint64_t>(context->capture.threadContext.Rip))
        + "\n";
    report += "stack_pointer="
        + formatHex(static_cast<std::uint64_t>(context->capture.threadContext.Rsp))
        + "\n";
    if (!failingModuleName.empty()) {
        report += "failing_module=" + failingModuleName + "\n";
        report += "failing_module_offset=" + failingOffset + "\n";
        report += "failing_module_path_utf8=" + failingModulePath + "\n";
    } else {
        report += "failing_module=unknown\n";
    }
    const std::string accessViolation =
        describeAccessViolation(context->capture.exceptionRecord);
    if (!accessViolation.empty()) {
        report += accessViolation + "\n";
    }
    if (context->detail[0] != '\0') {
        report += std::string("extra_detail=") + context->detail + "\n";
    }

    std::array<wchar_t, MAX_PATH> executablePath{};
    if (GetModuleFileNameW(
            nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()))
        != 0) {
        const std::filesystem::path executable(executablePath.data());
        report += "executable=" + executable.filename().string() + "\n";
        report += "executable_path_utf8=" + executable.string() + "\n";
        const std::filesystem::path symbolPath =
            executable.parent_path() / (executable.stem().wstring() + L".pdb");
        std::error_code symbolError;
        report += "pdb_present_next_to_exe=";
        report += std::filesystem::exists(symbolPath, symbolError) ? "1\n" : "0\n";
    }
    report += std::string("command_line=")
        + (GetCommandLineA() != nullptr ? GetCommandLineA() : "") + "\n";
    report += "crash_handler=in-process (vectored + unhandled filter + terminate)\n";

    report += "\n=== 崩溃瞬间的应用上下文 ===\n";
    report += readCrashActivity();

    report += "\n=== 最近诊断日志 ===\n";
    report += readRecentLogLines(logsDirectory, g_recentLogLineCount);

    report += "\n=== 采集线程自身栈（未符号化，仅供参考）===\n";
    report += collectCaptureThreadBacktrace();

    report += "\n=== 已加载模块 ===\n";
    report += collectModuleLines();

    report += "\n=== 符号与排查 ===\n";
    report += "crash_dump=crash.dmp\n";
    report += "logs_snapshot=logs/\n";
    report += "symbol_archive=symbols/<version>/relaydesk_skiaui.pdb\n";
    report += "symbolization_hint=symbolize.txt\n";

    if (report.size() > kMaximumReportBytes) {
        report.resize(kMaximumReportBytes);
        report += "\n[报告超长，已截断]\n";
    }
    writeFileContents(reportDirectory / kReportFileName, report);

    std::string metadata;
    metadata += "report_id=" + reportId + "\n";
    metadata += std::string("version=") + g_applicationVersion + "\n";
    metadata += std::string("build_configuration=") + g_buildConfiguration + "\n";
    metadata += std::string("build_timestamp=") + g_buildTimestamp + "\n";
    metadata += "crashed_at_local=" + currentLocalTimestampText() + "\n";
    metadata += "crashed_at_utc=" + currentUtcTimestampText() + "\n";
    metadata += "exception_code="
        + formatHex(context->capture.exceptionRecord.ExceptionCode) + "\n";
    metadata += "exception_name="
        + describeExceptionCode(context->capture.exceptionRecord.ExceptionCode) + "\n";
    metadata += "failing_module="
        + (failingModuleName.empty() ? std::string("unknown") : failingModuleName) + "\n";
    metadata += "failing_module_offset=" + failingOffset + "\n";
    metadata += "dump_type="
        + formatDecimal(static_cast<std::uint64_t>(g_dumpType)) + "\n";
    metadata += "dump_file=" + std::string(kDumpFileName) + "\n";
    writeFileContents(reportDirectory / kMetadataFileName, metadata);

    writeSymbolizationHint(reportDirectory);
    pruneOldCrashReports(crashDirectory);

    // 转储与日志都已落盘，现在立刻拉起同一个 exe 的崩溃处理模式，由它弹窗询问
    // 用户是否上传。放在最后一步：处理程序读到的目录一定是完整现场。
    runReporterLaunch(reportDirectory, reportId);
}

void captureCrashReport(const CrashCaptureContext& capture,
                        const char* extraDetail)
{
    ReportWriteContext context;
    context.capture = capture;
    if (extraDetail != nullptr) {
        std::snprintf(context.detail, sizeof(context.detail), "%s", extraDetail);
    }
    // 采集过程中再次崩溃时安静退出，把控制权还给系统默认处理。
    runProtected(&writeCrashReport, &context);
}

// ---------------------------------------------------------------------------
// 异常入口
// ---------------------------------------------------------------------------

// 向量化处理器是唯一能覆盖"工作线程上未处理的结构化异常"的入口：没有它，网络
// 线程或异步刷新线程里的访问违例会直接杀掉线程，既不进 std::terminate，也不留报告。
// 必须使用 WINAPI 调用约定才能匹配 PVECTORED_EXCEPTION_HANDLER。
LONG WINAPI runVectoredHandler(PEXCEPTION_POINTERS exceptionPointers)
{
    if (exceptionPointers == nullptr || exceptionPointers->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!isFatalExceptionCode(exceptionPointers->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // 调试器在场时让调试器先处理；采集期间的二次异常一律忽略，避免重入。
    if (IsDebuggerPresent() || !tryBeginCapture()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    g_pendingException = exceptionPointers;

    CrashCaptureContext capture;
    capture.exceptionRecord = *exceptionPointers->ExceptionRecord;
    if (exceptionPointers->ContextRecord != nullptr) {
        capture.threadContext = *exceptionPointers->ContextRecord;
    }
    capture.processId = GetCurrentProcessId();
    capture.threadId = GetCurrentThreadId();
    captureCrashReport(capture, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

long WINAPI crashUnhandledExceptionFilter(PEXCEPTION_POINTERS exceptionPointers)
{
    g_pendingException = exceptionPointers;

    // 向量化处理器已经采集过的异常不再重复采集。
    if (tryBeginCapture()) {
        CrashCaptureContext capture;
        if (exceptionPointers != nullptr
            && exceptionPointers->ExceptionRecord != nullptr) {
            capture.exceptionRecord = *exceptionPointers->ExceptionRecord;
            if (exceptionPointers->ContextRecord != nullptr) {
                capture.threadContext = *exceptionPointers->ContextRecord;
            }
        }
        capture.processId = GetCurrentProcessId();
        capture.threadId = GetCurrentThreadId();
        captureCrashReport(capture, nullptr);
    }

    if (g_previousFilter != nullptr) {
        return g_previousFilter(exceptionPointers);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void crashTerminateHandler()
{
    // 未捕获的 C++ 异常在 /EHsc 下最终走到这里，是"进程直接消失且没有报告"的
    // 最常见来源。
    if (tryBeginCapture()) {
        CrashCaptureContext capture;
        capture.processId = GetCurrentProcessId();
        capture.threadId = GetCurrentThreadId();
        capture.exceptionRecord.ExceptionCode = kUncaughtCxxExceptionCode;
        captureCrashReport(capture, nullptr);
    }

    if (g_previousTerminateHandler != nullptr) {
        g_previousTerminateHandler();
        return;
    }
    TerminateProcess(GetCurrentProcess(), 3);
}

#if defined(_MSC_VER)
void crashPurecallHandler()
{
    if (tryBeginCapture()) {
        CrashCaptureContext capture;
        capture.processId = GetCurrentProcessId();
        capture.threadId = GetCurrentThreadId();
        capture.exceptionRecord.ExceptionCode = kPureVirtualCallCode;
        captureCrashReport(capture, nullptr);
    }

    if (g_previousPurecallHandler != nullptr) {
        g_previousPurecallHandler();
        return;
    }
    TerminateProcess(GetCurrentProcess(), 4);
}
#endif

void crashInvalidParameterHandler(const wchar_t* expression,
                                  const wchar_t* functionName,
                                  const wchar_t* fileName,
                                  unsigned int lineNumber,
                                  uintptr_t)
{
    (void)functionName;

    // 无效参数处理器对"可恢复的 CRT 参数检查"同样会触发。只有正在采集崩溃时才把它
    // 当成崩溃信号，否则会把正常路径误判成崩溃。
    if (!isCaptureInProgress()) {
        return;
    }

    std::string detail = "line=" + formatDecimal(lineNumber);
    if (expression != nullptr) {
        detail += " expression=" + wideToUtf8(expression);
    }
    if (fileName != nullptr) {
        detail += " file=" + wideToUtf8(fileName);
    }

    CrashCaptureContext capture;
    capture.processId = GetCurrentProcessId();
    capture.threadId = GetCurrentThreadId();
    capture.exceptionRecord.ExceptionCode = kInvalidParameterCode;
    captureCrashReport(capture, detail.c_str());
}

#if defined(_MSC_VER)
int crashReportHook(int reportType, char* message, int* returnValue)
{
    const bool shouldCapture =
        (reportType == _CRT_ASSERT || reportType == _CRT_ERROR)
        && isCaptureInProgress();
    if (!shouldCapture) {
        return FALSE;
    }
    if (tryBeginCapture()) {
        CrashCaptureContext capture;
        capture.processId = GetCurrentProcessId();
        capture.threadId = GetCurrentThreadId();
        capture.exceptionRecord.ExceptionCode = kCrtAssertOrErrorCode;
        captureCrashReport(capture, message);
    }
    if (returnValue != nullptr) {
        *returnValue = 1;
    }
    return TRUE;
}
#endif

void crashSignalHandler(int signalNumber)
{
    if (tryBeginCapture()) {
        CrashCaptureContext capture;
        capture.processId = GetCurrentProcessId();
        capture.threadId = GetCurrentThreadId();
        capture.exceptionRecord.ExceptionCode = kCRuntimeSignalCode;
        const std::string detail =
            "signal=" + formatDecimal(static_cast<std::uint64_t>(signalNumber));
        captureCrashReport(capture, detail.c_str());
    }
    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

// Windows 错误报告的 LocalDumps 兜底：当进程在我们的处理器之前就被系统终止
// （栈严重损坏、堆已崩溃）时，仍能拿到一份系统级转储。
void configureWindowsErrorReportingLocalDumps(
    const std::filesystem::path& dumpDirectory)
{
    if (!g_writeHelperDump) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(dumpDirectory, error);

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\Windows Error Reporting\\"
                        L"LocalDumps\\relaydesk_skiaui.exe",
                        0,
                        nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_WRITE,
                        nullptr,
                        &key,
                        nullptr)
        != ERROR_SUCCESS) {
        return;
    }

    const std::wstring folder = dumpDirectory.wstring();
    RegSetValueExW(key,
                   L"DumpFolder",
                   0,
                   REG_EXPAND_SZ,
                   reinterpret_cast<const BYTE*>(folder.c_str()),
                   static_cast<DWORD>((folder.size() + 1) * sizeof(wchar_t)));
    // 2 = 完整内存转储，与进程内采集保持一致，便于对照两份现场。
    const DWORD dumpType = 2;
    RegSetValueExW(key,
                   L"DumpType",
                   0,
                   REG_DWORD,
                   reinterpret_cast<const BYTE*>(&dumpType),
                   sizeof(dumpType));
    const DWORD dumpCount =
        static_cast<DWORD>(std::max(1, g_maximumRetainedCrashes));
    RegSetValueExW(key,
                   L"DumpCount",
                   0,
                   REG_DWORD,
                   reinterpret_cast<const BYTE*>(&dumpCount),
                   sizeof(dumpCount));
    RegCloseKey(key);
}

MINIDUMP_TYPE toNativeDumpType(CrashDumpType dumpType)
{
    switch (dumpType) {
    case CrashDumpType::Normal:
        // 带上间接引用内存：栈里引用的字符串和缓冲区往往是定位问题的关键。
        return static_cast<MINIDUMP_TYPE>(MiniDumpNormal
                                         | MiniDumpWithIndirectlyReferencedMemory
                                         | MiniDumpWithThreadInfo
                                         | MiniDumpWithUnloadedModules);
    case CrashDumpType::WithDataSegments:
        return static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs
                                         | MiniDumpWithIndirectlyReferencedMemory
                                         | MiniDumpWithThreadInfo
                                         | MiniDumpWithUnloadedModules);
    case CrashDumpType::Full:
        return static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory
                                         | MiniDumpWithFullMemoryInfo
                                         | MiniDumpWithHandleData
                                         | MiniDumpWithThreadInfo
                                         | MiniDumpWithUnloadedModules
                                         | MiniDumpWithProcessThreadData
                                         | MiniDumpWithTokenInformation);
    }
    return MiniDumpNormal;
}

} // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

void recordCrashActivity(const std::string& activity) noexcept
{
    if (!g_installed.load(std::memory_order_relaxed) || activity.empty()) {
        return;
    }
    if (isCaptureInProgress()) {
        return;
    }

    EnterCriticalSection(&activityCriticalSection());
    std::size_t offset = g_activitySize.load(std::memory_order_relaxed);
    if (offset + kMaximumActivityEntryBytes + 2 > kMaximumActivityBytes) {
        // 环形回卷：丢弃历史，保证崩溃前最近的上下文一定还在缓冲区里。
        offset = 0;
    }

    const std::size_t maximumEntryForSpace = kMaximumActivityBytes - offset - 2;
    const std::size_t copied =
        std::min(std::min(activity.size(), kMaximumActivityEntryBytes),
                 maximumEntryForSpace);
    std::memcpy(g_activityBuffer + offset, activity.data(), copied);
    g_activityBuffer[offset + copied] = '\n';
    g_activitySize.store(offset + copied + 1, std::memory_order_release);
    LeaveCriticalSection(&activityCriticalSection());
}

void installCrashHandler(const CrashReportConfiguration& configuration)
{
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;
    }

    const std::string crashDirectoryText =
        configuration.GetCrashDirectory().string();
    const std::string logsDirectoryText = configuration.GetLogsDirectory().string();
    std::snprintf(g_crashDirectory,
                  sizeof(g_crashDirectory),
                  "%s",
                  crashDirectoryText.c_str());
    std::snprintf(g_logsDirectory,
                  sizeof(g_logsDirectory),
                  "%s",
                  logsDirectoryText.c_str());
    std::snprintf(g_applicationVersion,
                  sizeof(g_applicationVersion),
                  "%s",
                  configuration.GetApplicationVersion().c_str());
    std::snprintf(g_buildConfiguration,
                  sizeof(g_buildConfiguration),
                  "%s",
                  configuration.GetBuildConfiguration().c_str());
    std::snprintf(g_buildTimestamp,
                  sizeof(g_buildTimestamp),
                  "%s",
                  configuration.GetBuildTimestamp().c_str());

    // 崩溃处理程序默认就是当前 exe；配置里给了路径时以配置为准。
    std::filesystem::path reporterExecutable =
        configuration.GetReporterExecutablePath();
    if (reporterExecutable.empty()) {
        std::array<wchar_t, kMaximumPathTextLength> currentExecutable{};
        const DWORD length = GetModuleFileNameW(
            nullptr,
            currentExecutable.data(),
            static_cast<DWORD>(currentExecutable.size()));
        if (length != 0 && length < currentExecutable.size()) {
            reporterExecutable = std::filesystem::path(currentExecutable.data());
        }
    }
    if (!reporterExecutable.empty()) {
        (void)wcsncpy_s(g_reporterExecutablePath,
                        kMaximumPathTextLength,
                        reporterExecutable.c_str(),
                        _TRUNCATE);
    }

    g_maximumRetainedCrashes = configuration.GetMaximumRetainedCrashes();
    g_recentLogLineCount = configuration.GetRecentLogLineCount();
    g_writeHelperDump = configuration.GetWriteHelperDump();
    g_dumpType = toNativeDumpType(configuration.GetDumpType());

    std::error_code error;
    std::filesystem::create_directories(configuration.GetCrashDirectory(), error);
    std::filesystem::create_directories(configuration.GetLogsDirectory(), error);

    g_previousFilter = SetUnhandledExceptionFilter(&crashUnhandledExceptionFilter);
    g_previousTerminateHandler = std::set_terminate(&crashTerminateHandler);
    g_previousInvalidParameterHandler =
        _set_invalid_parameter_handler(&crashInvalidParameterHandler);
#if defined(_MSC_VER)
    g_previousPurecallHandler = _set_purecall_handler(&crashPurecallHandler);
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &crashReportHook);
#endif
    signal(SIGABRT, &crashSignalHandler);
    signal(SIGFPE, &crashSignalHandler);
    signal(SIGILL, &crashSignalHandler);
    signal(SIGSEGV, &crashSignalHandler);

    // 向量化处理器覆盖进程里所有线程，必须与其他入口一起在启动早期安装。
    g_vectoredHandler = AddVectoredExceptionHandler(1, &runVectoredHandler);
    configureWindowsErrorReportingLocalDumps(configuration.GetCrashDirectory()
                                             / "wer");
}

bool isCrashHandlerInstalled()
{
    return g_installed.load(std::memory_order_relaxed);
}

std::vector<CrashReportEntry> findPendingCrashReports(
    const std::filesystem::path& crashDirectory)
{
    std::vector<CrashReportEntry> reports;

    std::error_code error;
    std::filesystem::directory_iterator iterator(
        crashDirectory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error) {
        return reports;
    }

    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entryError;
        if (!entry.is_directory(entryError)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind(kReportIdPrefix, 0) != 0) {
            continue;
        }
        if (std::filesystem::exists(entry.path() / kUploadMarkerFileName,
                                    entryError)) {
            continue;
        }

        CrashReportEntry report;
        report.SetDirectory(entry.path());
        report.SetReportId(name.substr(std::strlen(kReportIdPrefix)));
        reports.push_back(std::move(report));
    }

    std::sort(reports.begin(),
              reports.end(),
              [](const CrashReportEntry& left, const CrashReportEntry& right) {
                  return left.GetReportId() < right.GetReportId();
              });
    return reports;
}

void markCrashReportUploaded(const std::filesystem::path& reportDirectory,
                             const std::string& uploadedAt)
{
    const std::string marker = "uploaded_at=" + uploadedAt + "\n";
    writeFileContents(reportDirectory / kUploadMarkerFileName, marker);
}

void clearCrashReport(const std::filesystem::path& reportDirectory)
{
    std::error_code error;
    std::filesystem::remove_all(reportDirectory, error);
}

} // namespace relaydesk::platform
