#include "core/app_version.h"
#include "core/platform/async.h"
#include "main/app_runtime.h"
#include "net/boost_asio_tcp_peer_transport.h"
#include "net/peer_message.h"
#include "storage/app_paths.h"
#include "storage/history_store.h"
#include "storage/peer_profile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include <windows.h>
#include <shellapi.h>

namespace {

constexpr wchar_t kAppUpdateRestartProbeEnvironment[] =
    L"RELAYDESK_APP_UPDATE_RESTART_PROBE";

int fail(const std::string& message)
{
    std::cerr << message << '\n';
    return 1;
}

int expect(bool condition, const std::string& message)
{
    if (!condition) {
        return fail(message);
    }
    return 0;
}

relaydesk::runtime::RelayDeskRuntimeOptions makeTestRuntimeOptions()
{
    relaydesk::runtime::RelayDeskRuntimeOptions options;
    options.SetTcpListenPort(0);
    options.SetDiscoveryUdpPort(0);
    options.SetDiscoveryBroadcastEnabled(false);
    options.SetDiscoveryAnnounceOnStart(false);
    options.SetNetworkEnabled(false);
    options.SetAsyncRefreshEnabled(false);
    return options;
}

relaydesk::storage::PeerProfile makePeerProfile(int appVersion,
                                                const std::string& lastSeenAt)
{
    relaydesk::storage::PeerProfile profile;
    profile.SetDeviceId("runtime-update-peer");
    profile.SetHostName("RUNTIME-UPDATE-HOST");
    profile.SetDisplayName("Runtime Update Peer");
    profile.SetLastAddresses({"127.0.0.1"});
    profile.SetTcpPort(39171);
    profile.SetAppVersion(appVersion);
    profile.SetCapabilities({"text", "file"});
    profile.SetFirstSeenAt("2026-06-18T10:00:00Z");
    profile.SetLastSeenAt(lastSeenAt);
    return profile;
}

relaydesk::storage::PeerProfile makePeerProfileForDevice(
    const std::string& deviceId,
    const std::string& displayName,
    int appVersion,
    const std::string& lastSeenAt)
{
    relaydesk::storage::PeerProfile profile;
    profile.SetDeviceId(deviceId);
    profile.SetHostName(displayName + "-HOST");
    profile.SetDisplayName(displayName);
    profile.SetLastAddresses({"127.0.0.1"});
    profile.SetTcpPort(39171);
    profile.SetAppVersion(appVersion);
    profile.SetCapabilities({"text", "file"});
    profile.SetFirstSeenAt("2026-06-18T10:00:00Z");
    profile.SetLastSeenAt(lastSeenAt);
    return profile;
}

relaydesk::storage::PeerProfile makeUpdatePeerProfile(std::uint16_t tcpPort,
                                                      int appVersion)
{
    relaydesk::storage::PeerProfile profile;
    profile.SetDeviceId("runtime-update-peer");
    profile.SetHostName("RUNTIME-UPDATE-HOST");
    profile.SetDisplayName("Runtime Update Peer");
    profile.SetLastAddresses({"127.0.0.1"});
    profile.SetTcpPort(tcpPort);
    profile.SetAppVersion(appVersion);
    profile.SetCapabilities({"text", "file"});
    profile.SetFirstSeenAt("2026-06-21T09:00:00Z");
    profile.SetLastSeenAt("2026-06-21T09:30:00Z");
    return profile;
}

relaydesk::storage::ChatMessagePart makeRuntimeTextPart(
    const std::string& partId,
    const std::string& text)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::Text);
    part.SetText(text);
    return part;
}

relaydesk::storage::ChatMessageRecord makeIncomingChatRecord(
    const relaydesk::runtime::LocalUserSummary& localUser)
{
    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId("notification-message");
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     "runtime-transfer-peer"));
    record.SetDirection(relaydesk::storage::MessageDirection::Incoming);
    record.SetSenderDeviceId("runtime-transfer-peer");
    record.SetReceiverDeviceId(localUser.GetDeviceId());
    record.SetSenderDisplayNameSnapshot("Runtime Transfer Peer");
    record.SetReceiverDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetCreatedAt("2026-06-23T10:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Received);
    record.AddPart(makeRuntimeTextPart("notification-part", "hello"));
    return record;
}

std::string makeIndexedMessageId(int index)
{
    std::ostringstream output;
    output << "runtime-history-" << std::setw(2) << std::setfill('0') << index;
    return output.str();
}

relaydesk::storage::ChatMessageRecord makeRuntimeHistoryTextRecord(
    const relaydesk::runtime::LocalUserSummary& localUser,
    int index)
{
    const std::string messageId = makeIndexedMessageId(index);
    const int hour = 10 + index / 60;
    const int minute = index % 60;
    std::ostringstream createdAt;
    createdAt << "2026-06-23T" << std::setw(2) << std::setfill('0') << hour
              << ":" << std::setw(2) << std::setfill('0') << minute
              << ":00Z";
    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     "runtime-transfer-peer"));
    record.SetDirection(relaydesk::storage::MessageDirection::Incoming);
    record.SetSenderDeviceId("runtime-transfer-peer");
    record.SetReceiverDeviceId(localUser.GetDeviceId());
    record.SetSenderDisplayNameSnapshot("Runtime Transfer Peer");
    record.SetReceiverDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetCreatedAt(createdAt.str());
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Received);
    record.AddPart(makeRuntimeTextPart("history-part", "message " + messageId));
    return record;
}

std::string makeWorkRelativePath(const relaydesk::storage::AppPaths& appPaths,
                                 const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path relativePath =
        std::filesystem::relative(path, appPaths.GetWorkDirectory(), error);
    if (error || relativePath.empty()) {
        return path.string();
    }
    const auto value = relativePath.generic_u8string();
    return std::string(value.begin(), value.end());
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

void writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open text file for writing: "
                                 + path.string());
    }
    output << text;
    if (!output) {
        throw std::runtime_error("Failed to write text file: " + path.string());
    }
}

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open text file for reading: "
                                 + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string readTextFileIfExists(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        return {};
    }
    return readTextFile(path);
}

std::string readStatusValue(const std::string& status, const std::string& key)
{
    const std::string prefix = key + "=";
    std::size_t position = 0;
    while (position <= status.size()) {
        const std::size_t lineEnd = status.find('\n', position);
        const std::string line = status.substr(
            position,
            lineEnd == std::string::npos ? std::string::npos : lineEnd - position);
        if (line.rfind(prefix, 0) == 0) {
            return line.substr(prefix.size());
        }
        if (lineEnd == std::string::npos) {
            break;
        }
        position = lineEnd + 1;
    }
    return {};
}

std::filesystem::path processTestRoot()
{
    return std::filesystem::path(RELAYDESK_APP_RUNTIME_PROCESS_TEST_WORK_DIR);
}

std::filesystem::path normalizedAbsoluteTestPath(
    const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalizedPath =
        std::filesystem::weakly_canonical(path, error);
    if (error) {
        throw std::runtime_error("Failed to normalize test path.");
    }
    return normalizedPath.lexically_normal();
}

bool isStrictDescendantOfProcessTestRoot(
    const std::filesystem::path& path)
{
    const std::filesystem::path relativePath =
        normalizedAbsoluteTestPath(path).lexically_relative(
            normalizedAbsoluteTestPath(processTestRoot()));
    if (relativePath.empty() || relativePath == "." ||
        relativePath.is_absolute()) {
        return false;
    }

    for (const auto& component : relativePath) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

void requireTestProcessIsolation()
{
    if (!relaydesk::storage::isTestDataSandboxEnabled()) {
        throw std::runtime_error(
            "Refusing to run without an explicit test data sandbox.");
    }
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    if (!isStrictDescendantOfProcessTestRoot(
            appPaths.GetDataDirectory())) {
        throw std::runtime_error(
            "Refusing to run with a data directory outside the test root.");
    }
}

void removeTestDirectory(const std::filesystem::path& directory)
{
    if (!isStrictDescendantOfProcessTestRoot(directory)) {
        throw std::runtime_error(
            "Refusing to remove a directory outside the test root.");
    }

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    if (error) {
        throw std::runtime_error("Failed to remove test directory.");
    }
}

void removeTestDataDirectory(
    const relaydesk::storage::AppPaths& appPaths)
{
    const std::filesystem::path& dataDirectory =
        appPaths.GetDataDirectory();
    if (dataDirectory.filename() != "data") {
        throw std::runtime_error("Refusing to remove a non-data directory.");
    }
    removeTestDirectory(dataDirectory);
}

int refusesCleanupOutsideProcessTestRoot()
{
    const std::filesystem::path outsideDataDirectory =
        processTestRoot().parent_path() / "data";
    if (isStrictDescendantOfProcessTestRoot(outsideDataDirectory)) {
        return fail("Test cleanup guard accepted a path outside the test root.");
    }

    try {
        removeTestDirectory(outsideDataDirectory);
    } catch (const std::runtime_error&) {
        return 0;
    }
    return fail("Test cleanup guard did not reject an external path.");
}

std::filesystem::path makeUniqueProcessScenarioRoot(const std::string& prefix)
{
    return processTestRoot()
        / (prefix + "-"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
}

std::filesystem::path makeUniqueProcessScenarioRoot(const std::wstring& prefix)
{
    return processTestRoot()
        / (prefix + L"-"
           + std::to_wstring(
               std::chrono::steady_clock::now().time_since_epoch().count()));
}

std::filesystem::path currentExecutablePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error("Failed to resolve current executable path.");
    }

    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::optional<std::filesystem::path> appUpdateRestartProbePath()
{
    const DWORD requiredLength = GetEnvironmentVariableW(
        kAppUpdateRestartProbeEnvironment, nullptr, 0);
    if (requiredLength == 0) {
        return std::nullopt;
    }

    std::wstring value(requiredLength, L'\0');
    const DWORD actualLength = GetEnvironmentVariableW(
        kAppUpdateRestartProbeEnvironment,
        value.data(),
        static_cast<DWORD>(value.size()));
    if (actualLength == 0 || actualLength >= value.size()) {
        throw std::runtime_error(
            "Failed to read app update restart probe path.");
    }
    value.resize(actualLength);
    return std::filesystem::path(value);
}

int writeAppUpdateRestartProbe(const std::filesystem::path& markerPath)
{
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    GetStartupInfoW(&startupInfo);

    std::ostringstream status;
    status << "started=1\n"
           << "use_show_window="
           << ((startupInfo.dwFlags & STARTF_USESHOWWINDOW) != 0 ? 1 : 0)
           << "\nshow_window="
           << static_cast<unsigned int>(startupInfo.wShowWindow) << '\n';
    writeTextFile(markerPath, status.str());
    return 0;
}

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(const wchar_t* name, const std::wstring& value)
        : name_(name)
    {
        const DWORD requiredLength = GetEnvironmentVariableW(name_, nullptr, 0);
        if (requiredLength != 0) {
            previousValue_.emplace(requiredLength, L'\0');
            const DWORD actualLength = GetEnvironmentVariableW(
                name_,
                previousValue_->data(),
                static_cast<DWORD>(previousValue_->size()));
            if (actualLength == 0 || actualLength >= previousValue_->size()) {
                throw std::runtime_error(
                    "Failed to save existing environment variable.");
            }
            previousValue_->resize(actualLength);
        }
        if (!SetEnvironmentVariableW(name_, value.c_str())) {
            throw std::runtime_error("Failed to set environment variable.");
        }
    }

    ~ScopedEnvironmentVariable()
    {
        const wchar_t* previousValue = previousValue_.has_value()
            ? previousValue_->c_str()
            : nullptr;
        (void)SetEnvironmentVariableW(name_, previousValue);
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) =
        delete;

protected:
    const wchar_t* name_;
    std::optional<std::wstring> previousValue_;
};

std::wstring utf8ToWideForTest(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int requiredLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (requiredLength <= 0) {
        throw std::runtime_error("Failed to measure UTF-8 test string.");
    }

    std::wstring result(static_cast<std::size_t>(requiredLength), L'\0');
    const int convertedLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        requiredLength);
    if (convertedLength != requiredLength) {
        throw std::runtime_error("Failed to convert UTF-8 test string.");
    }
    return result;
}

std::filesystem::path filesystemPathFromGenericUtf8ForTest(
    const std::string& pathText)
{
    return std::filesystem::path(utf8ToWideForTest(pathText));
}

std::string filesystemPathToGenericUtf8ForTest(
    const std::filesystem::path& path)
{
    const std::wstring value = path.generic_wstring();
    if (value.empty()) {
        return {};
    }
    const int requiredLength = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (requiredLength <= 0) {
        throw std::runtime_error("Failed to measure filesystem path.");
    }

    std::string result(static_cast<std::size_t>(requiredLength), '\0');
    const int convertedLength = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        requiredLength,
        nullptr,
        nullptr);
    if (convertedLength != requiredLength) {
        throw std::runtime_error("Failed to convert filesystem path.");
    }
    return result;
}

std::filesystem::path copyExecutableToScenarioRoot(
    const std::filesystem::path& scenarioRoot)
{
    std::filesystem::create_directories(scenarioRoot);
    const std::filesystem::path executablePath = currentExecutablePath();
    const std::filesystem::path copiedExecutablePath =
        scenarioRoot / executablePath.filename();
    std::filesystem::copy_file(executablePath,
                               copiedExecutablePath,
                               std::filesystem::copy_options::overwrite_existing);
    return copiedExecutablePath;
}

std::uint16_t reserveAvailableTcpPort()
{
    relaydesk::net::BoostAsioTcpPeerTransport probe(0);
    probe.start();
    const std::uint16_t port = probe.GetLocalPort();
    probe.stop();
    return port;
}

std::wstring quoteWindowsArgument(const std::wstring& value)
{
    std::wstring quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back(L'"');
    std::size_t slashCount = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashCount;
            continue;
        }
        if (character == L'"') {
            quoted.append(slashCount * 2 + 1, L'\\');
            quoted.push_back(character);
            slashCount = 0;
            continue;
        }
        quoted.append(slashCount, L'\\');
        slashCount = 0;
        quoted.push_back(character);
    }
    quoted.append(slashCount * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring makeWindowsCommandLine(
    const std::filesystem::path& executablePath,
    const std::vector<std::wstring>& arguments)
{
    std::wstring commandLine = quoteWindowsArgument(executablePath.wstring());
    for (const auto& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(argument);
    }
    return commandLine;
}

std::vector<std::wstring> currentProcessWideArgumentsForTest()
{
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return {};
    }

    std::vector<std::wstring> result;
    result.reserve(static_cast<std::size_t>(argumentCount));
    for (int index = 1; index < argumentCount; ++index) {
        result.emplace_back(arguments[index]);
    }
    LocalFree(arguments);
    return result;
}

std::vector<std::wstring> makeAppUpdateApplyArgumentsForTest(
    const relaydesk::runtime::AppUpdateApplyOptions& options)
{
    return {L"--relaydesk-apply-update",
            L"--target",
            options.GetTargetPath().wstring(),
            L"--payload",
            options.GetPayloadPath().wstring(),
            L"--pid",
            std::to_wstring(options.GetTargetProcessId()),
            L"--start-directory",
            options.GetStartDirectory().wstring(),
            L"--log",
            options.GetLogPath().wstring(),
            L"--restart",
            options.GetRestartAfterApply() ? L"1" : L"0"};
}

struct ChildProcessHandle {
    PROCESS_INFORMATION processInformation{};

    ~ChildProcessHandle()
    {
        close();
    }

    ChildProcessHandle() = default;
    ChildProcessHandle(const ChildProcessHandle&) = delete;
    ChildProcessHandle& operator=(const ChildProcessHandle&) = delete;

    ChildProcessHandle(ChildProcessHandle&& other) noexcept
        : processInformation(other.processInformation)
    {
        other.processInformation = {};
    }

    ChildProcessHandle& operator=(ChildProcessHandle&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        close();
        processInformation = other.processInformation;
        other.processInformation = {};
        return *this;
    }

    HANDLE processHandle() const { return processInformation.hProcess; }

    void close()
    {
        if (processInformation.hThread != nullptr) {
            CloseHandle(processInformation.hThread);
            processInformation.hThread = nullptr;
        }
        if (processInformation.hProcess != nullptr) {
            CloseHandle(processInformation.hProcess);
            processInformation.hProcess = nullptr;
        }
    }
};

ChildProcessHandle startChildProcess(
    const std::filesystem::path& executablePath,
    const std::vector<std::wstring>& arguments)
{
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);

    ChildProcessHandle childProcess;
    std::wstring commandLine =
        makeWindowsCommandLine(executablePath, arguments);
    std::wstring workingDirectory = executablePath.parent_path().wstring();
    const BOOL started = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectory.c_str(),
        &startupInfo,
        &childProcess.processInformation);
    if (!started) {
        throw std::runtime_error("Failed to start child test process.");
    }

    CloseHandle(childProcess.processInformation.hThread);
    childProcess.processInformation.hThread = nullptr;
    return childProcess;
}

DWORD waitForChildProcess(
    ChildProcessHandle& childProcess,
    std::chrono::milliseconds timeout)
{
    const DWORD waitResult = WaitForSingleObject(
        childProcess.processHandle(),
        static_cast<DWORD>(timeout.count()));
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(childProcess.processHandle(), 1);
        (void)WaitForSingleObject(childProcess.processHandle(), 5000);
    }

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(childProcess.processHandle(), &exitCode)) {
        throw std::runtime_error("Failed to read child process exit code.");
    }
    return exitCode;
}

void terminateChildProcessIfActive(ChildProcessHandle& childProcess)
{
    if (childProcess.processHandle() == nullptr) {
        return;
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(childProcess.processHandle(), &exitCode)
        && exitCode == STILL_ACTIVE) {
        TerminateProcess(childProcess.processHandle(), 1);
        (void)WaitForSingleObject(childProcess.processHandle(), 5000);
    }
}

struct ChildProcessTerminator {
    ChildProcessHandle& childProcess;

    ~ChildProcessTerminator()
    {
        terminateChildProcessIfActive(childProcess);
    }
};

DWORD runCommandAndWait(const std::filesystem::path& executablePath,
                        const std::vector<std::wstring>& arguments,
                        const std::filesystem::path& workingDirectory,
                        std::chrono::milliseconds timeout)
{
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);

    ChildProcessHandle process;
    std::wstring commandLine =
        makeWindowsCommandLine(executablePath, arguments);
    std::wstring workingDirectoryText = workingDirectory.wstring();
    const BOOL started = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectoryText.c_str(),
        &startupInfo,
        &process.processInformation);
    if (!started) {
        throw std::runtime_error("Failed to start command.");
    }

    CloseHandle(process.processInformation.hThread);
    process.processInformation.hThread = nullptr;
    return waitForChildProcess(process, timeout);
}

void removeScenarioRoot(const std::filesystem::path& scenarioRoot)
{
    removeTestDirectory(scenarioRoot);
}

template <typename Predicate>
bool waitForPredicate(Predicate&& predicate,
                      std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

void writeRawPeerFrame(boost::asio::ip::tcp::socket& socket,
                       const relaydesk::net::PeerFrame& frame)
{
    const std::array<std::uint8_t, relaydesk::net::kPeerFrameHeaderSize>
        frameHeader = relaydesk::net::encodePeerFrameHeader(frame);
    const std::array<boost::asio::const_buffer, 3> buffers{{
        boost::asio::buffer(frameHeader),
        boost::asio::buffer(frame.GetHeader()),
        boost::asio::buffer(frame.GetBody()),
    }};
    boost::asio::write(socket, buffers);
}

void writePartialRawPeerFrameBody(boost::asio::ip::tcp::socket& socket,
                                  const relaydesk::net::PeerFrame& frame,
                                  std::size_t bodyBytesToWrite)
{
    const std::array<std::uint8_t, relaydesk::net::kPeerFrameHeaderSize>
        frameHeader = relaydesk::net::encodePeerFrameHeader(frame);
    boost::asio::write(socket, boost::asio::buffer(frameHeader));
    boost::asio::write(socket, boost::asio::buffer(frame.GetHeader()));
    boost::asio::write(socket,
                       boost::asio::buffer(frame.GetBody().data(),
                                           bodyBytesToWrite));
}

relaydesk::net::TransferOfferMessage makeTransferOffer(
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    bool resumeRequest)
{
    relaydesk::net::TransferOfferMessage message;
    message.SetMessageId(messageId);
    message.SetPartId(partId);
    message.SetTransferId(transferId);
    message.SetSenderDeviceId("runtime-transfer-peer");
    message.SetFileName("resume.bin");
    message.SetFileSize(4);
    message.SetResumeRequest(resumeRequest);
    return message;
}

relaydesk::net::TransferChunkMessage makeTransferChunk(
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    std::uintmax_t offset)
{
    relaydesk::net::TransferChunkMessage message;
    message.SetMessageId(messageId);
    message.SetPartId(partId);
    message.SetTransferId(transferId);
    message.SetOffset(offset);
    return message;
}

relaydesk::net::TransferChunkMessage makeFolderTransferChunk(
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    std::uintmax_t offset,
    std::string relativePath,
    std::uintmax_t fileOffset,
    bool directory)
{
    relaydesk::net::TransferChunkMessage message =
        makeTransferChunk(messageId, partId, transferId, offset);
    message.SetFolderRelativePath(std::move(relativePath));
    message.SetFolderFileOffset(fileOffset);
    message.SetFolderDirectory(directory);
    return message;
}

relaydesk::net::TransferCompleteMessage makeTransferComplete(
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId)
{
    relaydesk::net::TransferCompleteMessage message;
    message.SetMessageId(messageId);
    message.SetPartId(partId);
    message.SetTransferId(transferId);
    message.SetFileSize(4);
    return message;
}

relaydesk::net::TransferOfferMessage makeFolderTransferOffer(
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    std::uintmax_t fileSize)
{
    relaydesk::net::TransferOfferMessage message;
    message.SetMessageId(messageId);
    message.SetPartId(partId);
    message.SetTransferId(transferId);
    message.SetSenderDeviceId("runtime-transfer-peer");
    message.SetFileName("Project");
    message.SetFileSize(fileSize);
    message.SetFolderTransfer(true);
    return message;
}

relaydesk::net::TransferCompleteMessage makeFolderTransferComplete(
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    std::uintmax_t fileSize)
{
    relaydesk::net::TransferCompleteMessage message;
    message.SetMessageId(messageId);
    message.SetPartId(partId);
    message.SetTransferId(transferId);
    message.SetFileSize(fileSize);
    return message;
}

relaydesk::storage::ChatMessageRecord makeInterruptedIncomingTransferRecord(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::runtime::LocalUserSummary& localUser,
    const std::filesystem::path& localPath,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::File);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Interrupted);
    part.SetFileName("resume.bin");
    part.SetFileSize(4);
    part.SetTransferredSize(2);
    part.SetLocalPath(makeWorkRelativePath(appPaths, localPath));

    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     "runtime-transfer-peer"));
    record.SetDirection(relaydesk::storage::MessageDirection::Incoming);
    record.SetSenderDeviceId("runtime-transfer-peer");
    record.SetReceiverDeviceId(localUser.GetDeviceId());
    record.SetSenderDisplayNameSnapshot("Runtime Transfer Peer");
    record.SetReceiverDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetCreatedAt("2026-06-20T10:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Received);
    record.SetParts({part});
    return record;
}

relaydesk::storage::ChatMessageRecord makeInterruptedIncomingFolderTransferRecord(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::runtime::LocalUserSummary& localUser,
    const std::filesystem::path& localPath,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::Folder);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Interrupted);
    part.SetFileName("Project");
    part.SetFileSize(4);
    part.SetTransferredSize(2);
    part.SetLocalPath(makeWorkRelativePath(appPaths, localPath));

    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     "runtime-transfer-peer"));
    record.SetDirection(relaydesk::storage::MessageDirection::Incoming);
    record.SetSenderDeviceId("runtime-transfer-peer");
    record.SetReceiverDeviceId(localUser.GetDeviceId());
    record.SetSenderDisplayNameSnapshot("Runtime Transfer Peer");
    record.SetReceiverDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetCreatedAt("2026-06-20T10:30:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Received);
    record.SetParts({part});
    return record;
}

relaydesk::runtime::RelayDeskRuntimeOptions makeTransferRuntimeOptions()
{
    relaydesk::runtime::RelayDeskRuntimeOptions options = makeTestRuntimeOptions();
    options.SetNetworkEnabled(true);
    return options;
}

relaydesk::storage::PeerProfile makeTransferPeerProfileForDevice(
    const std::string& deviceId,
    const std::string& displayName,
    std::uint16_t tcpPort)
{
    relaydesk::storage::PeerProfile profile;
    profile.SetDeviceId(deviceId);
    profile.SetHostName(displayName + "-HOST");
    profile.SetDisplayName(displayName);
    profile.SetLastAddresses({"127.0.0.1"});
    profile.SetTcpPort(tcpPort);
    profile.SetAppVersion(relaydesk::core::kAppVersion);
    profile.SetCapabilities({"text", "file"});
    profile.SetFirstSeenAt("2026-06-21T10:00:00Z");
    profile.SetLastSeenAt("2026-06-21T11:00:00Z");
    return profile;
}

relaydesk::storage::PeerProfile makeTransferPeerProfile(std::uint16_t tcpPort)
{
    return makeTransferPeerProfileForDevice("runtime-transfer-peer",
                                            "Runtime Transfer Peer",
                                            tcpPort);
}

relaydesk::runtime::LocalUserSummary makeProcessLocalUser(
    const std::string& deviceId,
    const std::string& hostName,
    const std::string& displayName)
{
    relaydesk::runtime::LocalUserSummary localUser;
    localUser.SetDeviceId(deviceId);
    localUser.SetHostName(hostName);
    localUser.SetDisplayName(displayName);
    return localUser;
}

relaydesk::storage::ChatMessageRecord makeOutgoingInterruptedTransferRecord(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::runtime::LocalUserSummary& localUser,
    const std::filesystem::path& localPath,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::File);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Interrupted);
    part.SetFileName("resume.bin");
    part.SetFileSize(4);
    part.SetTransferredSize(2);
    part.SetLocalPath(makeWorkRelativePath(appPaths, localPath));

    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     "runtime-transfer-peer"));
    record.SetDirection(relaydesk::storage::MessageDirection::Outgoing);
    record.SetSenderDeviceId(localUser.GetDeviceId());
    record.SetReceiverDeviceId("runtime-transfer-peer");
    record.SetSenderDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetReceiverDisplayNameSnapshot("Runtime Transfer Peer");
    record.SetCreatedAt("2026-06-21T12:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Pending);
    record.SetParts({part});
    return record;
}

struct ProcessInterruptedTransferRecordSeed {
    std::string peerDeviceId;
    std::string peerDisplayName;
    std::string messageId;
    std::string partId;
    std::string transferId;
    relaydesk::storage::MessageDirection direction =
        relaydesk::storage::MessageDirection::Outgoing;
    std::uintmax_t fileSize = 0;
    std::uintmax_t transferredSize = 0;
};

relaydesk::storage::ChatMessageRecord makeProcessInterruptedTransferRecord(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::runtime::LocalUserSummary& localUser,
    const std::filesystem::path& localPath,
    const ProcessInterruptedTransferRecordSeed& seed)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(seed.partId);
    part.SetType(relaydesk::storage::MessagePartType::File);
    part.SetTransferId(seed.transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Interrupted);
    part.SetFileName(localPath.filename().string());
    part.SetFileSize(seed.fileSize);
    part.SetTransferredSize(seed.transferredSize);
    part.SetLocalPath(makeWorkRelativePath(appPaths, localPath));

    const bool outgoing =
        seed.direction == relaydesk::storage::MessageDirection::Outgoing;

    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(seed.messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     seed.peerDeviceId));
    record.SetDirection(seed.direction);
    record.SetSenderDeviceId(outgoing ? localUser.GetDeviceId()
                                      : seed.peerDeviceId);
    record.SetReceiverDeviceId(outgoing ? seed.peerDeviceId
                                        : localUser.GetDeviceId());
    record.SetSenderDisplayNameSnapshot(outgoing ? localUser.GetDisplayName()
                                                 : seed.peerDisplayName);
    record.SetReceiverDisplayNameSnapshot(outgoing ? seed.peerDisplayName
                                                   : localUser.GetDisplayName());
    record.SetCreatedAt("2026-06-24T10:00:00Z");
    record.SetDeliveryState(outgoing ? relaydesk::storage::DeliveryState::Pending
                                     : relaydesk::storage::DeliveryState::Received);
    record.SetParts({part});
    return record;
}

relaydesk::storage::ChatMessageRecord makeIncomingFolderTransferRecord(
    const relaydesk::runtime::LocalUserSummary& localUser,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    std::uintmax_t fileSize)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::Folder);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Offered);
    part.SetFileName("Project");
    part.SetFileSize(fileSize);
    part.SetTransferredSize(0);

    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     "runtime-transfer-peer"));
    record.SetDirection(relaydesk::storage::MessageDirection::Incoming);
    record.SetSenderDeviceId("runtime-transfer-peer");
    record.SetReceiverDeviceId(localUser.GetDeviceId());
    record.SetSenderDisplayNameSnapshot("Runtime Transfer Peer");
    record.SetReceiverDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetCreatedAt("2026-06-22T10:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Received);
    record.SetParts({part});
    return record;
}

relaydesk::storage::ChatMessageRecord makeIncomingFileTransferRecord(
    const relaydesk::runtime::LocalUserSummary& localUser,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::File);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Offered);
    part.SetFileName("auto-received.bin");
    part.SetFileSize(4);
    part.SetTransferredSize(0);
    part.SetLocalPath("auto-received.bin");

    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     "runtime-transfer-peer"));
    record.SetDirection(relaydesk::storage::MessageDirection::Incoming);
    record.SetSenderDeviceId("runtime-transfer-peer");
    record.SetReceiverDeviceId(localUser.GetDeviceId());
    record.SetSenderDisplayNameSnapshot("Runtime Transfer Peer");
    record.SetReceiverDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetCreatedAt("2026-06-22T11:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Received);
    record.SetParts({part});
    return record;
}

relaydesk::storage::ChatMessageRecord makeOutgoingFolderTransferRecord(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::runtime::LocalUserSummary& localUser,
    const std::filesystem::path& localPath,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    std::uintmax_t fileSize)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::Folder);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Offered);
    part.SetFileName("Project");
    part.SetFileSize(fileSize);
    part.SetTransferredSize(0);
    part.SetLocalPath(makeWorkRelativePath(appPaths, localPath));

    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     "runtime-transfer-peer"));
    record.SetDirection(relaydesk::storage::MessageDirection::Outgoing);
    record.SetSenderDeviceId(localUser.GetDeviceId());
    record.SetReceiverDeviceId("runtime-transfer-peer");
    record.SetSenderDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetReceiverDisplayNameSnapshot("Runtime Transfer Peer");
    record.SetCreatedAt("2026-06-22T11:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Sent);
    record.SetParts({part});
    return record;
}

relaydesk::storage::ChatMessageRecord makeOutgoingInterruptedFolderTransferRecord(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::runtime::LocalUserSummary& localUser,
    const std::filesystem::path& localPath,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::Folder);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Interrupted);
    part.SetFileName("Project");
    part.SetFileSize(4);
    part.SetTransferredSize(2);
    part.SetLocalPath(makeWorkRelativePath(appPaths, localPath));

    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(messageId);
    record.SetConversationId(
        relaydesk::storage::makeDirectConversationId(localUser.GetDeviceId(),
                                                     "runtime-transfer-peer"));
    record.SetDirection(relaydesk::storage::MessageDirection::Outgoing);
    record.SetSenderDeviceId(localUser.GetDeviceId());
    record.SetReceiverDeviceId("runtime-transfer-peer");
    record.SetSenderDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetReceiverDisplayNameSnapshot("Runtime Transfer Peer");
    record.SetCreatedAt("2026-06-22T12:00:00Z");
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Pending);
    record.SetParts({part});
    return record;
}

relaydesk::net::PeerFrame makeTransferAcceptFrame(
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    const std::string& receiverDeviceId,
    std::uintmax_t resumeOffset)
{
    relaydesk::net::TransferAcceptMessage message;
    message.SetMessageId(messageId);
    message.SetPartId(partId);
    message.SetTransferId(transferId);
    message.SetReceiverDeviceId(receiverDeviceId);
    message.SetSaveStrategy(relaydesk::net::TransferSaveStrategy::Overwrite);
    message.SetResumeOffset(resumeOffset);
    return relaydesk::net::makeTransferAcceptFrame(message);
}

relaydesk::storage::ChatMessagePart makeProcessFileTransferPart(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& localPath,
    const std::string& partId,
    const std::string& transferId)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::File);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Pending);
    part.SetFileName(localPath.filename().string());
    part.SetFileSize(std::filesystem::file_size(localPath));
    part.SetTransferredSize(0);
    part.SetLocalPath(makeWorkRelativePath(appPaths, localPath));
    return part;
}

relaydesk::storage::ChatMessagePart makeProcessFolderTransferPart(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& localPath,
    const std::string& partId,
    const std::string& transferId,
    std::uintmax_t fileSize)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(partId);
    part.SetType(relaydesk::storage::MessagePartType::Folder);
    part.SetTransferId(transferId);
    part.SetTransferState(relaydesk::storage::TransferState::Pending);
    part.SetFileName(localPath.filename().string());
    part.SetFileSize(fileSize);
    part.SetTransferredSize(0);
    part.SetLocalPath(makeWorkRelativePath(appPaths, localPath));
    return part;
}

std::filesystem::path resolveWorkRelativePathForTest(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& pathText)
{
    std::filesystem::path path =
        filesystemPathFromGenericUtf8ForTest(pathText);
    if (path.is_absolute()) {
        return path;
    }
    return appPaths.GetWorkDirectory() / path;
}

std::string transferStateNameForTest(
    relaydesk::storage::TransferState state)
{
    switch (state) {
    case relaydesk::storage::TransferState::Pending:
        return "Pending";
    case relaydesk::storage::TransferState::Offered:
        return "Offered";
    case relaydesk::storage::TransferState::Transferring:
        return "Transferring";
    case relaydesk::storage::TransferState::Completed:
        return "Completed";
    case relaydesk::storage::TransferState::Failed:
        return "Failed";
    case relaydesk::storage::TransferState::Rejected:
        return "Rejected";
    case relaydesk::storage::TransferState::Cancelled:
        return "Cancelled";
    case relaydesk::storage::TransferState::Interrupted:
        return "Interrupted";
    }
    return "Unknown";
}

std::string describeSelectedTransferPartForTest(
    const relaydesk::storage::AppPaths& appPaths,
    const std::vector<relaydesk::storage::ChatMessageRecord>& messages,
    const std::string& partId)
{
    for (const auto& message : messages) {
        for (const auto& part : message.GetParts()) {
            if (part.GetPartId() != partId) {
                continue;
            }

            std::string status =
                "message_id=" + message.GetMessageId()
                + "\nlast_state="
                + (part.GetTransferState().has_value()
                       ? transferStateNameForTest(part.GetTransferState().value())
                       : "none")
                + "\nlast_transferred_size="
                + (part.GetTransferredSize().has_value()
                       ? std::to_string(part.GetTransferredSize().value())
                       : "none")
                + "\nlast_file_size="
                + (part.GetFileSize().has_value()
                       ? std::to_string(part.GetFileSize().value())
                       : "none");
            if (part.GetLocalPath().has_value()) {
                const std::filesystem::path localPath =
                    resolveWorkRelativePathForTest(appPaths,
                                                   part.GetLocalPath().value());
                status += "\nlast_local_path=" + part.GetLocalPath().value()
                    + "\nlast_local_exists="
                    + std::to_string(std::filesystem::exists(localPath) ? 1 : 0);
                if (std::filesystem::is_regular_file(localPath)) {
                    status += "\nlast_local_size="
                        + std::to_string(std::filesystem::file_size(localPath));
                }
            }
            return status;
        }
    }
    return "last_state=missing";
}

relaydesk::net::AppUpdateChunkMessage makeProcessAppUpdateChunkMessage(
    const std::string& requestId,
    std::uintmax_t offset,
    std::uintmax_t fileSize)
{
    relaydesk::net::AppUpdateChunkMessage message;
    message.SetRequestId(requestId);
    message.SetOffset(offset);
    message.SetFileSize(fileSize);
    return message;
}

relaydesk::net::AppUpdateCompleteMessage makeProcessAppUpdateCompleteMessage(
    const std::string& requestId,
    int appVersion,
    const std::string& fileName,
    std::uintmax_t fileSize)
{
    relaydesk::net::AppUpdateCompleteMessage message;
    message.SetRequestId(requestId);
    message.SetAppVersion(appVersion);
    message.SetFileName(fileName);
    message.SetFileSize(fileSize);
    return message;
}

void sendProcessAppUpdatePayloadFrames(
    relaydesk::net::BoostAsioTcpPeerTransport& source,
    const std::string& address,
    std::uint16_t port,
    const relaydesk::net::AppUpdateRequestMessage& request,
    const std::string& fileName,
    const std::vector<std::uint8_t>& payload)
{
    if (payload.empty()) {
        throw std::runtime_error("Process app update payload is empty.");
    }

    const std::uintmax_t fileSize =
        static_cast<std::uintmax_t>(payload.size());
    const auto sendChunk =
        [&](std::size_t offset, std::size_t byteCount) {
            if (byteCount == 0) {
                return;
            }

            using DifferenceType =
                std::vector<std::uint8_t>::difference_type;
            std::vector<std::uint8_t> body(
                payload.begin() + static_cast<DifferenceType>(offset),
                payload.begin()
                    + static_cast<DifferenceType>(offset + byteCount));
            source.sendFrameTo(
                address,
                port,
                relaydesk::net::makeAppUpdateChunkFrame(
                    makeProcessAppUpdateChunkMessage(
                        request.GetRequestId(),
                        static_cast<std::uintmax_t>(offset),
                        fileSize),
                    std::move(body)));
        };

    const std::size_t midpoint = payload.size() / 2;
    sendChunk(0, midpoint);
    sendChunk(midpoint, payload.size() - midpoint);
    source.sendFrameTo(
        address,
        port,
        relaydesk::net::makeAppUpdateCompleteFrame(
            makeProcessAppUpdateCompleteMessage(request.GetRequestId(),
                                                request.GetRequestedAppVersion(),
                                                fileName,
                                                fileSize)));
}

class TestableRelayDeskRuntime : public relaydesk::runtime::RelayDeskRuntime {
public:
    TestableRelayDeskRuntime()
        : RelayDeskRuntime(makeTestRuntimeOptions())
    {
        installHelperLauncher();
    }

    explicit TestableRelayDeskRuntime(relaydesk::runtime::RelayDeskRuntimeOptions options)
        : RelayDeskRuntime(std::move(options))
    {
        installHelperLauncher();
    }

    void receivePeerProfile(relaydesk::storage::PeerProfile profile, bool online)
    {
        enqueuePeerProfile(std::move(profile), online);
        refreshPeersIfNeeded();
    }

    void setLocalUserForProcessTest(std::string deviceId,
                                    std::string hostName,
                                    std::string displayName)
    {
        localUser_.SetDeviceId(std::move(deviceId));
        localUser_.SetHostName(std::move(hostName));
        localUser_.SetDisplayName(std::move(displayName));
    }

    void scheduleInstallOnExitUpdate(int appVersion)
    {
        relaydesk::runtime::PendingIncomingAppUpdate update;
        update.SetRequestId("scheduled-update-request");
        update.SetSourceDeviceId("runtime-update-peer");
        update.SetAppVersion(appVersion);
        update.SetFileName("relaydesk.exe");
        update.SetInstallMode(
            relaydesk::runtime::AppUpdateInstallMode::InstallOnExit);

        completeDownloadedAppUpdate(update);
    }

    void clearScheduledAppUpdate()
    {
        std::lock_guard lock(pendingAppUpdateMutex_);
        scheduledAppUpdate_.reset();
    }

    std::optional<relaydesk::runtime::PendingIncomingAppUpdate>
    getScheduledAppUpdate()
    {
        std::lock_guard lock(pendingAppUpdateMutex_);
        return scheduledAppUpdate_;
    }

    relaydesk::runtime::AppUpdateApplyOptions prepareScheduledAppUpdateForTest()
    {
        const std::optional<relaydesk::runtime::PendingIncomingAppUpdate>
            update = getScheduledAppUpdate();
        if (!update.has_value()) {
            throw std::runtime_error("No scheduled app update to apply.");
        }
        return prepareDownloadedAppUpdate(update.value(), false);
    }

    void applyDownloadedAppUpdateForTest(
        const relaydesk::runtime::PendingIncomingAppUpdate& update,
        bool restartAfterApply)
    {
        applyDownloadedAppUpdate(update, restartAfterApply);
    }

    bool GetHelperLaunchCalled() const { return helperLaunchCalled_; }

    const relaydesk::runtime::AppUpdateApplyOptions&
    GetLastHelperLaunchOptions() const
    {
        return lastHelperLaunchOptions_;
    }

    void loadSelectedTransferRecord(
        relaydesk::storage::ChatMessageRecord record)
    {
        selectedPeerDeviceId_ = "runtime-transfer-peer";
        selectedPeerMessages_ = {std::move(record)};
    }

    void seedAcceptedIncomingTransfer(const std::string& messageId,
                                      const std::string& partId,
                                      const std::string& transferId,
                                      const std::filesystem::path& payloadPath,
                                      std::uintmax_t receivedSize)
    {
        relaydesk::runtime::PendingIncomingTransfer transfer;
        transfer.SetSenderDeviceId("runtime-transfer-peer");
        transfer.SetMessageId(messageId);
        transfer.SetPartId(partId);
        transfer.SetTransferId(transferId);
        transfer.SetFileName("resume.bin");
        transfer.SetExpectedSize(4);
        transfer.SetReceivedSize(receivedSize);
        transfer.SetTempFilePath(payloadPath);
        transfer.SetFinalFilePath(payloadPath);

        std::lock_guard lock(pendingTransferMutex_);
        pendingIncomingTransfers_[transferId] = std::move(transfer);
    }

    void receivePeerFrame(relaydesk::net::PeerFrame frame)
    {
        handleIncomingPeerFrame(std::move(frame));
    }

    void drainTransferStateAndProgress()
    {
        drainPendingTransferStateUpdates();
        drainPendingTransferProgressUpdates();
    }

    void drainTransferCompletion()
    {
        drainPendingTransferUpdates();
    }

protected:
    void installHelperLauncher()
    {
        SetAppUpdateHelperLauncherForTest(
            [this](const relaydesk::runtime::AppUpdateApplyOptions& options) {
                helperLaunchCalled_ = true;
                lastHelperLaunchOptions_ = options;
                return true;
            });
    }

    bool helperLaunchCalled_ = false;
    relaydesk::runtime::AppUpdateApplyOptions lastHelperLaunchOptions_;
};

template <typename Predicate>
bool waitForRuntimeCondition(TestableRelayDeskRuntime& runtime,
                             Predicate&& predicate,
                             std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)::core::async::dispatchReady();
        runtime.refreshPeersIfNeeded();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)::core::async::dispatchReady();
    runtime.refreshPeersIfNeeded();
    return predicate();
}

template <typename Predicate>
bool waitForCondition(TestableRelayDeskRuntime& runtime, Predicate&& predicate)
{
    return waitForRuntimeCondition(runtime,
                                   std::forward<Predicate>(predicate),
                                   std::chrono::seconds(3));
}

int autoAcceptsIncomingFileTransferWhenEnabled()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
    } received;
    relaydesk::net::BoostAsioTcpPeerTransport sender(0);
    sender.SetFrameCallback(
        [&received](relaydesk::net::PeerFrame frame,
                    std::string,
                    std::uint16_t) {
            std::lock_guard lock(received.mutex);
            received.frames.push_back(std::move(frame));
        });
    sender.start();

    struct SenderStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& sender;
        ~SenderStopper() { sender.stop(); }
    } senderStopper{sender};

    {
        TestableRelayDeskRuntime runtime(makeTransferRuntimeOptions());
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }
        runtime.receivePeerProfile(
            makeTransferPeerProfile(sender.GetLocalPort()),
            true);
        runtime.SetAutoReceiveFilesEnabled(true);

        const std::string messageId = "auto-receive-message";
        const std::string partId = "auto-receive-part";
        const std::string transferId = "auto-receive-transfer";
        runtime.receivePeerFrame(relaydesk::net::makeChatMessageFrame(
            makeIncomingFileTransferRecord(runtime.GetLocalUser(),
                                            messageId,
                                            partId,
                                            transferId)));

        if (!waitForCondition(runtime, [&received] {
                std::lock_guard lock(received.mutex);
                return !received.frames.empty();
            })) {
            return fail("Enabled auto receive did not send transfer accept.");
        }

        std::lock_guard lock(received.mutex);
        const relaydesk::net::TransferAcceptMessage accept =
            relaydesk::net::parseTransferAcceptFrame(received.frames.front());
        if (const int result =
                expect(accept.GetMessageId() == messageId,
                       "Auto receive accept message id mismatch.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(accept.GetPartId() == partId,
                       "Auto receive accept part id mismatch.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(accept.GetTransferId() == transferId,
                       "Auto receive accept transfer id mismatch.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int queuesUserNotificationForIncomingChatMessage()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);

    {
        TestableRelayDeskRuntime runtime;
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        if (const int result =
                expect(runtime.ConsumePendingUserNotificationCount() == 0,
                       "Runtime should start without pending user notifications.");
            result != 0) {
            return result;
        }

        int notificationHandlerCalls = 0;
        runtime.SetUserNotificationHandler([&notificationHandlerCalls] {
            ++notificationHandlerCalls;
        });

        runtime.receivePeerFrame(relaydesk::net::makeChatMessageFrame(
            makeIncomingChatRecord(runtime.GetLocalUser())));
        if (const int result =
                expect(runtime.ConsumePendingUserNotificationCount() == 1,
                       "Incoming chat message did not queue a user notification.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(notificationHandlerCalls == 1,
                       "Incoming chat message did not invoke notification handler.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(runtime.ConsumePendingUserNotificationCount() == 0,
                       "User notification count was not consumed.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int persistsIncomingChatMessageBeforeUiDrain()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);
    relaydesk::storage::savePeerProfile(appPaths, makeTransferPeerProfile(39171));

    {
        TestableRelayDeskRuntime runtime;
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerFrame(relaydesk::net::makeChatMessageFrame(
            makeIncomingChatRecord(runtime.GetLocalUser())));

        const auto history =
            relaydesk::storage::loadChatHistory(appPaths, "runtime-transfer-peer");
        if (const int result =
                expect(history.GetRecords().size() == 1,
                       "Incoming chat message was not persisted before UI drain.");
            result != 0) {
            return result;
        }

        const auto profile =
            relaydesk::storage::loadPeerProfile(appPaths, "runtime-transfer-peer");
        if (const int result =
                expect(profile.GetUnreadMessageCount() == 1,
                       "Incoming chat unread count was not persisted before UI drain.");
            result != 0) {
            return result;
        }

        if (const int result =
                expect(runtime.GetSelectedPeerMessages().empty(),
                       "Unselected incoming chat message was rendered immediately.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int selectsPeerWithPagedHistory()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);
    relaydesk::storage::savePeerProfile(appPaths, makeTransferPeerProfile(39171));

    {
        TestableRelayDeskRuntime runtime;
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        for (int index = 1; index <= 65; ++index) {
            relaydesk::storage::appendChatMessage(
                appPaths,
                "runtime-transfer-peer",
                makeRuntimeHistoryTextRecord(runtime.GetLocalUser(), index));
        }

        runtime.selectPeer("runtime-transfer-peer");
        const auto& firstPage = runtime.GetSelectedPeerMessages();
        if (const int result = expect(firstPage.size() == 30,
                                      "Selected peer did not load 30 recent messages.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(firstPage.front().GetMessageId() == makeIndexedMessageId(36),
                       "Selected peer first recent message mismatch.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(firstPage.back().GetMessageId() == makeIndexedMessageId(65),
                       "Selected peer last recent message mismatch.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(runtime.GetSelectedPeerHasMoreMessages(),
                       "Selected peer did not expose older history.");
            result != 0) {
            return result;
        }

        runtime.loadMoreSelectedPeerMessages();
        const auto& secondPage = runtime.GetSelectedPeerMessages();
        if (const int result = expect(secondPage.size() == 60,
                                      "Loading older messages did not prepend a page.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(secondPage.front().GetMessageId() == makeIndexedMessageId(6),
                       "Second history page first message mismatch.");
            result != 0) {
            return result;
        }

        runtime.loadMoreSelectedPeerMessages();
        const auto& allMessages = runtime.GetSelectedPeerMessages();
        if (const int result = expect(allMessages.size() == 65,
                                      "Final history page size mismatch.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(allMessages.front().GetMessageId() == makeIndexedMessageId(1),
                       "Final history page first message mismatch.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(!runtime.GetSelectedPeerHasMoreMessages(),
                       "Runtime still reported older history after all messages loaded.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int offersAppUpdatePromptWhenOnlinePeerVersionIncreases()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);

    {
        TestableRelayDeskRuntime runtime;
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(
            makePeerProfile(relaydesk::core::kAppVersion,
                            "2026-06-18T10:10:00Z"),
            true);
        if (const int result = expect(!runtime.GetAppUpdatePrompt().has_value(),
                                      "Current-version peer showed update prompt.");
            result != 0) {
            return result;
        }

        runtime.receivePeerProfile(
            makePeerProfile(relaydesk::core::kAppVersion + 1,
                            "2026-06-18T10:05:00Z"),
            true);
        const std::optional<relaydesk::runtime::AppUpdatePrompt> prompt =
            runtime.GetAppUpdatePrompt();
        if (const int result = expect(prompt.has_value(),
                                      "Higher-version peer did not show prompt.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(prompt->GetSourceDeviceId() == "runtime-update-peer",
                       "Update prompt source peer mismatch.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(prompt->GetAppVersion()
                           == relaydesk::core::kAppVersion + 1,
                       "Update prompt app version mismatch.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int suppressesScheduledInstallOnExitVersionUntilHigherVersionArrives()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);

    {
        TestableRelayDeskRuntime runtime;
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        const int scheduledVersion = relaydesk::core::kAppVersion + 1;
        runtime.receivePeerProfile(
            makePeerProfile(scheduledVersion, "2026-06-18T10:20:00Z"),
            true);
        if (const int result = expect(runtime.GetAppUpdatePrompt().has_value(),
                                      "Initial higher-version peer did not show "
                                      "update prompt.");
            result != 0) {
            return result;
        }

        runtime.scheduleInstallOnExitUpdate(scheduledVersion);
        if (const int result =
                expect(!runtime.GetAppUpdatePrompt().has_value(),
                       "Scheduled install-on-exit update kept prompt visible.");
            result != 0) {
            return result;
        }

        runtime.receivePeerProfile(
            makePeerProfile(scheduledVersion, "2026-06-18T10:30:00Z"),
            true);
        if (const int result =
                expect(!runtime.GetAppUpdatePrompt().has_value(),
                       "Scheduled install-on-exit version was offered again.");
            result != 0) {
            return result;
        }

        runtime.receivePeerProfile(
            makePeerProfile(scheduledVersion + 1, "2026-06-18T10:40:00Z"),
            true);
        const std::optional<relaydesk::runtime::AppUpdatePrompt> prompt =
            runtime.GetAppUpdatePrompt();
        if (const int result =
                expect(prompt.has_value(),
                       "Higher version after scheduled update did not show prompt.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(prompt->GetAppVersion() == scheduledVersion + 1,
                       "Higher version prompt app version mismatch.");
            result != 0) {
            return result;
        }

        runtime.clearScheduledAppUpdate();
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int keepsHighestAppUpdatePromptAcrossMultiplePeers()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);

    {
        TestableRelayDeskRuntime runtime;
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(
            makePeerProfileForDevice("runtime-update-peer-a",
                                     "Runtime Update Peer A",
                                     relaydesk::core::kAppVersion + 1,
                                     "2026-06-18T10:20:00Z"),
            true);
        std::optional<relaydesk::runtime::AppUpdatePrompt> prompt =
            runtime.GetAppUpdatePrompt();
        if (const int result =
                expect(prompt.has_value(),
                       "Initial higher-version peer did not show prompt.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(prompt->GetAppVersion() == relaydesk::core::kAppVersion + 1,
                       "Initial update prompt version mismatch.");
            result != 0) {
            return result;
        }

        runtime.receivePeerProfile(
            makePeerProfileForDevice("runtime-update-peer-b",
                                     "Runtime Update Peer B",
                                     relaydesk::core::kAppVersion + 3,
                                     "2026-06-18T10:21:00Z"),
            true);
        prompt = runtime.GetAppUpdatePrompt();
        if (const int result =
                expect(prompt.has_value()
                           && prompt->GetSourceDeviceId()
                               == "runtime-update-peer-b",
                       "Higher-version peer did not become update source.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(prompt->GetAppVersion() == relaydesk::core::kAppVersion + 3,
                       "Higher-version prompt app version mismatch.");
            result != 0) {
            return result;
        }

        runtime.receivePeerProfile(
            makePeerProfileForDevice("runtime-update-peer-a",
                                     "Runtime Update Peer A",
                                     relaydesk::core::kAppVersion + 1,
                                     "2026-06-18T10:22:00Z"),
            true);
        prompt = runtime.GetAppUpdatePrompt();
        if (const int result =
                expect(prompt.has_value()
                           && prompt->GetSourceDeviceId()
                               == "runtime-update-peer-b",
                       "Lower-version peer downgraded the update source.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(prompt->GetAppVersion() == relaydesk::core::kAppVersion + 3,
                       "Lower-version peer downgraded the prompt version.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int requestsProcessExitAfterLaunchingRestartUpdateHelper()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    {
        TestableRelayDeskRuntime runtime;
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        const std::filesystem::path updatePath =
            appPaths.GetTempTransfersDirectory()
            / "updates"
            / "restart-now-test"
            / currentExecutablePath().filename();
        const std::vector<std::uint8_t> updatePayload =
            readBytes(currentExecutablePath());
        writeBytes(updatePath, updatePayload);

        relaydesk::runtime::PendingIncomingAppUpdate update;
        update.SetRequestId("restart-now-test");
        update.SetSourceDeviceId("runtime-update-peer");
        update.SetAppVersion(relaydesk::core::kAppVersion + 1);
        update.SetFileName(currentExecutablePath().filename().string());
        update.SetTempFilePath(updatePath);
        update.SetExpectedSize(
            static_cast<std::uintmax_t>(updatePayload.size()));
        update.SetReceivedSize(
            static_cast<std::uintmax_t>(updatePayload.size()));
        update.SetInstallMode(
            relaydesk::runtime::AppUpdateInstallMode::RestartNow);

        runtime.applyDownloadedAppUpdateForTest(update, true);
        if (const int result =
                expect(runtime.GetHelperLaunchCalled(),
                       "Restart update did not launch the helper.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(runtime.GetLastHelperLaunchOptions()
                           .GetRestartAfterApply(),
                       "Restart update did not request helper restart.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(runtime.GetAppUpdateExitRequested(),
                       "Restart update did not request app exit.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int continuesInterruptedIncomingTransferFromAcceptedOffset()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::filesystem::path finalPath =
        appPaths.GetInboxDirectory() / "resume.bin";
    writeBytes(finalPath, {'A', 'B'});

    {
        TestableRelayDeskRuntime runtime;
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        const std::string messageId = "resume-message";
        const std::string partId = "resume-part";
        const std::string transferId = "resume-transfer";
        runtime.loadSelectedTransferRecord(
            makeInterruptedIncomingTransferRecord(appPaths,
                                                  runtime.GetLocalUser(),
                                                  finalPath,
                                                  messageId,
                                                  partId,
                                                  transferId));

        runtime.seedAcceptedIncomingTransfer(messageId,
                                             partId,
                                             transferId,
                                             finalPath,
                                             2);
        runtime.receivePeerFrame(relaydesk::net::makeTransferOfferFrame(
            makeTransferOffer(messageId, partId, transferId, false)));
        runtime.receivePeerFrame(relaydesk::net::makeTransferChunkFrame(
            makeTransferChunk(messageId, partId, transferId, 2),
            {'C', 'D'}));
        runtime.receivePeerFrame(relaydesk::net::makeTransferCompleteFrame(
            makeTransferComplete(messageId, partId, transferId)));
        runtime.drainTransferCompletion();

        const std::vector<std::uint8_t> content = readBytes(finalPath);
        if (const int result =
                expect(content == std::vector<std::uint8_t>({'A', 'B', 'C', 'D'}),
                       "Resumed transfer did not append from accepted offset.");
            result != 0) {
            return result;
        }

        const auto& messages = runtime.GetSelectedPeerMessages();
        if (const int result = expect(!messages.empty(),
                                      "Resumed transfer message was not retained.");
            result != 0) {
            return result;
        }
        const auto state = messages.front().GetParts().front().GetTransferState();
        if (const int result =
                expect(state.has_value()
                           && state.value()
                                  == relaydesk::storage::TransferState::Completed,
                       "Resumed transfer did not complete.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int restartsInterruptedIncomingTransferWhenFreshOfferStartsAtZero()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::filesystem::path finalPath =
        appPaths.GetInboxDirectory() / "resume.bin";
    writeBytes(finalPath, {'A', 'B'});

    {
        TestableRelayDeskRuntime runtime;
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        const std::string messageId = "fresh-message";
        const std::string partId = "fresh-part";
        const std::string transferId = "fresh-transfer";
        runtime.loadSelectedTransferRecord(
            makeInterruptedIncomingTransferRecord(appPaths,
                                                  runtime.GetLocalUser(),
                                                  finalPath,
                                                  messageId,
                                                  partId,
                                                  transferId));

        try {
            runtime.receivePeerFrame(relaydesk::net::makeTransferOfferFrame(
                makeTransferOffer(messageId, partId, transferId, false)));
            runtime.receivePeerFrame(relaydesk::net::makeTransferChunkFrame(
                makeTransferChunk(messageId, partId, transferId, 0),
                {'W', 'X', 'Y', 'Z'}));
            runtime.receivePeerFrame(relaydesk::net::makeTransferCompleteFrame(
                makeTransferComplete(messageId, partId, transferId)));
        } catch (const std::exception& error) {
            return fail(std::string("Fresh transfer from zero failed: ")
                        + error.what());
        }
        runtime.drainTransferCompletion();

        const std::vector<std::uint8_t> content = readBytes(finalPath);
        if (const int result =
                expect(content == std::vector<std::uint8_t>({'W', 'X', 'Y', 'Z'}),
                       "Fresh transfer did not replace interrupted payload.");
            result != 0) {
            return result;
        }

        const auto& messages = runtime.GetSelectedPeerMessages();
        const auto state = messages.front().GetParts().front().GetTransferState();
        if (const int result =
                expect(state.has_value()
                           && state.value()
                                  == relaydesk::storage::TransferState::Completed,
                       "Fresh transfer did not complete after interrupted state.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int answersResumeRequestFromStoredHistoryOffset()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::filesystem::path finalPath =
        appPaths.GetInboxDirectory() / "resume.bin";
    writeBytes(finalPath, {'A', 'B'});

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
    };

    ReceivedTransferFrames received;
    relaydesk::net::BoostAsioTcpPeerTransport sender(0);
    sender.SetFrameCallback(
        [&received](relaydesk::net::PeerFrame frame,
                    std::string,
                    std::uint16_t) {
            std::lock_guard lock(received.mutex);
            received.frames.push_back(std::move(frame));
        });
    sender.start();

    struct SenderStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& sender;

        ~SenderStopper()
        {
            sender.stop();
        }
    } senderStopper{sender};

    {
        TestableRelayDeskRuntime runtime(makeTransferRuntimeOptions());
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(
            makeTransferPeerProfile(sender.GetLocalPort()),
            true);

        const std::string messageId = "history-resume-message";
        const std::string partId = "history-resume-part";
        const std::string transferId = "history-resume-transfer";
        relaydesk::storage::appendChatMessage(
            appPaths,
            "runtime-transfer-peer",
            makeInterruptedIncomingTransferRecord(appPaths,
                                                  runtime.GetLocalUser(),
                                                  finalPath,
                                                  messageId,
                                                  partId,
                                                  transferId));

        runtime.receivePeerFrame(relaydesk::net::makeTransferOfferFrame(
            makeTransferOffer(messageId, partId, transferId, true)));
        if (!waitForCondition(runtime, [&received] {
                std::lock_guard lock(received.mutex);
                return received.frames.size() >= 1;
            })) {
            return fail("Resume request from stored history was not answered.");
        }

        std::lock_guard lock(received.mutex);
        const relaydesk::net::TransferAcceptMessage accept =
            relaydesk::net::parseTransferAcceptFrame(received.frames[0]);
        if (const int result = expect(accept.GetResumeOffset() == 2,
                                      "Stored history resume offset mismatch.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int answersRepeatedResumeRequestsWithStoredOffset()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::filesystem::path finalPath =
        appPaths.GetInboxDirectory() / "resume.bin";
    writeBytes(finalPath, {'A', 'B'});

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
    };

    ReceivedTransferFrames received;
    relaydesk::net::BoostAsioTcpPeerTransport sender(0);
    sender.SetFrameCallback(
        [&received](relaydesk::net::PeerFrame frame,
                    std::string,
                    std::uint16_t) {
            std::lock_guard lock(received.mutex);
            received.frames.push_back(std::move(frame));
        });
    sender.start();

    struct SenderStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& sender;

        ~SenderStopper()
        {
            sender.stop();
        }
    } senderStopper{sender};

    {
        TestableRelayDeskRuntime runtime(makeTransferRuntimeOptions());
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(
            makeTransferPeerProfile(sender.GetLocalPort()),
            true);

        const std::string messageId = "repeat-resume-message";
        const std::string partId = "repeat-resume-part";
        const std::string transferId = "repeat-resume-transfer";
        runtime.loadSelectedTransferRecord(
            makeInterruptedIncomingTransferRecord(appPaths,
                                                  runtime.GetLocalUser(),
                                                  finalPath,
                                                  messageId,
                                                  partId,
                                                  transferId));

        runtime.receivePeerFrame(relaydesk::net::makeTransferOfferFrame(
            makeTransferOffer(messageId, partId, transferId, true)));
        if (!waitForCondition(runtime, [&received] {
                std::lock_guard lock(received.mutex);
                return received.frames.size() >= 1;
            })) {
            return fail("Initial resume request was not answered.");
        }

        runtime.loadSelectedTransferRecord(
            makeInterruptedIncomingTransferRecord(appPaths,
                                                  runtime.GetLocalUser(),
                                                  finalPath,
                                                  messageId,
                                                  partId,
                                                  transferId));
        runtime.receivePeerFrame(relaydesk::net::makeTransferOfferFrame(
            makeTransferOffer(messageId, partId, transferId, true)));
        if (!waitForCondition(runtime, [&received] {
                std::lock_guard lock(received.mutex);
                return received.frames.size() >= 2;
            })) {
            return fail("Repeated resume request was not answered.");
        }

        std::lock_guard lock(received.mutex);
        for (const relaydesk::net::PeerFrame& frame : received.frames) {
            const relaydesk::net::TransferAcceptMessage accept =
                relaydesk::net::parseTransferAcceptFrame(frame);
            if (const int result =
                    expect(accept.GetResumeOffset() == 2,
                           "Repeated resume accept offset mismatch.");
                result != 0) {
                return result;
            }
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int acceptsRepeatedInterruptedIncomingTransferWithStoredOffset()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::filesystem::path finalPath =
        appPaths.GetInboxDirectory() / "resume.bin";
    writeBytes(finalPath, {'A', 'B'});

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
    };

    ReceivedTransferFrames received;
    relaydesk::net::BoostAsioTcpPeerTransport sender(0);
    sender.SetFrameCallback(
        [&received](relaydesk::net::PeerFrame frame,
                    std::string,
                    std::uint16_t) {
            std::lock_guard lock(received.mutex);
            received.frames.push_back(std::move(frame));
        });
    sender.start();

    struct SenderStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& sender;

        ~SenderStopper()
        {
            sender.stop();
        }
    } senderStopper{sender};

    {
        TestableRelayDeskRuntime runtime(makeTransferRuntimeOptions());
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(
            makeTransferPeerProfile(sender.GetLocalPort()),
            true);

        const std::string messageId = "repeat-accept-message";
        const std::string partId = "repeat-accept-part";
        const std::string transferId = "repeat-accept-transfer";
        runtime.loadSelectedTransferRecord(
            makeInterruptedIncomingTransferRecord(appPaths,
                                                  runtime.GetLocalUser(),
                                                  finalPath,
                                                  messageId,
                                                  partId,
                                                  transferId));

        runtime.acceptSelectedPeerFileTransfer(messageId, partId, true);
        if (!waitForCondition(runtime, [&received] {
                std::lock_guard lock(received.mutex);
                return received.frames.size() >= 1;
            })) {
            return fail("Initial interrupted transfer accept was not sent.");
        }

        runtime.loadSelectedTransferRecord(
            makeInterruptedIncomingTransferRecord(appPaths,
                                                  runtime.GetLocalUser(),
                                                  finalPath,
                                                  messageId,
                                                  partId,
                                                  transferId));
        runtime.acceptSelectedPeerFileTransfer(messageId, partId, true);
        if (!waitForCondition(runtime, [&received] {
                std::lock_guard lock(received.mutex);
                return received.frames.size() >= 2;
            })) {
            return fail("Repeated interrupted transfer accept was not sent.");
        }

        std::lock_guard lock(received.mutex);
        for (const relaydesk::net::PeerFrame& frame : received.frames) {
            const relaydesk::net::TransferAcceptMessage accept =
                relaydesk::net::parseTransferAcceptFrame(frame);
            if (const int result =
                    expect(accept.GetResumeOffset() == 2,
                           "Repeated interrupted accept offset mismatch.");
                result != 0) {
                return result;
            }
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int resumesInterruptedOutgoingTransferAfterResumeAccept()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::filesystem::path sourcePath =
        appPaths.GetWorkDirectory() / "resume-source.bin";
    writeBytes(sourcePath, {'A', 'B', 'C', 'D'});

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
    };

    ReceivedTransferFrames received;
    relaydesk::net::BoostAsioTcpPeerTransport receiver(0);
    receiver.SetFrameCallback(
        [&received](relaydesk::net::PeerFrame frame,
                    std::string,
                    std::uint16_t) {
            std::lock_guard lock(received.mutex);
            received.frames.push_back(std::move(frame));
        });
    receiver.start();

    struct ReceiverStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& receiver;

        ~ReceiverStopper()
        {
            receiver.stop();
        }
    } receiverStopper{receiver};

    {
        TestableRelayDeskRuntime runtime(makeTransferRuntimeOptions());
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(
            makeTransferPeerProfile(receiver.GetLocalPort()),
            true);

        const std::string messageId = "outgoing-resume-message";
        const std::string partId = "outgoing-resume-part";
        const std::string transferId = "outgoing-resume-transfer";
        runtime.loadSelectedTransferRecord(
            makeOutgoingInterruptedTransferRecord(appPaths,
                                                  runtime.GetLocalUser(),
                                                  sourcePath,
                                                  messageId,
                                                  partId,
                                                  transferId));

        runtime.sendSelectedPeerFileTransfer(messageId, partId);
        if (!waitForCondition(runtime, [&received] {
                std::lock_guard lock(received.mutex);
                return received.frames.size() >= 1;
            })) {
            return fail("Outgoing resume request was not sent.");
        }

        {
            std::lock_guard lock(received.mutex);
            const relaydesk::net::TransferOfferMessage resumeOffer =
                relaydesk::net::parseTransferOfferFrame(received.frames[0]);
            if (const int result =
                    expect(resumeOffer.GetResumeRequest(),
                           "Outgoing interrupted transfer did not ask to resume.");
                result != 0) {
                return result;
            }
            if (const int result =
                    expect(resumeOffer.GetSenderDeviceId()
                               == runtime.GetLocalUser().GetDeviceId(),
                           "Resume request sender device mismatch.");
                result != 0) {
                return result;
            }
        }

        runtime.receivePeerFrame(
            makeTransferAcceptFrame(messageId,
                                    partId,
                                    transferId,
                                    "runtime-transfer-peer",
                                    2));

        if (!waitForCondition(runtime, [&runtime, &received] {
                std::lock_guard lock(received.mutex);
                if (received.frames.size() < 4) {
                    return false;
                }
                const auto& messages = runtime.GetSelectedPeerMessages();
                if (messages.empty() || messages.front().GetParts().empty()) {
                    return false;
                }
                const auto& part = messages.front().GetParts().front();
                return part.GetTransferState().has_value()
                    && part.GetTransferState().value()
                        == relaydesk::storage::TransferState::Completed;
            })) {
            return fail("Outgoing transfer did not continue after resume accept.");
        }

        {
            std::lock_guard lock(received.mutex);
            const relaydesk::net::TransferOfferMessage sendOffer =
                relaydesk::net::parseTransferOfferFrame(received.frames[1]);
            if (const int result =
                    expect(!sendOffer.GetResumeRequest(),
                           "Data transfer offer should not be a resume request.");
                result != 0) {
                return result;
            }

            const relaydesk::net::TransferChunkMessage chunk =
                relaydesk::net::parseTransferChunkFrame(received.frames[2]);
            if (const int result = expect(chunk.GetOffset() == 2,
                                          "Resumed chunk offset mismatch.");
                result != 0) {
                return result;
            }
            if (const int result =
                    expect(received.frames[2].GetBody()
                               == std::vector<std::uint8_t>({'C', 'D'}),
                           "Resumed chunk body mismatch.");
                result != 0) {
                return result;
            }

            const relaydesk::net::TransferCompleteMessage complete =
                relaydesk::net::parseTransferCompleteFrame(received.frames[3]);
            if (const int result =
                    expect(complete.GetFileSize() == 4,
                           "Resumed transfer complete size mismatch.");
                result != 0) {
                return result;
            }
        }

        const auto& messages = runtime.GetSelectedPeerMessages();
        if (const int result = expect(!messages.empty(),
                                      "Outgoing transfer message was lost.");
            result != 0) {
            return result;
        }
        const auto& part = messages.front().GetParts().front();
        if (const int result =
                expect(part.GetTransferState().has_value()
                           && part.GetTransferState().value()
                               == relaydesk::storage::TransferState::Completed,
                       "Outgoing transfer did not complete after resume.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(part.GetTransferredSize().has_value()
                           && part.GetTransferredSize().value() == 4,
                       "Outgoing transfer progress did not reach the file size.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int acceptsIncomingFolderTransferAndExtractsPayload()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::uint16_t runtimeListenPort = reserveAvailableTcpPort();
    relaydesk::runtime::RelayDeskRuntimeOptions options =
        makeTransferRuntimeOptions();
    options.SetTcpListenPort(runtimeListenPort);

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
    };

    ReceivedTransferFrames received;
    relaydesk::net::BoostAsioTcpPeerTransport sender(0);
    sender.SetFrameCallback(
        [&received](relaydesk::net::PeerFrame frame,
                    std::string,
                    std::uint16_t) {
            std::lock_guard lock(received.mutex);
            received.frames.push_back(std::move(frame));
        });
    sender.start();

    struct SenderStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& sender;

        ~SenderStopper()
        {
            sender.stop();
        }
    } senderStopper{sender};

    const std::string messageId = "incoming-folder-message";
    const std::string partId = "incoming-folder-part";
    const std::string transferId = "incoming-folder-transfer";
    constexpr std::uintmax_t folderPayloadSize = 4;

    {
        TestableRelayDeskRuntime runtime(options);
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(
            makeTransferPeerProfile(sender.GetLocalPort()),
            true);
        runtime.loadSelectedTransferRecord(
            makeIncomingFolderTransferRecord(runtime.GetLocalUser(),
                                             messageId,
                                             partId,
                                             transferId,
                                             4));

        runtime.acceptSelectedPeerFileTransfer(messageId, partId, false);
        if (!waitForCondition(runtime, [&received] {
                std::lock_guard lock(received.mutex);
                return !received.frames.empty();
            })) {
            return fail("Incoming folder accept frame was not sent.");
        }

        {
            std::lock_guard lock(received.mutex);
            const relaydesk::net::TransferAcceptMessage accept =
                relaydesk::net::parseTransferAcceptFrame(received.frames.front());
            if (const int result =
                    expect(accept.GetTransferId() == transferId,
                           "Incoming folder accept transfer id mismatch.");
                result != 0) {
                return result;
            }
        }

        std::vector<relaydesk::net::PeerFrame> payloadFrames;
        payloadFrames.push_back(
            relaydesk::net::makeTransferOfferFrame(
                makeFolderTransferOffer(messageId,
                                        partId,
                                        transferId,
                                        folderPayloadSize)));
        payloadFrames.push_back(
            relaydesk::net::makeTransferChunkFrame(
                makeFolderTransferChunk(messageId,
                                        partId,
                                        transferId,
                                        0,
                                        "nested",
                                        0,
                                        true),
                {}));
        payloadFrames.push_back(
            relaydesk::net::makeTransferChunkFrame(
                makeFolderTransferChunk(messageId,
                                        partId,
                                        transferId,
                                        0,
                                        "root.txt",
                                        0,
                                        false),
                std::vector<std::uint8_t>({'A', 'B'})));
        payloadFrames.push_back(
            relaydesk::net::makeTransferChunkFrame(
                makeFolderTransferChunk(messageId,
                                        partId,
                                        transferId,
                                        2,
                                        "nested/child.txt",
                                        0,
                                        false),
                std::vector<std::uint8_t>({'C', 'D'})));
        payloadFrames.push_back(
            relaydesk::net::makeTransferCompleteFrame(
                makeFolderTransferComplete(messageId,
                                           partId,
                                           transferId,
                                           folderPayloadSize)));
        std::size_t payloadFrameIndex = 0;
        sender.sendFramesTo(
            "127.0.0.1",
            runtimeListenPort,
            [&payloadFrames,
             &payloadFrameIndex]() -> std::optional<relaydesk::net::PeerFrame> {
                if (payloadFrameIndex >= payloadFrames.size()) {
                    return std::nullopt;
                }
                return payloadFrames[payloadFrameIndex++];
            });

        if (!waitForCondition(runtime, [&runtime] {
                const auto& messages = runtime.GetSelectedPeerMessages();
                if (messages.empty() || messages.front().GetParts().empty()) {
                    return false;
                }
                const auto& part = messages.front().GetParts().front();
                return part.GetTransferState().has_value()
                    && part.GetTransferState().value()
                        == relaydesk::storage::TransferState::Completed;
            })) {
            return fail("Incoming folder transfer did not complete.");
        }

        const std::filesystem::path targetRoot =
            appPaths.GetInboxDirectory() / "Project";
        if (const int result = expect(readTextFile(targetRoot / "root.txt") == "AB",
                                      "Incoming folder root file mismatch.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(readTextFile(targetRoot / "nested" / "child.txt") == "CD",
                       "Incoming folder nested file mismatch.");
            result != 0) {
            return result;
        }

        const auto& part = runtime.GetSelectedPeerMessages()
                               .front()
                               .GetParts()
                               .front();
        if (const int result =
                expect(part.GetFileSize().has_value()
                           && part.GetFileSize().value() == folderPayloadSize,
                       "Incoming folder file size did not use file content size.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(part.GetTransferredSize().has_value()
                           && part.GetTransferredSize().value()
                               == folderPayloadSize,
                       "Incoming folder progress did not reach file content size.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(part.GetLocalPath().has_value()
                           && part.GetLocalPath().value()
                               == makeWorkRelativePath(appPaths, targetRoot),
                       "Incoming folder local path mismatch.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int resumesInterruptedIncomingFolderTransferFromExistingFiles()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::filesystem::path targetRoot =
        appPaths.GetInboxDirectory() / "Project";
    writeBytes(targetRoot / "a-root.txt", {'A', 'B'});

    const std::uint16_t runtimeListenPort = reserveAvailableTcpPort();
    relaydesk::runtime::RelayDeskRuntimeOptions options =
        makeTransferRuntimeOptions();
    options.SetTcpListenPort(runtimeListenPort);

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
    };

    ReceivedTransferFrames received;
    relaydesk::net::BoostAsioTcpPeerTransport sender(0);
    sender.SetFrameCallback(
        [&received](relaydesk::net::PeerFrame frame,
                    std::string,
                    std::uint16_t) {
            std::lock_guard lock(received.mutex);
            received.frames.push_back(std::move(frame));
        });
    sender.start();

    struct SenderStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& sender;

        ~SenderStopper()
        {
            sender.stop();
        }
    } senderStopper{sender};

    const std::string messageId = "incoming-folder-resume-message";
    const std::string partId = "incoming-folder-resume-part";
    const std::string transferId = "incoming-folder-resume-transfer";
    constexpr std::uintmax_t folderPayloadSize = 4;

    {
        TestableRelayDeskRuntime runtime(options);
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(
            makeTransferPeerProfile(sender.GetLocalPort()),
            true);
        runtime.loadSelectedTransferRecord(
            makeInterruptedIncomingFolderTransferRecord(appPaths,
                                                        runtime.GetLocalUser(),
                                                        targetRoot,
                                                        messageId,
                                                        partId,
                                                        transferId));

        runtime.acceptSelectedPeerFileTransfer(messageId, partId, true);
        if (!waitForCondition(runtime, [&received] {
                std::lock_guard lock(received.mutex);
                return !received.frames.empty();
            })) {
            return fail("Interrupted folder accept frame was not sent.");
        }

        {
            std::lock_guard lock(received.mutex);
            const relaydesk::net::TransferAcceptMessage accept =
                relaydesk::net::parseTransferAcceptFrame(received.frames.front());
            if (const int result =
                    expect(accept.GetResumeOffset() == 2,
                           "Interrupted folder accept offset mismatch.");
                result != 0) {
                return result;
            }
        }

        std::vector<relaydesk::net::PeerFrame> payloadFrames;
        payloadFrames.push_back(
            relaydesk::net::makeTransferOfferFrame(
                makeFolderTransferOffer(messageId,
                                        partId,
                                        transferId,
                                        folderPayloadSize)));
        payloadFrames.push_back(
            relaydesk::net::makeTransferChunkFrame(
                makeFolderTransferChunk(messageId,
                                        partId,
                                        transferId,
                                        2,
                                        "nested/child.txt",
                                        0,
                                        false),
                std::vector<std::uint8_t>({'C', 'D'})));
        payloadFrames.push_back(
            relaydesk::net::makeTransferCompleteFrame(
                makeFolderTransferComplete(messageId,
                                           partId,
                                           transferId,
                                           folderPayloadSize)));
        std::size_t payloadFrameIndex = 0;
        sender.sendFramesTo(
            "127.0.0.1",
            runtimeListenPort,
            [&payloadFrames,
             &payloadFrameIndex]() -> std::optional<relaydesk::net::PeerFrame> {
                if (payloadFrameIndex >= payloadFrames.size()) {
                    return std::nullopt;
                }
                return payloadFrames[payloadFrameIndex++];
            });

        if (!waitForCondition(runtime, [&runtime] {
                const auto& messages = runtime.GetSelectedPeerMessages();
                if (messages.empty() || messages.front().GetParts().empty()) {
                    return false;
                }
                const auto& part = messages.front().GetParts().front();
                return part.GetTransferState().has_value()
                    && part.GetTransferState().value()
                        == relaydesk::storage::TransferState::Completed;
            })) {
            return fail("Interrupted folder transfer did not resume.");
        }

        if (const int result = expect(readTextFile(targetRoot / "a-root.txt") == "AB",
                                      "Resumed folder root file was lost.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(readTextFile(targetRoot / "nested" / "child.txt") == "CD",
                       "Resumed folder nested file mismatch.");
            result != 0) {
            return result;
        }

        const auto& part = runtime.GetSelectedPeerMessages()
                               .front()
                               .GetParts()
                               .front();
        if (const int result =
                expect(part.GetTransferredSize().has_value()
                           && part.GetTransferredSize().value()
                               == folderPayloadSize,
                       "Resumed folder progress did not reach content size.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int sendsOutgoingFolderTransferWithPackageProgressAndSourcePath()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::filesystem::path sourceFolder =
        appPaths.GetWorkDirectory() / "Project";
    writeBytes(sourceFolder / "root.txt", {'A', 'B'});
    writeBytes(sourceFolder / "nested" / "child.txt", {'C', 'D'});

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
    };

    ReceivedTransferFrames received;
    relaydesk::net::BoostAsioTcpPeerTransport receiver(0);
    receiver.SetFrameCallback(
        [&received](relaydesk::net::PeerFrame frame,
                    std::string,
                    std::uint16_t) {
            std::lock_guard lock(received.mutex);
            received.frames.push_back(std::move(frame));
        });
    receiver.start();

    struct ReceiverStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& receiver;

        ~ReceiverStopper()
        {
            receiver.stop();
        }
    } receiverStopper{receiver};

    const std::string messageId = "outgoing-folder-message";
    const std::string partId = "outgoing-folder-part";
    const std::string transferId = "outgoing-folder-transfer";

    {
        TestableRelayDeskRuntime runtime(makeTransferRuntimeOptions());
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(
            makeTransferPeerProfile(receiver.GetLocalPort()),
            true);
        runtime.loadSelectedTransferRecord(
            makeOutgoingFolderTransferRecord(appPaths,
                                             runtime.GetLocalUser(),
                                             sourceFolder,
                                             messageId,
                                             partId,
                                             transferId,
                                             4));

        runtime.sendSelectedPeerFileTransfer(messageId, partId);
        if (!waitForCondition(runtime, [&runtime, &received] {
                {
                    std::lock_guard lock(received.mutex);
                    if (received.frames.size() < 3) {
                        return false;
                    }
                }
                const auto& messages = runtime.GetSelectedPeerMessages();
                if (messages.empty() || messages.front().GetParts().empty()) {
                    return false;
                }
                const auto& part = messages.front().GetParts().front();
                return part.GetTransferState().has_value()
                    && part.GetTransferState().value()
                        == relaydesk::storage::TransferState::Completed;
            })) {
            return fail("Outgoing folder transfer did not complete.");
        }

        std::uintmax_t transferSize = 0;
        {
            std::lock_guard lock(received.mutex);
            const relaydesk::net::TransferOfferMessage offer =
                relaydesk::net::parseTransferOfferFrame(received.frames[0]);
            if (const int result = expect(offer.GetFolderTransfer(),
                                          "Outgoing folder offer flag missing.");
                result != 0) {
                return result;
            }
            transferSize = offer.GetFileSize();

            std::optional<relaydesk::net::TransferCompleteMessage> complete;
            bool sawFolderChunkMetadata = false;
            for (const auto& frame : received.frames) {
                if (frame.GetType()
                    == relaydesk::net::PeerFrameType::TransferChunk) {
                    const relaydesk::net::TransferChunkMessage chunk =
                        relaydesk::net::parseTransferChunkFrame(frame);
                    sawFolderChunkMetadata =
                        sawFolderChunkMetadata
                        || chunk.GetFolderRelativePath().has_value();
                } else if (frame.GetType()
                           == relaydesk::net::PeerFrameType::TransferComplete) {
                    complete = relaydesk::net::parseTransferCompleteFrame(frame);
                }
            }
            if (const int result = expect(complete.has_value(),
                                          "Outgoing folder complete frame missing.");
                result != 0) {
                return result;
            }
            if (const int result =
                    expect(complete->GetFileSize() == transferSize,
                           "Outgoing folder complete size mismatch.");
                result != 0) {
                return result;
            }
            if (const int result =
                    expect(sawFolderChunkMetadata,
                           "Outgoing folder chunk metadata missing.");
                result != 0) {
                return result;
            }
        }

        if (const int result = expect(transferSize == 4,
                                      "Folder transfer size should use file bytes.");
            result != 0) {
            return result;
        }

        const auto& part = runtime.GetSelectedPeerMessages()
                               .front()
                               .GetParts()
                               .front();
        if (const int result =
                expect(part.GetFileSize().has_value()
                           && part.GetFileSize().value() == transferSize,
                       "Outgoing folder progress total did not use file bytes.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(part.GetTransferredSize().has_value()
                           && part.GetTransferredSize().value() == transferSize,
                       "Outgoing folder progress did not reach file bytes.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(part.GetLocalPath().has_value()
                           && part.GetLocalPath().value()
                               == makeWorkRelativePath(appPaths, sourceFolder),
                       "Outgoing folder local path should stay on source folder.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(std::filesystem::is_directory(sourceFolder),
                       "Outgoing folder source path disappeared after transfer.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int interruptsIncomingTransferWhenTcpStreamBreaksMidFrame()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    removeTestDataDirectory(appPaths);
    relaydesk::storage::ensureAppDirectories(appPaths);

    const std::uint16_t runtimeListenPort = reserveAvailableTcpPort();
    relaydesk::runtime::RelayDeskRuntimeOptions options =
        makeTransferRuntimeOptions();
    options.SetTcpListenPort(runtimeListenPort);

    const std::filesystem::path finalPath =
        appPaths.GetInboxDirectory() / "resume.bin";
    writeBytes(finalPath, {'A', 'B'});

    const std::string messageId = "network-break-message";
    const std::string partId = "network-break-part";
    const std::string transferId = "network-break-transfer";

    {
        TestableRelayDeskRuntime runtime(options);
        if (!runtime.GetStartupErrorMessage().empty()) {
            return fail("Runtime startup failed: "
                        + runtime.GetStartupErrorMessage());
        }

        runtime.receivePeerProfile(makeTransferPeerProfile(0), true);
        relaydesk::storage::ChatMessageRecord record =
            makeInterruptedIncomingTransferRecord(appPaths,
                                                  runtime.GetLocalUser(),
                                                  finalPath,
                                                  messageId,
                                                  partId,
                                                  transferId);
        std::vector<relaydesk::storage::ChatMessagePart> parts =
            record.GetParts();
        parts.front().SetTransferState(relaydesk::storage::TransferState::Offered);
        parts.front().SetTransferredSize(0);
        record.SetParts(std::move(parts));
        runtime.loadSelectedTransferRecord(std::move(record));

        boost::asio::io_context ioContext;
        boost::asio::ip::tcp::socket socket(ioContext);
        socket.connect(boost::asio::ip::tcp::endpoint(
            boost::asio::ip::make_address("127.0.0.1"),
            runtimeListenPort));
        writeRawPeerFrame(
            socket,
            relaydesk::net::makeTransferOfferFrame(
                makeTransferOffer(messageId, partId, transferId, false)));

        if (!waitForCondition(runtime, [&runtime] {
                const auto& messages = runtime.GetSelectedPeerMessages();
                if (messages.empty() || messages.front().GetParts().empty()) {
                    return false;
                }
                const auto state =
                    messages.front().GetParts().front().GetTransferState();
                return state.has_value()
                    && state.value()
                        == relaydesk::storage::TransferState::Transferring;
            })) {
            return fail("Incoming transfer did not enter transferring state.");
        }

        const relaydesk::net::PeerFrame chunkFrame =
            relaydesk::net::makeTransferChunkFrame(
                makeTransferChunk(messageId, partId, transferId, 0),
                {'A', 'B', 'C', 'D'});
        writePartialRawPeerFrameBody(socket, chunkFrame, 2);
        socket.close();

        if (!waitForCondition(runtime, [&runtime] {
                const auto& messages = runtime.GetSelectedPeerMessages();
                if (messages.empty() || messages.front().GetParts().empty()) {
                    return false;
                }
                const auto state =
                    messages.front().GetParts().front().GetTransferState();
                return state.has_value()
                    && state.value()
                        == relaydesk::storage::TransferState::Interrupted;
            })) {
            return fail("Incoming transfer was not interrupted after TCP break.");
        }

        const auto& messages = runtime.GetSelectedPeerMessages();
        const auto& part = messages.front().GetParts().front();
        if (const int result =
                expect(part.GetTransferredSize().has_value()
                           && part.GetTransferredSize().value() == 0,
                       "Interrupted incoming transfer progress should stay at "
                       "the last complete chunk.");
            result != 0) {
            return result;
        }
    }

    removeTestDataDirectory(appPaths);
    return 0;
}

int runProcessUpdateChild(std::uint16_t sourcePort,
                          std::uint16_t listenPort,
                          const std::filesystem::path& statusFile)
{
    relaydesk::runtime::RelayDeskRuntimeOptions options =
        makeTransferRuntimeOptions();
    options.SetTcpListenPort(listenPort);
    TestableRelayDeskRuntime runtime(options);
    if (!runtime.GetStartupErrorMessage().empty()) {
        return fail("Runtime startup failed: "
                    + runtime.GetStartupErrorMessage());
    }

    runtime.receivePeerProfile(
        makeUpdatePeerProfile(sourcePort, relaydesk::core::kAppVersion + 1),
        true);
    if (!waitForCondition(runtime, [&runtime] {
            return runtime.GetAppUpdatePrompt().has_value();
        })) {
        return fail("Process update child did not show an update prompt.");
    }

    const std::optional<relaydesk::runtime::AppUpdatePrompt> prompt =
        runtime.GetAppUpdatePrompt();
    if (const int result =
            expect(prompt.has_value(),
                   "Process update prompt disappeared before starting.");
        result != 0) {
        return result;
    }
    if (const int result =
            expect(prompt->GetSourceDeviceId() == "runtime-update-peer",
                   "Process update prompt source mismatch.");
        result != 0) {
        return result;
    }
    if (const int result =
            expect(prompt->GetAppVersion() == relaydesk::core::kAppVersion + 1,
                   "Process update prompt version mismatch.");
        result != 0) {
        return result;
    }

    runtime.startAppUpdate(
        relaydesk::runtime::AppUpdateInstallMode::InstallOnExit);
    if (!waitForCondition(runtime, [&runtime] {
            return runtime.getScheduledAppUpdate().has_value();
        })) {
        return fail("Process update child did not schedule the downloaded update.");
    }

    const std::optional<relaydesk::runtime::PendingIncomingAppUpdate>
        scheduledUpdate = runtime.getScheduledAppUpdate();
    if (const int result =
            expect(scheduledUpdate.has_value(),
                   "Scheduled update disappeared after download.");
        result != 0) {
        return result;
    }

    const std::filesystem::path executablePath = currentExecutablePath();
    const std::vector<std::uint8_t> expectedPayload =
        readBytes(executablePath);
    if (const int result =
            expect(scheduledUpdate->GetTempFilePath().filename()
                       == executablePath.filename(),
                   "Downloaded update temp file should keep executable name.");
        result != 0) {
        return result;
    }
    if (const int result =
            expect(std::filesystem::is_regular_file(
                       scheduledUpdate->GetTempFilePath()),
                   "Downloaded update temp file does not exist.");
        result != 0) {
        return result;
    }
    const std::vector<std::uint8_t> downloadedPayload =
        readBytes(scheduledUpdate->GetTempFilePath());
    if (const int result =
            expect(downloadedPayload == expectedPayload,
                   "Downloaded update payload did not match source executable.");
        result != 0) {
        return result;
    }
    if (const int result =
            expect(scheduledUpdate->GetExpectedSize()
                       == static_cast<std::uintmax_t>(expectedPayload.size()),
                   "Downloaded update expected size mismatch.");
        result != 0) {
        return result;
    }
    if (const int result =
            expect(scheduledUpdate->GetReceivedSize()
                       == static_cast<std::uintmax_t>(expectedPayload.size()),
                   "Downloaded update received size mismatch.");
        result != 0) {
        return result;
    }
    if (const int result =
            expect(scheduledUpdate->GetAppVersion()
                       == relaydesk::core::kAppVersion + 1,
                   "Downloaded update version mismatch.");
        result != 0) {
        return result;
    }
    if (const int result =
            expect(scheduledUpdate->GetFileName()
                       == executablePath.filename().string(),
                   "Downloaded update file name mismatch.");
        result != 0) {
        return result;
    }

    const relaydesk::runtime::AppUpdateApplyOptions updateOptions =
        runtime.prepareScheduledAppUpdateForTest();
    writeTextFile(statusFile,
                  "ok=1\nreceived_size="
                      + std::to_string(scheduledUpdate->GetReceivedSize())
                      + "\nrequest_id=" + scheduledUpdate->GetRequestId()
                      + "\nhelper_path="
                      + filesystemPathToGenericUtf8ForTest(
                          updateOptions.GetHelperPath())
                      + "\ntarget_path="
                      + filesystemPathToGenericUtf8ForTest(
                          updateOptions.GetTargetPath())
                      + "\npayload_path="
                      + filesystemPathToGenericUtf8ForTest(
                          updateOptions.GetPayloadPath())
                      + "\nlog_path="
                      + filesystemPathToGenericUtf8ForTest(
                          updateOptions.GetLogPath())
                      + "\nstart_directory="
                      + filesystemPathToGenericUtf8ForTest(
                          updateOptions.GetStartDirectory())
                      + "\npid="
                      + std::to_string(updateOptions.GetTargetProcessId())
                      + "\nrestart="
                      + std::to_string(
                          updateOptions.GetRestartAfterApply() ? 1 : 0)
                      + "\n");
    return 0;
}

int runProcessResumeChild(std::uint16_t receiverPort,
                          std::uint16_t listenPort,
                          const std::filesystem::path& statusFile)
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    const std::filesystem::path sourcePath =
        appPaths.GetWorkDirectory() / "resume-source.bin";
    if (!std::filesystem::is_regular_file(sourcePath)) {
        return fail("Resume child source file was not prepared.");
    }

    relaydesk::runtime::RelayDeskRuntimeOptions options =
        makeTransferRuntimeOptions();
    options.SetTcpListenPort(listenPort);
    TestableRelayDeskRuntime runtime(options);
    if (!runtime.GetStartupErrorMessage().empty()) {
        return fail("Runtime startup failed: "
                    + runtime.GetStartupErrorMessage());
    }

    runtime.receivePeerProfile(
        makeTransferPeerProfile(receiverPort),
        true);
    if (!waitForCondition(runtime, [&runtime] {
            return !runtime.GetPeers().empty();
        })) {
        return fail("Resume child did not receive peer profile.");
    }

    runtime.selectPeer("runtime-transfer-peer");
    if (!waitForCondition(runtime, [&runtime] {
            return runtime.GetSelectedPeer().has_value()
                && !runtime.GetSelectedPeerMessages().empty();
        })) {
        return fail("Resume child did not load persisted transfer history.");
    }

    const std::string messageId = "process-resume-message";
    const std::string partId = "process-resume-part";
    runtime.sendSelectedPeerFileTransfer(messageId, partId);
    if (!waitForCondition(runtime, [&runtime, &messageId, &partId] {
            for (const auto& message : runtime.GetSelectedPeerMessages()) {
                if (message.GetMessageId() != messageId) {
                    continue;
                }
                for (const auto& part : message.GetParts()) {
                    if (part.GetPartId() != partId) {
                        continue;
                    }
                    return part.GetTransferState().has_value()
                        && part.GetTransferState().value()
                            == relaydesk::storage::TransferState::Completed
                        && part.GetTransferredSize().has_value()
                        && part.GetTransferredSize().value() == 4;
                }
            }
            return false;
        })) {
        return fail("Resume child did not complete the resumed transfer.");
    }

    writeTextFile(statusFile, "ok=1\ncompleted=1\n");
    return 0;
}

int runProcessFolderResumeChild(std::uint16_t receiverPort,
                                std::uint16_t listenPort,
                                const std::filesystem::path& statusFile)
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    const std::filesystem::path sourceFolder =
        appPaths.GetWorkDirectory() / "Project";
    if (!std::filesystem::is_regular_file(sourceFolder / "a-root.txt")
        || !std::filesystem::is_regular_file(sourceFolder / "nested" / "child.txt")) {
        return fail("Folder resume child source folder was not prepared.");
    }

    relaydesk::runtime::RelayDeskRuntimeOptions options =
        makeTransferRuntimeOptions();
    options.SetTcpListenPort(listenPort);
    TestableRelayDeskRuntime runtime(options);
    if (!runtime.GetStartupErrorMessage().empty()) {
        return fail("Runtime startup failed: "
                    + runtime.GetStartupErrorMessage());
    }

    runtime.receivePeerProfile(
        makeTransferPeerProfile(receiverPort),
        true);
    if (!waitForCondition(runtime, [&runtime] {
            return !runtime.GetPeers().empty();
        })) {
        return fail("Folder resume child did not receive peer profile.");
    }

    runtime.selectPeer("runtime-transfer-peer");
    if (!waitForCondition(runtime, [&runtime] {
            return runtime.GetSelectedPeer().has_value()
                && !runtime.GetSelectedPeerMessages().empty();
        })) {
        return fail("Folder resume child did not load persisted transfer history.");
    }

    const std::string messageId = "process-folder-resume-message";
    const std::string partId = "process-folder-resume-part";
    runtime.sendSelectedPeerFileTransfer(messageId, partId);
    if (!waitForCondition(runtime, [&runtime, &messageId, &partId] {
            for (const auto& message : runtime.GetSelectedPeerMessages()) {
                if (message.GetMessageId() != messageId) {
                    continue;
                }
                for (const auto& part : message.GetParts()) {
                    if (part.GetPartId() != partId) {
                        continue;
                    }
                    return part.GetTransferState().has_value()
                        && part.GetTransferState().value()
                            == relaydesk::storage::TransferState::Completed
                        && part.GetTransferredSize().has_value()
                        && part.GetTransferredSize().value() == 4;
                }
            }
            return false;
        })) {
        return fail("Folder resume child did not complete resumed transfer.");
    }

    writeTextFile(statusFile, "ok=1\ncompleted=1\n");
    return 0;
}

int runProcessTransferReceiverChild(
    std::uint16_t senderPort,
    std::uint16_t listenPort,
    const std::filesystem::path& statusFile,
    const std::filesystem::path& acceptGateFile,
    const std::string& partId)
{
    relaydesk::runtime::RelayDeskRuntimeOptions options =
        makeTransferRuntimeOptions();
    options.SetTcpListenPort(listenPort);
    TestableRelayDeskRuntime runtime(options);
    runtime.setLocalUserForProcessTest("process-e2e-receiver-device",
                                       "PROCESS-E2E-RECEIVER-HOST",
                                       "Process Receiver");
    if (!runtime.GetStartupErrorMessage().empty()) {
        return fail("Transfer receiver child runtime startup failed: "
                    + runtime.GetStartupErrorMessage());
    }

    runtime.receivePeerProfile(
        makeTransferPeerProfileForDevice("process-e2e-sender-device",
                                         "Process Sender",
                                         senderPort),
        true);
    if (!waitForCondition(runtime, [&runtime] {
            return !runtime.GetPeers().empty();
        })) {
        return fail("Transfer receiver child did not receive peer profile.");
    }

    runtime.selectPeer("process-e2e-sender-device");
    if (!waitForCondition(runtime, [&runtime] {
            return runtime.GetSelectedPeer().has_value();
        })) {
        return fail("Transfer receiver child did not select sender.");
    }

    writeTextFile(statusFile, "ready=1\n");

    std::string messageId;
    if (!waitForCondition(runtime, [&runtime, &messageId, &partId] {
            for (const auto& message : runtime.GetSelectedPeerMessages()) {
                for (const auto& part : message.GetParts()) {
                    if (part.GetPartId() != partId
                        || !part.GetTransferState().has_value()) {
                        continue;
                    }
                    if (part.GetTransferState().value()
                        == relaydesk::storage::TransferState::Offered) {
                        messageId = message.GetMessageId();
                        return true;
                    }
                }
            }
            return false;
        })) {
        return fail("Transfer receiver child did not receive offer.");
    }

    if (!waitForPredicate(
            [&acceptGateFile] {
                return std::filesystem::exists(acceptGateFile);
            },
            std::chrono::seconds(10))) {
        return fail("Transfer receiver child accept gate timed out.");
    }

    runtime.acceptSelectedPeerFileTransfer(messageId, partId, false);

    std::string localPathText;
    std::uintmax_t transferredSize = 0;
    std::uintmax_t fileSize = 0;
    if (!waitForRuntimeCondition(
            runtime,
            [&runtime,
             &fileSize,
             &localPathText,
             &messageId,
             &partId,
             &transferredSize] {
                for (const auto& message : runtime.GetSelectedPeerMessages()) {
                    if (message.GetMessageId() != messageId) {
                        continue;
                    }
                    for (const auto& part : message.GetParts()) {
                        if (part.GetPartId() != partId
                            || !part.GetTransferState().has_value()) {
                            continue;
                        }
                        if (part.GetTransferState().value()
                            != relaydesk::storage::TransferState::Completed) {
                            return false;
                        }
                        if (!part.GetLocalPath().has_value()
                            || !part.GetTransferredSize().has_value()
                            || !part.GetFileSize().has_value()) {
                            return false;
                        }
                        localPathText = part.GetLocalPath().value();
                        transferredSize = part.GetTransferredSize().value();
                        fileSize = part.GetFileSize().value();
                        return true;
                    }
                }
                return false;
            },
            std::chrono::seconds(30))) {
        return fail("Transfer receiver child did not complete transfer.");
    }

    writeTextFile(statusFile,
                  "ready=1\nok=1\ncompleted=1\nmessage_id=" + messageId
                      + "\nlocal_path=" + localPathText
                      + "\ntransferred_size="
                      + std::to_string(transferredSize)
                      + "\nfile_size=" + std::to_string(fileSize) + "\n");
    return 0;
}

int runProcessTransferResumeReceiverChild(
    std::uint16_t senderPort,
    std::uint16_t listenPort,
    const std::filesystem::path& statusFile,
    const std::string& partId)
{
    relaydesk::runtime::RelayDeskRuntimeOptions options =
        makeTransferRuntimeOptions();
    options.SetTcpListenPort(listenPort);
    TestableRelayDeskRuntime runtime(options);
    runtime.setLocalUserForProcessTest("process-e2e-receiver-device",
                                       "PROCESS-E2E-RECEIVER-HOST",
                                       "Process Receiver");
    if (!runtime.GetStartupErrorMessage().empty()) {
        return fail("Resume receiver child runtime startup failed: "
                    + runtime.GetStartupErrorMessage());
    }

    runtime.receivePeerProfile(
        makeTransferPeerProfileForDevice("process-e2e-sender-device",
                                         "Process Sender",
                                         senderPort),
        true);
    if (!waitForCondition(runtime, [&runtime] {
            return !runtime.GetPeers().empty();
        })) {
        return fail("Resume receiver child did not receive sender profile.");
    }

    runtime.selectPeer("process-e2e-sender-device");
    if (!waitForCondition(runtime, [&runtime, &partId] {
            if (!runtime.GetSelectedPeer().has_value()) {
                return false;
            }
            for (const auto& message : runtime.GetSelectedPeerMessages()) {
                for (const auto& part : message.GetParts()) {
                    if (part.GetPartId() == partId
                        && part.GetTransferState().has_value()
                        && part.GetTransferState().value()
                            == relaydesk::storage::TransferState::Interrupted) {
                        return true;
                    }
                }
            }
            return false;
        })) {
        return fail("Resume receiver child did not load interrupted history.");
    }

    writeTextFile(statusFile, "ready=1\n");

    std::string messageId;
    std::string localPathText;
    std::uintmax_t transferredSize = 0;
    std::uintmax_t fileSize = 0;
    if (!waitForRuntimeCondition(
            runtime,
            [&runtime,
             &fileSize,
             &localPathText,
             &messageId,
             &partId,
             &transferredSize] {
                for (const auto& message : runtime.GetSelectedPeerMessages()) {
                    for (const auto& part : message.GetParts()) {
                        if (part.GetPartId() != partId
                            || !part.GetTransferState().has_value()) {
                            continue;
                        }
                        if (part.GetTransferState().value()
                            != relaydesk::storage::TransferState::Completed) {
                            return false;
                        }
                        if (!part.GetLocalPath().has_value()
                            || !part.GetTransferredSize().has_value()
                            || !part.GetFileSize().has_value()) {
                            return false;
                        }
                        messageId = message.GetMessageId();
                        localPathText = part.GetLocalPath().value();
                        transferredSize = part.GetTransferredSize().value();
                        fileSize = part.GetFileSize().value();
                        return true;
                    }
                }
                return false;
            },
            std::chrono::seconds(30))) {
        const relaydesk::storage::AppPaths appPaths =
            relaydesk::storage::createAppPaths();
        writeTextFile(statusFile,
                      "ready=1\n"
                          + describeSelectedTransferPartForTest(
                              appPaths,
                              runtime.GetSelectedPeerMessages(),
                              partId)
                          + "\n");
        return fail("Resume receiver child did not complete resumed transfer.");
    }

    writeTextFile(statusFile,
                  "ready=1\nok=1\ncompleted=1\nmessage_id=" + messageId
                      + "\nlocal_path=" + localPathText
                      + "\ntransferred_size="
                      + std::to_string(transferredSize)
                      + "\nfile_size=" + std::to_string(fileSize) + "\n");
    return 0;
}

int runsAppUpdateAcrossProcesses()
{
    const std::filesystem::path scenarioRoot =
        makeUniqueProcessScenarioRoot(
            utf8ToWideForTest(
                "\xE5\xBA\x94\xE7\x94\xA8\xE6\x9B\xB4\xE6\x96\xB0"
                "\xE5\x9C\xBA\xE6\x99\xAF"));
    const std::filesystem::path childInstallRoot =
        scenarioRoot / utf8ToWideForTest(
            "\xE4\xB8\xAD\xE6\x96\x87\xE6\x9B\xB4\xE6\x96\xB0"
            "\xE8\xB7\xAF\xE5\xBE\x84");
    const std::filesystem::path childExecutablePath =
        copyExecutableToScenarioRoot(childInstallRoot);
    const std::filesystem::path statusFile =
        scenarioRoot
        / utf8ToWideForTest(
            "\xE5\xAD\x90\xE8\xBF\x9B\xE7\xA8\x8B\xE7\x8A\xB6\xE6\x80\x81.txt");
    const std::vector<std::uint8_t> expectedPayload =
        readBytes(currentExecutablePath());
    const std::uint16_t childListenPort = reserveAvailableTcpPort();

    struct AppUpdateSourceState {
        std::mutex mutex;
        bool requestSeen = false;
        bool responseSent = false;
        std::string errorMessage;
    };

    AppUpdateSourceState state;
    relaydesk::net::BoostAsioTcpPeerTransport source(0);
    source.SetFrameCallback(
        [&childExecutablePath,
         childListenPort,
         &state,
         &source,
         &expectedPayload](
            relaydesk::net::PeerFrame frame,
            std::string address,
            std::uint16_t) {
            if (frame.GetType()
                != relaydesk::net::PeerFrameType::AppUpdateRequest) {
                return;
            }

            relaydesk::net::AppUpdateRequestMessage request;
            try {
                request = relaydesk::net::parseAppUpdateRequestFrame(frame);
            } catch (const std::exception& error) {
                std::lock_guard lock(state.mutex);
                state.errorMessage = error.what();
                return;
            }

            {
                std::lock_guard lock(state.mutex);
                state.requestSeen = true;
                if (request.GetCurrentAppVersion()
                    != relaydesk::core::kAppVersion) {
                    state.errorMessage =
                        "App update request current version mismatch.";
                } else if (request.GetRequestedAppVersion()
                           != relaydesk::core::kAppVersion + 1) {
                    state.errorMessage =
                        "App update request target version mismatch.";
                } else if (request.GetRequesterDeviceId().empty()) {
                    state.errorMessage =
                        "App update requester device id was empty.";
                }
                state.responseSent = true;
            }

            try {
                sendProcessAppUpdatePayloadFrames(
                    source,
                    address,
                    childListenPort,
                    request,
                    childExecutablePath.filename().string(),
                    expectedPayload);
            } catch (const std::exception& error) {
                std::lock_guard lock(state.mutex);
                state.errorMessage = error.what();
            }
        });
    source.start();

    struct SourceStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& source;

        ~SourceStopper()
        {
            source.stop();
        }
    } sourceStopper{source};

    ChildProcessHandle childProcess = startChildProcess(
        childExecutablePath,
        {L"--process-update-child",
         std::to_wstring(source.GetLocalPort()),
         std::to_wstring(childListenPort),
         statusFile.wstring()});
    const DWORD exitCode =
        waitForChildProcess(childProcess, std::chrono::seconds(30));
    const std::string status = readTextFileIfExists(statusFile);

    if (exitCode != 0) {
        std::string sourceStatus;
        {
            std::lock_guard lock(state.mutex);
            sourceStatus = " request_seen="
                + std::to_string(state.requestSeen ? 1 : 0)
                + " response_sent="
                + std::to_string(state.responseSent ? 1 : 0)
                + " source_error=" + state.errorMessage;
        }
        return fail("App update child process failed with exit code "
                    + std::to_string(exitCode) + ". Scenario: "
                    + scenarioRoot.string() + "." + sourceStatus
                    + ". Status: " + status);
    }
    if (!waitForPredicate(
            [&state] {
                std::lock_guard lock(state.mutex);
                return state.requestSeen;
            },
            std::chrono::seconds(5))) {
        removeScenarioRoot(scenarioRoot);
        return fail("App update request was never observed.");
    }

    {
        std::lock_guard lock(state.mutex);
        if (const int result = expect(state.errorMessage.empty(),
                                      "App update request validation failed: "
                                      + state.errorMessage);
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
        if (const int result = expect(state.responseSent,
                                      "App update response was not sent.");
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
    }

    if (const int result = expect(status.find("ok=1") != std::string::npos,
                                  "App update child status file mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    const std::string requestId = readStatusValue(status, "request_id");
    if (const int result = expect(!requestId.empty(),
                                  "App update child did not report request id.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    const std::string helperPathText = readStatusValue(status, "helper_path");
    if (const int result = expect(!helperPathText.empty(),
                                  "App update child did not report helper path.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    const std::string targetPathText = readStatusValue(status, "target_path");
    const std::string payloadPathText = readStatusValue(status, "payload_path");
    const std::string logPathText = readStatusValue(status, "log_path");
    const std::string startDirectoryText =
        readStatusValue(status, "start_directory");
    const std::string pidText = readStatusValue(status, "pid");
    if (const int result =
            expect(!targetPathText.empty() && !payloadPathText.empty()
                       && !logPathText.empty()
                       && !startDirectoryText.empty() && !pidText.empty(),
                   "App update child did not report helper arguments.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    relaydesk::runtime::AppUpdateApplyOptions updateOptions;
    updateOptions.SetHelperPath(
        filesystemPathFromGenericUtf8ForTest(helperPathText));
    updateOptions.SetTargetPath(
        filesystemPathFromGenericUtf8ForTest(targetPathText));
    updateOptions.SetPayloadPath(
        filesystemPathFromGenericUtf8ForTest(payloadPathText));
    updateOptions.SetLogPath(filesystemPathFromGenericUtf8ForTest(logPathText));
    updateOptions.SetStartDirectory(
        filesystemPathFromGenericUtf8ForTest(startDirectoryText));
    updateOptions.SetTargetProcessId(static_cast<unsigned long>(
        std::stoul(pidText)));
    updateOptions.SetRestartAfterApply(readStatusValue(status, "restart") == "1");

    const std::filesystem::path helperPath = updateOptions.GetHelperPath();
    const std::filesystem::path helperLogPath = updateOptions.GetLogPath();
    writeBytes(childExecutablePath, {'s', 't', 'a', 'l', 'e'});
    const DWORD helperExitCode = runCommandAndWait(
        helperPath,
        makeAppUpdateApplyArgumentsForTest(updateOptions),
        updateOptions.GetStartDirectory(),
        std::chrono::seconds(30));
    if (!waitForPredicate(
            [&helperLogPath] {
                const std::string helperLog =
                    readTextFileIfExists(helperLogPath);
                return helperLog.find("update helper finished")
                    != std::string::npos;
            },
            std::chrono::seconds(5))) {
        const std::string helperLog = readTextFileIfExists(helperLogPath);
        removeScenarioRoot(scenarioRoot);
        return fail("App update helper did not finish. Scenario: "
                    + scenarioRoot.string() + ". Status: " + status
                    + ". Exit code: " + std::to_string(helperExitCode)
                    + ". Log path: " + helperLogPath.string()
                    + ". Log: " + helperLog);
    }
    if (const int result = expect(helperExitCode == 0,
                                  "App update helper returned a failure code.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    const std::string helperLog = readTextFileIfExists(helperLogPath);
    if (const int result =
            expect(helperLog.find("copy succeeded") != std::string::npos,
                   "App update helper did not copy payload.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readBytes(childExecutablePath) == expectedPayload,
                   "App update helper did not replace child executable.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    removeScenarioRoot(scenarioRoot);
    return 0;
}

int skiaUiUpdateHelperRestartsUpdatedAppVisible()
{
#if defined(RELAYDESK_SKIAUI_EXECUTABLE_PATH)
    const std::filesystem::path scenarioRoot =
        makeUniqueProcessScenarioRoot("skiaui-update-restart");
    const std::filesystem::path targetPath =
        scenarioRoot / "relaydesk-restarted.exe";
    const std::filesystem::path markerPath =
        scenarioRoot / "restart-probe.txt";
    const std::filesystem::path logPath = scenarioRoot / "apply-update.log";
    const std::filesystem::path payloadPath = currentExecutablePath();
    const std::filesystem::path helperPath =
        std::filesystem::path(RELAYDESK_SKIAUI_EXECUTABLE_PATH);
    const std::filesystem::path dataSentinelPath =
        scenarioRoot / "data" / "peers" / "messages.jsonl";
    const std::vector<std::uint8_t> dataSentinel{
        'c', 'h', 'a', 't', '-', 'h', 'i', 's', 't', 'o', 'r', 'y'};
    std::filesystem::create_directories(scenarioRoot);
    writeBytes(targetPath, {'s', 't', 'a', 'l', 'e'});
    writeBytes(dataSentinelPath, dataSentinel);

    relaydesk::runtime::AppUpdateApplyOptions options;
    options.SetTargetPath(targetPath);
    options.SetPayloadPath(payloadPath);
    options.SetHelperPath(helperPath);
    options.SetStartDirectory(scenarioRoot);
    options.SetLogPath(logPath);
    options.SetTargetProcessId(0);
    options.SetRestartAfterApply(true);

    DWORD helperExitCode = 1;
    {
        const ScopedEnvironmentVariable restartProbe(
            kAppUpdateRestartProbeEnvironment, markerPath.wstring());
        helperExitCode = runCommandAndWait(
            helperPath,
            makeAppUpdateApplyArgumentsForTest(options),
            scenarioRoot,
            std::chrono::seconds(5));
        (void)waitForPredicate(
            [&markerPath] {
                return std::filesystem::is_regular_file(markerPath);
            },
            std::chrono::seconds(5));
    }

    const std::string helperLog = readTextFileIfExists(logPath);
    const std::string marker = readTextFileIfExists(markerPath);
    if (const int result = expect(
            helperExitCode == 0,
            "SkiaUI update helper did not exit successfully. Log: "
                + helperLog);
        result != 0) {
        return result;
    }
    if (const int result = expect(
            readBytes(targetPath) == readBytes(payloadPath),
            "SkiaUI update helper did not replace the target executable.");
        result != 0) {
        return result;
    }
    if (const int result = expect(
            readBytes(dataSentinelPath) == dataSentinel,
            "SkiaUI update helper modified the portable data directory.");
        result != 0) {
        return result;
    }
    if (const int result = expect(
            helperLog.find("copy succeeded") != std::string::npos
                && helperLog.find("restart requested") != std::string::npos
                && helperLog.find("update helper finished")
                    != std::string::npos,
            "SkiaUI update helper log did not confirm replacement and "
            "restart. Log: " + helperLog);
        result != 0) {
        return result;
    }
    if (const int result = expect(
            marker.find("started=1") != std::string::npos,
            "Updated application was not restarted. Marker: " + marker);
        result != 0) {
        return result;
    }
    if (const int result = expect(
            readStatusValue(marker, "show_window")
                == std::to_string(SW_SHOWNORMAL),
            "Updated application was restarted with a hidden window. Marker: "
                + marker);
        result != 0) {
        return result;
    }

    if (!waitForPredicate(
            [&targetPath] {
                std::error_code error;
                (void)std::filesystem::remove(targetPath, error);
                return !std::filesystem::exists(targetPath);
            },
            std::chrono::seconds(5))) {
        return fail("Restarted update probe process did not exit.");
    }
    removeScenarioRoot(scenarioRoot);
#endif
    return 0;
}

int rejectsAppUpdateTargetInsideDataDirectory()
{
    const std::filesystem::path scenarioRoot =
        makeUniqueProcessScenarioRoot("update-data-guard");
    const std::filesystem::path targetPath =
        scenarioRoot / "data" / "peers" / "messages.jsonl";
    const std::filesystem::path payloadPath = scenarioRoot / "payload.exe";
    const std::filesystem::path logPath = scenarioRoot / "apply-update.log";
    const std::vector<std::uint8_t> originalData{
        'u', 's', 'e', 'r', '-', 'h', 'i', 's', 't', 'o', 'r', 'y'};
    writeBytes(targetPath, originalData);
    writeBytes(payloadPath, {'u', 'p', 'd', 'a', 't', 'e'});

    relaydesk::runtime::AppUpdateApplyOptions options;
    options.SetTargetPath(targetPath);
    options.SetPayloadPath(payloadPath);
    options.SetStartDirectory(scenarioRoot);
    options.SetLogPath(logPath);
    options.SetTargetProcessId(0);
    options.SetRestartAfterApply(false);

    const int applyResult = relaydesk::runtime::runAppUpdateApplyMode(options);
    if (const int result = expect(
            applyResult != 0,
            "App update helper accepted a target inside the data directory.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result = expect(
            readBytes(targetPath) == originalData,
            "Rejected app update modified the data directory target.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    removeScenarioRoot(scenarioRoot);
    return 0;
}

int sendsFileTransferBetweenTwoRuntimeProcesses()
{
    const std::filesystem::path scenarioRoot =
        makeUniqueProcessScenarioRoot("file-transfer-e2e");
    const std::filesystem::path childExecutablePath =
        copyExecutableToScenarioRoot(scenarioRoot);
    const std::uint16_t senderListenPort = reserveAvailableTcpPort();
    const std::uint16_t receiverListenPort = reserveAvailableTcpPort();
    const relaydesk::storage::AppPaths senderAppPaths(currentExecutablePath());
    const relaydesk::storage::AppPaths receiverAppPaths(childExecutablePath);
    removeTestDataDirectory(senderAppPaths);
    relaydesk::storage::ensureAppDirectories(senderAppPaths);
    relaydesk::storage::ensureAppDirectories(receiverAppPaths);

    const std::filesystem::path sourcePath =
        senderAppPaths.GetWorkDirectory() / "process-e2e-source.bin";
    const std::vector<std::uint8_t> expectedPayload{
        'R', 'e', 'l', 'a', 'y', 'D', 'e', 's', 'k',
        '-', 't', 'r', 'a', 'n', 's', 'f', 'e', 'r'};
    writeBytes(sourcePath, expectedPayload);

    const std::filesystem::path statusFile =
        scenarioRoot / "receiver-status.txt";
    const std::filesystem::path acceptGateFile =
        scenarioRoot / "receiver-accept-gate.txt";
    const std::string partId = "process-e2e-file-part";
    ChildProcessHandle receiverProcess = startChildProcess(
        childExecutablePath,
        {L"--process-transfer-receiver-child",
         std::to_wstring(senderListenPort),
         std::to_wstring(receiverListenPort),
         statusFile.wstring(),
         acceptGateFile.wstring(),
         utf8ToWideForTest(partId)});
    ChildProcessTerminator receiverProcessTerminator{receiverProcess};

    if (!waitForPredicate(
            [&statusFile] {
                return readTextFileIfExists(statusFile).find("ready=1")
                    != std::string::npos;
            },
            std::chrono::seconds(10))) {
        removeScenarioRoot(scenarioRoot);
        return fail("File transfer receiver process did not become ready.");
    }

    std::string messageId;
    {
        relaydesk::runtime::RelayDeskRuntimeOptions options =
            makeTransferRuntimeOptions();
        options.SetTcpListenPort(senderListenPort);
        TestableRelayDeskRuntime senderRuntime(options);
        senderRuntime.setLocalUserForProcessTest("process-e2e-sender-device",
                                                 "PROCESS-E2E-SENDER-HOST",
                                                 "Process Sender");
        if (!senderRuntime.GetStartupErrorMessage().empty()) {
            removeScenarioRoot(scenarioRoot);
            return fail("File transfer sender runtime startup failed: "
                        + senderRuntime.GetStartupErrorMessage());
        }

        senderRuntime.receivePeerProfile(
            makeTransferPeerProfileForDevice("process-e2e-receiver-device",
                                             "Process Receiver",
                                             receiverListenPort),
            true);
        if (!waitForCondition(senderRuntime, [&senderRuntime] {
                return !senderRuntime.GetPeers().empty();
            })) {
            removeScenarioRoot(scenarioRoot);
            return fail("File transfer sender did not receive receiver profile.");
        }

        senderRuntime.selectPeer("process-e2e-receiver-device");
        if (!waitForCondition(senderRuntime, [&senderRuntime] {
                return senderRuntime.GetSelectedPeer().has_value();
            })) {
            removeScenarioRoot(scenarioRoot);
            return fail("File transfer sender did not select receiver.");
        }

        std::vector<relaydesk::storage::ChatMessagePart> parts;
        parts.push_back(makeProcessFileTransferPart(senderAppPaths,
                                                    sourcePath,
                                                    partId,
                                                    "process-e2e-file-transfer"));
        senderRuntime.sendMessagePartsToSelectedPeer(
            std::move(parts), std::nullopt);
        if (!waitForCondition(senderRuntime, [&senderRuntime, &messageId, &partId] {
                for (const auto& message : senderRuntime.GetSelectedPeerMessages()) {
                    for (const auto& part : message.GetParts()) {
                        if (part.GetPartId() != partId
                            || !part.GetTransferState().has_value()) {
                            continue;
                        }
                        if (part.GetTransferState().value()
                            == relaydesk::storage::TransferState::Offered) {
                            messageId = message.GetMessageId();
                            return true;
                        }
                    }
                }
                return false;
            })) {
            removeScenarioRoot(scenarioRoot);
            return fail("File transfer sender did not create an offered transfer.");
        }

        writeTextFile(acceptGateFile, "accept=1\n");

        if (!waitForRuntimeCondition(
                senderRuntime,
                [&senderRuntime, &messageId, &partId, &expectedPayload] {
                    for (const auto& message : senderRuntime.GetSelectedPeerMessages()) {
                        if (message.GetMessageId() != messageId) {
                            continue;
                        }
                        for (const auto& part : message.GetParts()) {
                            if (part.GetPartId() != partId
                                || !part.GetTransferState().has_value()) {
                                continue;
                            }
                            return part.GetTransferState().value()
                                == relaydesk::storage::TransferState::Completed
                                && part.GetTransferredSize().has_value()
                                && part.GetTransferredSize().value()
                                    == expectedPayload.size();
                        }
                    }
                    return false;
                },
                std::chrono::seconds(30))) {
            removeScenarioRoot(scenarioRoot);
            return fail("File transfer sender did not complete transfer.");
        }
    }

    const DWORD exitCode =
        waitForChildProcess(receiverProcess, std::chrono::seconds(30));
    const std::string status = readTextFileIfExists(statusFile);
    if (exitCode != 0) {
        removeScenarioRoot(scenarioRoot);
        return fail("File transfer receiver child failed with exit code "
                    + std::to_string(exitCode) + ". Status: " + status);
    }
    if (const int result = expect(status.find("ok=1") != std::string::npos,
                                  "File transfer receiver status mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readStatusValue(status, "message_id") == messageId,
                   "File transfer receiver message id mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readStatusValue(status, "transferred_size")
                       == std::to_string(expectedPayload.size()),
                   "File transfer receiver transferred size mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    const std::filesystem::path receivedPath =
        resolveWorkRelativePathForTest(receiverAppPaths,
                                       readStatusValue(status, "local_path"));
    if (const int result =
            expect(std::filesystem::is_regular_file(receivedPath),
                   "File transfer receiver payload file does not exist.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readBytes(receivedPath) == expectedPayload,
                   "File transfer receiver payload bytes mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    std::filesystem::remove(sourcePath);
    removeTestDataDirectory(senderAppPaths);
    removeScenarioRoot(scenarioRoot);
    return 0;
}

int sendsFolderTransferBetweenTwoRuntimeProcesses()
{
    const std::filesystem::path scenarioRoot =
        makeUniqueProcessScenarioRoot("folder-transfer-e2e");
    const std::filesystem::path childExecutablePath =
        copyExecutableToScenarioRoot(scenarioRoot);
    const std::uint16_t senderListenPort = reserveAvailableTcpPort();
    const std::uint16_t receiverListenPort = reserveAvailableTcpPort();
    const relaydesk::storage::AppPaths senderAppPaths(currentExecutablePath());
    const relaydesk::storage::AppPaths receiverAppPaths(childExecutablePath);
    removeTestDataDirectory(senderAppPaths);
    relaydesk::storage::ensureAppDirectories(senderAppPaths);
    relaydesk::storage::ensureAppDirectories(receiverAppPaths);

    const std::filesystem::path sourceFolder =
        senderAppPaths.GetWorkDirectory() / "process-e2e-folder";
    writeBytes(sourceFolder / "a-root.txt", {'A', 'B'});
    writeBytes(sourceFolder / "nested" / "child.txt", {'C', 'D'});
    constexpr std::uintmax_t expectedSize = 4;

    const std::filesystem::path statusFile =
        scenarioRoot / "receiver-status.txt";
    const std::filesystem::path acceptGateFile =
        scenarioRoot / "receiver-accept-gate.txt";
    const std::string partId = "process-e2e-folder-part";
    ChildProcessHandle receiverProcess = startChildProcess(
        childExecutablePath,
        {L"--process-transfer-receiver-child",
         std::to_wstring(senderListenPort),
         std::to_wstring(receiverListenPort),
         statusFile.wstring(),
         acceptGateFile.wstring(),
         utf8ToWideForTest(partId)});
    ChildProcessTerminator receiverProcessTerminator{receiverProcess};

    if (!waitForPredicate(
            [&statusFile] {
                return readTextFileIfExists(statusFile).find("ready=1")
                    != std::string::npos;
            },
            std::chrono::seconds(10))) {
        removeScenarioRoot(scenarioRoot);
        return fail("Folder transfer receiver process did not become ready.");
    }

    std::string messageId;
    {
        relaydesk::runtime::RelayDeskRuntimeOptions options =
            makeTransferRuntimeOptions();
        options.SetTcpListenPort(senderListenPort);
        TestableRelayDeskRuntime senderRuntime(options);
        senderRuntime.setLocalUserForProcessTest("process-e2e-sender-device",
                                                 "PROCESS-E2E-SENDER-HOST",
                                                 "Process Sender");
        if (!senderRuntime.GetStartupErrorMessage().empty()) {
            removeScenarioRoot(scenarioRoot);
            return fail("Folder transfer sender runtime startup failed: "
                        + senderRuntime.GetStartupErrorMessage());
        }

        senderRuntime.receivePeerProfile(
            makeTransferPeerProfileForDevice("process-e2e-receiver-device",
                                             "Process Receiver",
                                             receiverListenPort),
            true);
        if (!waitForCondition(senderRuntime, [&senderRuntime] {
                return !senderRuntime.GetPeers().empty();
            })) {
            removeScenarioRoot(scenarioRoot);
            return fail("Folder transfer sender did not receive receiver profile.");
        }

        senderRuntime.selectPeer("process-e2e-receiver-device");
        if (!waitForCondition(senderRuntime, [&senderRuntime] {
                return senderRuntime.GetSelectedPeer().has_value();
            })) {
            removeScenarioRoot(scenarioRoot);
            return fail("Folder transfer sender did not select receiver.");
        }

        std::vector<relaydesk::storage::ChatMessagePart> parts;
        parts.push_back(makeProcessFolderTransferPart(
            senderAppPaths,
            sourceFolder,
            partId,
            "process-e2e-folder-transfer",
            expectedSize));
        senderRuntime.sendMessagePartsToSelectedPeer(
            std::move(parts), std::nullopt);
        if (!waitForCondition(senderRuntime, [&senderRuntime, &messageId, &partId] {
                for (const auto& message : senderRuntime.GetSelectedPeerMessages()) {
                    for (const auto& part : message.GetParts()) {
                        if (part.GetPartId() != partId
                            || !part.GetTransferState().has_value()) {
                            continue;
                        }
                        if (part.GetTransferState().value()
                            == relaydesk::storage::TransferState::Offered) {
                            messageId = message.GetMessageId();
                            return true;
                        }
                    }
                }
                return false;
            })) {
            removeScenarioRoot(scenarioRoot);
            return fail("Folder transfer sender did not create an offer.");
        }

        writeTextFile(acceptGateFile, "accept=1\n");

        if (!waitForRuntimeCondition(
                senderRuntime,
                [&senderRuntime, &messageId, &partId] {
                    for (const auto& message : senderRuntime.GetSelectedPeerMessages()) {
                        if (message.GetMessageId() != messageId) {
                            continue;
                        }
                        for (const auto& part : message.GetParts()) {
                            if (part.GetPartId() != partId
                                || !part.GetTransferState().has_value()) {
                                continue;
                            }
                            return part.GetTransferState().value()
                                == relaydesk::storage::TransferState::Completed
                                && part.GetTransferredSize().has_value()
                                && part.GetTransferredSize().value()
                                    == expectedSize;
                        }
                    }
                    return false;
                },
                std::chrono::seconds(30))) {
            removeScenarioRoot(scenarioRoot);
            return fail("Folder transfer sender did not complete transfer.");
        }
    }

    const DWORD exitCode =
        waitForChildProcess(receiverProcess, std::chrono::seconds(30));
    const std::string status = readTextFileIfExists(statusFile);
    if (exitCode != 0) {
        removeScenarioRoot(scenarioRoot);
        return fail("Folder transfer receiver child failed with exit code "
                    + std::to_string(exitCode) + ". Status: " + status);
    }
    if (const int result = expect(status.find("ok=1") != std::string::npos,
                                  "Folder transfer receiver status mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readStatusValue(status, "message_id") == messageId,
                   "Folder transfer receiver message id mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readStatusValue(status, "transferred_size")
                       == std::to_string(expectedSize),
                   "Folder transfer receiver transferred size mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    const std::filesystem::path receivedFolder =
        resolveWorkRelativePathForTest(receiverAppPaths,
                                       readStatusValue(status, "local_path"));
    if (const int result =
            expect(std::filesystem::is_directory(receivedFolder),
                   "Folder transfer receiver payload folder does not exist.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readBytes(receivedFolder / "a-root.txt")
                       == std::vector<std::uint8_t>({'A', 'B'}),
                   "Folder transfer root file bytes mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readBytes(receivedFolder / "nested" / "child.txt")
                       == std::vector<std::uint8_t>({'C', 'D'}),
                   "Folder transfer nested file bytes mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    removeTestDataDirectory(senderAppPaths);
    removeScenarioRoot(scenarioRoot);
    return 0;
}

int resumesInterruptedFileTransferBetweenTwoRuntimeProcesses()
{
    const std::filesystem::path scenarioRoot =
        makeUniqueProcessScenarioRoot("file-resume-e2e");
    const std::filesystem::path childExecutablePath =
        copyExecutableToScenarioRoot(scenarioRoot);
    const std::uint16_t senderListenPort = reserveAvailableTcpPort();
    const std::uint16_t receiverListenPort = reserveAvailableTcpPort();
    const relaydesk::storage::AppPaths senderAppPaths(currentExecutablePath());
    const relaydesk::storage::AppPaths receiverAppPaths(childExecutablePath);
    removeTestDataDirectory(senderAppPaths);
    relaydesk::storage::ensureAppDirectories(senderAppPaths);
    relaydesk::storage::ensureAppDirectories(receiverAppPaths);

    const std::string messageId = "process-e2e-resume-message";
    const std::string partId = "process-e2e-resume-part";
    const std::string transferId = "process-e2e-resume-transfer";
    const std::vector<std::uint8_t> expectedPayload{'A', 'B', 'C', 'D'};
    constexpr std::uintmax_t partialSize = 2;

    const relaydesk::runtime::LocalUserSummary senderUser =
        makeProcessLocalUser("process-e2e-sender-device",
                             "PROCESS-E2E-SENDER-HOST",
                             "Process Sender");
    const relaydesk::runtime::LocalUserSummary receiverUser =
        makeProcessLocalUser("process-e2e-receiver-device",
                             "PROCESS-E2E-RECEIVER-HOST",
                             "Process Receiver");
    const std::filesystem::path sourcePath =
        senderAppPaths.GetWorkDirectory() / "process-e2e-resume.bin";
    const std::filesystem::path receivedPath =
        receiverAppPaths.GetInboxDirectory() / "process-e2e-resume.bin";
    writeBytes(sourcePath, expectedPayload);
    writeBytes(receivedPath, {'A', 'B'});

    ProcessInterruptedTransferRecordSeed senderSeed;
    senderSeed.peerDeviceId = receiverUser.GetDeviceId();
    senderSeed.peerDisplayName = receiverUser.GetDisplayName();
    senderSeed.messageId = messageId;
    senderSeed.partId = partId;
    senderSeed.transferId = transferId;
    senderSeed.direction = relaydesk::storage::MessageDirection::Outgoing;
    senderSeed.fileSize = expectedPayload.size();
    senderSeed.transferredSize = partialSize;
    relaydesk::storage::appendChatMessage(
        senderAppPaths,
        receiverUser.GetDeviceId(),
        makeProcessInterruptedTransferRecord(senderAppPaths,
                                             senderUser,
                                             sourcePath,
                                             senderSeed));

    ProcessInterruptedTransferRecordSeed receiverSeed = senderSeed;
    receiverSeed.peerDeviceId = senderUser.GetDeviceId();
    receiverSeed.peerDisplayName = senderUser.GetDisplayName();
    receiverSeed.direction = relaydesk::storage::MessageDirection::Incoming;
    relaydesk::storage::appendChatMessage(
        receiverAppPaths,
        senderUser.GetDeviceId(),
        makeProcessInterruptedTransferRecord(receiverAppPaths,
                                             receiverUser,
                                             receivedPath,
                                             receiverSeed));

    const std::filesystem::path statusFile =
        scenarioRoot / "receiver-status.txt";
    ChildProcessHandle receiverProcess = startChildProcess(
        childExecutablePath,
        {L"--process-transfer-resume-receiver-child",
         std::to_wstring(senderListenPort),
         std::to_wstring(receiverListenPort),
         statusFile.wstring(),
         utf8ToWideForTest(partId)});
    ChildProcessTerminator receiverProcessTerminator{receiverProcess};

    if (!waitForPredicate(
            [&statusFile] {
                return readTextFileIfExists(statusFile).find("ready=1")
                    != std::string::npos;
            },
            std::chrono::seconds(10))) {
        removeScenarioRoot(scenarioRoot);
        return fail("Resume receiver process did not become ready.");
    }

    {
        relaydesk::runtime::RelayDeskRuntimeOptions options =
            makeTransferRuntimeOptions();
        options.SetTcpListenPort(senderListenPort);
        TestableRelayDeskRuntime senderRuntime(options);
        senderRuntime.setLocalUserForProcessTest(senderUser.GetDeviceId(),
                                                 senderUser.GetHostName(),
                                                 senderUser.GetDisplayName());
        if (!senderRuntime.GetStartupErrorMessage().empty()) {
            removeScenarioRoot(scenarioRoot);
            return fail("Resume sender runtime startup failed: "
                        + senderRuntime.GetStartupErrorMessage());
        }

        senderRuntime.receivePeerProfile(
            makeTransferPeerProfileForDevice(receiverUser.GetDeviceId(),
                                             receiverUser.GetDisplayName(),
                                             receiverListenPort),
            true);
        if (!waitForCondition(senderRuntime, [&senderRuntime] {
                return !senderRuntime.GetPeers().empty();
            })) {
            removeScenarioRoot(scenarioRoot);
            return fail("Resume sender did not receive receiver profile.");
        }

        senderRuntime.selectPeer(receiverUser.GetDeviceId());
        if (!waitForCondition(senderRuntime, [&senderRuntime, &partId] {
                if (!senderRuntime.GetSelectedPeer().has_value()) {
                    return false;
                }
                for (const auto& message : senderRuntime.GetSelectedPeerMessages()) {
                    for (const auto& part : message.GetParts()) {
                        if (part.GetPartId() == partId
                            && part.GetTransferState().has_value()
                            && part.GetTransferState().value()
                                == relaydesk::storage::TransferState::Interrupted) {
                            return true;
                        }
                    }
                }
                return false;
            })) {
            removeScenarioRoot(scenarioRoot);
            return fail("Resume sender did not load interrupted history.");
        }

        senderRuntime.sendSelectedPeerFileTransfer(messageId, partId);
        if (!waitForRuntimeCondition(
                senderRuntime,
                [&senderRuntime, &messageId, &partId, &expectedPayload] {
                    for (const auto& message : senderRuntime.GetSelectedPeerMessages()) {
                        if (message.GetMessageId() != messageId) {
                            continue;
                        }
                        for (const auto& part : message.GetParts()) {
                            if (part.GetPartId() != partId
                                || !part.GetTransferState().has_value()) {
                                continue;
                            }
                            return part.GetTransferState().value()
                                == relaydesk::storage::TransferState::Completed
                                && part.GetTransferredSize().has_value()
                                && part.GetTransferredSize().value()
                                    == expectedPayload.size();
                        }
                    }
                    return false;
                },
                std::chrono::seconds(30))) {
            removeScenarioRoot(scenarioRoot);
            return fail("Resume sender did not complete resumed transfer.");
        }
    }

    const DWORD exitCode =
        waitForChildProcess(receiverProcess, std::chrono::seconds(30));
    const std::string status = readTextFileIfExists(statusFile);
    if (exitCode != 0) {
        removeScenarioRoot(scenarioRoot);
        return fail("Resume receiver child failed with exit code "
                    + std::to_string(exitCode) + ". Status: " + status);
    }
    if (const int result = expect(status.find("ok=1") != std::string::npos,
                                  "Resume receiver status mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readStatusValue(status, "message_id") == messageId,
                   "Resume receiver message id mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readStatusValue(status, "transferred_size")
                       == std::to_string(expectedPayload.size()),
                   "Resume receiver transferred size mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    const std::filesystem::path completedPath =
        resolveWorkRelativePathForTest(receiverAppPaths,
                                       readStatusValue(status, "local_path"));
    if (const int result = expect(completedPath == receivedPath,
                                  "Resume receiver reused a different file path.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }
    if (const int result =
            expect(readBytes(receivedPath) == expectedPayload,
                   "Resume receiver payload bytes mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    removeTestDataDirectory(senderAppPaths);
    removeScenarioRoot(scenarioRoot);
    return 0;
}

int resumesInterruptedOutgoingTransferAcrossProcesses()
{
    const std::filesystem::path scenarioRoot =
        makeUniqueProcessScenarioRoot("transfer-resume");
    const std::filesystem::path childExecutablePath =
        copyExecutableToScenarioRoot(scenarioRoot);
    const std::uint16_t childListenPort = reserveAvailableTcpPort();
    const relaydesk::storage::AppPaths childAppPaths(childExecutablePath);
    relaydesk::storage::ensureAppDirectories(childAppPaths);

    const std::filesystem::path sourcePath =
        childAppPaths.GetWorkDirectory() / "resume-source.bin";
    writeBytes(sourcePath, {'A', 'B', 'C', 'D'});

    const std::string messageId = "process-resume-message";
    const std::string partId = "process-resume-part";
    const std::string transferId = "process-resume-transfer";
    relaydesk::runtime::LocalUserSummary localUser;
    localUser.SetDeviceId("process-sender-device");
    localUser.SetDisplayName("Process Sender");
    localUser.SetHostName("PROCESS-SENDER-HOST");
    relaydesk::storage::appendChatMessage(
        childAppPaths,
        "runtime-transfer-peer",
        makeOutgoingInterruptedTransferRecord(childAppPaths,
                                              localUser,
                                              sourcePath,
                                              messageId,
                                              partId,
                                              transferId));

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
        std::string errorMessage;
    };

    ReceivedTransferFrames received;
    relaydesk::net::BoostAsioTcpPeerTransport receiver(0);
    receiver.SetFrameCallback(
        [childListenPort,
         &messageId,
         &partId,
         &received,
         &receiver,
         &transferId](
            relaydesk::net::PeerFrame frame,
            std::string address,
            std::uint16_t) {
            std::optional<relaydesk::net::TransferOfferMessage> offer;
            {
                std::lock_guard lock(received.mutex);
                received.frames.push_back(frame);
                if (frame.GetType()
                    == relaydesk::net::PeerFrameType::TransferOffer) {
                    try {
                        offer = relaydesk::net::parseTransferOfferFrame(frame);
                    } catch (const std::exception& error) {
                        received.errorMessage = error.what();
                        return;
                    }
                }
            }

            if (offer.has_value() && offer->GetResumeRequest()) {
                receiver.sendFrameTo(
                    address,
                    childListenPort,
                    makeTransferAcceptFrame(messageId,
                                            partId,
                                            transferId,
                                            "runtime-transfer-peer",
                                            2));
            }
        });
    receiver.start();

    struct ReceiverStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& receiver;

        ~ReceiverStopper()
        {
            receiver.stop();
        }
    } receiverStopper{receiver};

    const std::filesystem::path statusFile = scenarioRoot / "child-status.txt";
    ChildProcessHandle childProcess = startChildProcess(
        childExecutablePath,
        {L"--process-resume-child",
         std::to_wstring(receiver.GetLocalPort()),
         std::to_wstring(childListenPort),
         statusFile.wstring()});
    const DWORD exitCode =
        waitForChildProcess(childProcess, std::chrono::seconds(30));

    if (!waitForPredicate(
            [&received] {
                std::lock_guard lock(received.mutex);
                for (const auto& frame : received.frames) {
                    if (frame.GetType()
                        == relaydesk::net::PeerFrameType::TransferComplete) {
                        return true;
                    }
                }
                return false;
            },
            std::chrono::seconds(5))) {
        removeScenarioRoot(scenarioRoot);
        return fail("Resumed transfer never reached completion.");
    }

    const std::string status = readTextFileIfExists(statusFile);
    if (exitCode != 0) {
        removeScenarioRoot(scenarioRoot);
        return fail("Resume child process failed with exit code "
                    + std::to_string(exitCode) + ". Status: " + status);
    }
    if (const int result = expect(status.find("ok=1") != std::string::npos,
                                  "Resume child status file mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    {
        std::lock_guard lock(received.mutex);
        if (const int result =
                expect(received.errorMessage.empty(),
                       "Resume receiver failed to parse an incoming frame: "
                       + received.errorMessage);
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }

        bool sawResumeOffer = false;
        bool sawDataOffer = false;
        bool sawChunk = false;
        bool sawComplete = false;
        for (const auto& frame : received.frames) {
            switch (frame.GetType()) {
            case relaydesk::net::PeerFrameType::TransferOffer: {
                const relaydesk::net::TransferOfferMessage offer =
                    relaydesk::net::parseTransferOfferFrame(frame);
                if (offer.GetResumeRequest()) {
                    sawResumeOffer = true;
                    if (const int result = expect(
                            offer.GetMessageId() == messageId
                                && offer.GetPartId() == partId
                                && offer.GetTransferId() == transferId,
                            "Resume offer fields mismatch.");
                        result != 0) {
                        removeScenarioRoot(scenarioRoot);
                        return result;
                    }
                } else {
                    sawDataOffer = true;
                }
                break;
            }
            case relaydesk::net::PeerFrameType::TransferChunk: {
                const relaydesk::net::TransferChunkMessage chunk =
                    relaydesk::net::parseTransferChunkFrame(frame);
                if (const int result =
                        expect(chunk.GetOffset() == 2,
                               "Resumed chunk offset mismatch.");
                    result != 0) {
                    removeScenarioRoot(scenarioRoot);
                    return result;
                }
                if (const int result =
                        expect(frame.GetBody()
                                   == std::vector<std::uint8_t>({'C', 'D'}),
                               "Resumed chunk payload mismatch.");
                    result != 0) {
                    removeScenarioRoot(scenarioRoot);
                    return result;
                }
                sawChunk = true;
                break;
            }
            case relaydesk::net::PeerFrameType::TransferComplete: {
                const relaydesk::net::TransferCompleteMessage complete =
                    relaydesk::net::parseTransferCompleteFrame(frame);
                if (const int result =
                        expect(complete.GetFileSize() == 4,
                               "Resumed transfer complete size mismatch.");
                    result != 0) {
                    removeScenarioRoot(scenarioRoot);
                    return result;
                }
                sawComplete = true;
                break;
            }
            default:
                break;
            }
        }

        if (const int result = expect(sawResumeOffer,
                                      "Resume request was never observed.");
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
        if (const int result = expect(sawDataOffer,
                                      "Resumed transfer never sent data.");
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
        if (const int result = expect(sawChunk,
                                      "Resumed transfer never sent chunk.");
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
        if (const int result = expect(sawComplete,
                                      "Resumed transfer never completed.");
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
    }

    removeScenarioRoot(scenarioRoot);
    return 0;
}

int resumesInterruptedOutgoingFolderTransferAcrossProcesses()
{
    const std::filesystem::path scenarioRoot =
        makeUniqueProcessScenarioRoot("folder-transfer-resume");
    const std::filesystem::path childExecutablePath =
        copyExecutableToScenarioRoot(scenarioRoot);
    const std::uint16_t childListenPort = reserveAvailableTcpPort();
    const relaydesk::storage::AppPaths childAppPaths(childExecutablePath);
    relaydesk::storage::ensureAppDirectories(childAppPaths);

    const std::filesystem::path sourceFolder =
        childAppPaths.GetWorkDirectory() / "Project";
    writeBytes(sourceFolder / "a-root.txt", {'A', 'B'});
    writeBytes(sourceFolder / "nested" / "child.txt", {'C', 'D'});

    const std::string messageId = "process-folder-resume-message";
    const std::string partId = "process-folder-resume-part";
    const std::string transferId = "process-folder-resume-transfer";
    relaydesk::runtime::LocalUserSummary localUser;
    localUser.SetDeviceId("process-sender-device");
    localUser.SetDisplayName("Process Sender");
    localUser.SetHostName("PROCESS-SENDER-HOST");
    relaydesk::storage::appendChatMessage(
        childAppPaths,
        "runtime-transfer-peer",
        makeOutgoingInterruptedFolderTransferRecord(childAppPaths,
                                                    localUser,
                                                    sourceFolder,
                                                    messageId,
                                                    partId,
                                                    transferId));

    struct ReceivedTransferFrames {
        std::mutex mutex;
        std::vector<relaydesk::net::PeerFrame> frames;
        std::string errorMessage;
    };

    ReceivedTransferFrames received;
    relaydesk::net::BoostAsioTcpPeerTransport receiver(0);
    receiver.SetFrameCallback(
        [childListenPort,
         &messageId,
         &partId,
         &received,
         &receiver,
         &transferId](
            relaydesk::net::PeerFrame frame,
            std::string address,
            std::uint16_t) {
            std::optional<relaydesk::net::TransferOfferMessage> offer;
            {
                std::lock_guard lock(received.mutex);
                received.frames.push_back(frame);
                if (frame.GetType()
                    == relaydesk::net::PeerFrameType::TransferOffer) {
                    try {
                        offer = relaydesk::net::parseTransferOfferFrame(frame);
                    } catch (const std::exception& error) {
                        received.errorMessage = error.what();
                        return;
                    }
                }
            }

            if (offer.has_value() && offer->GetResumeRequest()) {
                receiver.sendFrameTo(
                    address,
                    childListenPort,
                    makeTransferAcceptFrame(messageId,
                                            partId,
                                            transferId,
                                            "runtime-transfer-peer",
                                            2));
            }
        });
    receiver.start();

    struct ReceiverStopper {
        relaydesk::net::BoostAsioTcpPeerTransport& receiver;

        ~ReceiverStopper()
        {
            receiver.stop();
        }
    } receiverStopper{receiver};

    const std::filesystem::path statusFile = scenarioRoot / "child-status.txt";
    ChildProcessHandle childProcess = startChildProcess(
        childExecutablePath,
        {L"--process-folder-resume-child",
         std::to_wstring(receiver.GetLocalPort()),
         std::to_wstring(childListenPort),
         statusFile.wstring()});
    const DWORD exitCode =
        waitForChildProcess(childProcess, std::chrono::seconds(30));

    if (!waitForPredicate(
            [&received] {
                std::lock_guard lock(received.mutex);
                for (const auto& frame : received.frames) {
                    if (frame.GetType()
                        == relaydesk::net::PeerFrameType::TransferComplete) {
                        return true;
                    }
                }
                return false;
            },
            std::chrono::seconds(5))) {
        removeScenarioRoot(scenarioRoot);
        return fail("Resumed folder transfer never reached completion.");
    }

    const std::string status = readTextFileIfExists(statusFile);
    if (exitCode != 0) {
        removeScenarioRoot(scenarioRoot);
        return fail("Folder resume child process failed with exit code "
                    + std::to_string(exitCode) + ". Status: " + status);
    }
    if (const int result = expect(status.find("ok=1") != std::string::npos,
                                  "Folder resume child status file mismatch.");
        result != 0) {
        removeScenarioRoot(scenarioRoot);
        return result;
    }

    {
        std::lock_guard lock(received.mutex);
        if (const int result =
                expect(received.errorMessage.empty(),
                       "Folder resume receiver failed to parse a frame: "
                       + received.errorMessage);
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }

        bool sawResumeOffer = false;
        bool sawDataOffer = false;
        bool sawRemainingFileChunk = false;
        bool sawComplete = false;
        for (const auto& frame : received.frames) {
            switch (frame.GetType()) {
            case relaydesk::net::PeerFrameType::TransferOffer: {
                const relaydesk::net::TransferOfferMessage offer =
                    relaydesk::net::parseTransferOfferFrame(frame);
                if (offer.GetResumeRequest()) {
                    sawResumeOffer = true;
                    if (const int result =
                            expect(offer.GetFolderTransfer(),
                                   "Folder resume offer was not marked as folder.");
                        result != 0) {
                        removeScenarioRoot(scenarioRoot);
                        return result;
                    }
                } else {
                    sawDataOffer = true;
                    if (const int result =
                            expect(offer.GetFolderTransfer(),
                                   "Folder data offer was not marked as folder.");
                        result != 0) {
                        removeScenarioRoot(scenarioRoot);
                        return result;
                    }
                }
                if (const int result = expect(
                        offer.GetMessageId() == messageId
                            && offer.GetPartId() == partId
                            && offer.GetTransferId() == transferId,
                        "Folder resume offer fields mismatch.");
                    result != 0) {
                    removeScenarioRoot(scenarioRoot);
                    return result;
                }
                break;
            }
            case relaydesk::net::PeerFrameType::TransferChunk: {
                const relaydesk::net::TransferChunkMessage chunk =
                    relaydesk::net::parseTransferChunkFrame(frame);
                if (chunk.GetFolderDirectory()) {
                    break;
                }
                if (const int result =
                        expect(chunk.GetOffset() == 2,
                               "Resumed folder chunk offset mismatch.");
                    result != 0) {
                    removeScenarioRoot(scenarioRoot);
                    return result;
                }
                if (const int result =
                        expect(chunk.GetFolderRelativePath().has_value()
                                   && chunk.GetFolderRelativePath().value()
                                       == "nested/child.txt",
                               "Resumed folder chunk path mismatch.");
                    result != 0) {
                    removeScenarioRoot(scenarioRoot);
                    return result;
                }
                if (const int result =
                        expect(frame.GetBody()
                                   == std::vector<std::uint8_t>({'C', 'D'}),
                               "Resumed folder chunk payload mismatch.");
                    result != 0) {
                    removeScenarioRoot(scenarioRoot);
                    return result;
                }
                sawRemainingFileChunk = true;
                break;
            }
            case relaydesk::net::PeerFrameType::TransferComplete: {
                const relaydesk::net::TransferCompleteMessage complete =
                    relaydesk::net::parseTransferCompleteFrame(frame);
                if (const int result =
                        expect(complete.GetFileSize() == 4,
                               "Resumed folder complete size mismatch.");
                    result != 0) {
                    removeScenarioRoot(scenarioRoot);
                    return result;
                }
                sawComplete = true;
                break;
            }
            default:
                break;
            }
        }

        if (const int result = expect(sawResumeOffer,
                                      "Folder resume request was never observed.");
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
        if (const int result = expect(sawDataOffer,
                                      "Resumed folder transfer never sent data.");
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
        if (const int result = expect(
                sawRemainingFileChunk,
                "Resumed folder transfer never sent remaining file chunk.");
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
        if (const int result = expect(sawComplete,
                                      "Resumed folder transfer never completed.");
            result != 0) {
            removeScenarioRoot(scenarioRoot);
            return result;
        }
    }

    removeScenarioRoot(scenarioRoot);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (const std::optional<std::filesystem::path> markerPath =
                appUpdateRestartProbePath()) {
            return writeAppUpdateRestartProbe(*markerPath);
        }
    } catch (const std::exception& error) {
        return fail(std::string("Restart probe failed: ") + error.what());
    }

    struct AsyncShutdownGuard {
        ~AsyncShutdownGuard()
        {
            ::core::async::shutdown();
        }
    } asyncShutdownGuard;

    try {
        requireTestProcessIsolation();
        if (const int cleanupGuardResult =
                refusesCleanupOutsideProcessTestRoot();
            cleanupGuardResult != 0) {
            return cleanupGuardResult;
        }
        const std::vector<std::wstring> wideArguments =
            currentProcessWideArgumentsForTest();
        const std::optional<relaydesk::runtime::AppUpdateApplyOptions>
            updateOptions =
                relaydesk::runtime::parseAppUpdateApplyOptions(wideArguments);
        if (updateOptions.has_value()) {
            return relaydesk::runtime::runAppUpdateApplyMode(
                updateOptions.value());
        }
        if (!wideArguments.empty()
            && wideArguments.front() == L"--relaydesk-apply-update") {
            return fail("Invalid update apply mode arguments.");
        }

        if (argc > 1) {
            const std::string mode = argv[1];
            if (mode == "--process-update-child") {
                if (argc != 5) {
                    return fail("Update child mode expects ports and status file.");
                }
                return runProcessUpdateChild(
                    static_cast<std::uint16_t>(std::stoi(argv[2])),
                    static_cast<std::uint16_t>(std::stoi(argv[3])),
                    std::filesystem::path(wideArguments.at(3)));
            }
            if (mode == "--process-resume-child") {
                if (argc != 5) {
                    return fail("Resume child mode expects ports and status file.");
                }
                return runProcessResumeChild(
                    static_cast<std::uint16_t>(std::stoi(argv[2])),
                    static_cast<std::uint16_t>(std::stoi(argv[3])),
                    std::filesystem::path(wideArguments.at(3)));
            }
            if (mode == "--process-folder-resume-child") {
                if (argc != 5) {
                    return fail("Folder resume child mode expects ports and status file.");
                }
                return runProcessFolderResumeChild(
                    static_cast<std::uint16_t>(std::stoi(argv[2])),
                    static_cast<std::uint16_t>(std::stoi(argv[3])),
                    std::filesystem::path(wideArguments.at(3)));
            }
            if (mode == "--process-transfer-receiver-child") {
                if (argc != 7) {
                    return fail(
                        "Transfer receiver child mode expects ports, status file, gate file, and part id.");
                }
                return runProcessTransferReceiverChild(
                    static_cast<std::uint16_t>(std::stoi(argv[2])),
                    static_cast<std::uint16_t>(std::stoi(argv[3])),
                    std::filesystem::path(wideArguments.at(3)),
                    std::filesystem::path(wideArguments.at(4)),
                    argv[6]);
            }
            if (mode == "--process-transfer-resume-receiver-child") {
                if (argc != 6) {
                    return fail(
                        "Transfer resume receiver child mode expects ports, status file, and part id.");
                }
                return runProcessTransferResumeReceiverChild(
                    static_cast<std::uint16_t>(std::stoi(argv[2])),
                    static_cast<std::uint16_t>(std::stoi(argv[3])),
                    std::filesystem::path(wideArguments.at(3)),
                    argv[5]);
            }
        }

        if (const int updatePromptResult =
                queuesUserNotificationForIncomingChatMessage();
            updatePromptResult != 0) {
            return updatePromptResult;
        }
        if (const int persistedIncomingResult =
                persistsIncomingChatMessageBeforeUiDrain();
            persistedIncomingResult != 0) {
            return persistedIncomingResult;
        }
        if (const int pagedHistoryResult = selectsPeerWithPagedHistory();
            pagedHistoryResult != 0) {
            return pagedHistoryResult;
        }
        if (const int updatePromptResult =
                offersAppUpdatePromptWhenOnlinePeerVersionIncreases();
            updatePromptResult != 0) {
            return updatePromptResult;
        }
        if (const int scheduledUpdateResult =
                suppressesScheduledInstallOnExitVersionUntilHigherVersionArrives();
            scheduledUpdateResult != 0) {
            return scheduledUpdateResult;
        }
        if (const int restartUpdateResult =
                requestsProcessExitAfterLaunchingRestartUpdateHelper();
            restartUpdateResult != 0) {
            return restartUpdateResult;
        }
        if (const int highestPromptResult =
                keepsHighestAppUpdatePromptAcrossMultiplePeers();
            highestPromptResult != 0) {
            return highestPromptResult;
        }
        if (const int continuedTransferResult =
                continuesInterruptedIncomingTransferFromAcceptedOffset();
            continuedTransferResult != 0) {
            return continuedTransferResult;
        }
        if (const int restartedTransferResult =
                restartsInterruptedIncomingTransferWhenFreshOfferStartsAtZero();
            restartedTransferResult != 0) {
            return restartedTransferResult;
        }
        if (const int historyResumeResult =
                answersResumeRequestFromStoredHistoryOffset();
            historyResumeResult != 0) {
            return historyResumeResult;
        }
        if (const int repeatedResumeResult =
                answersRepeatedResumeRequestsWithStoredOffset();
            repeatedResumeResult != 0) {
            return repeatedResumeResult;
        }
        if (const int repeatedAcceptResult =
                acceptsRepeatedInterruptedIncomingTransferWithStoredOffset();
            repeatedAcceptResult != 0) {
            return repeatedAcceptResult;
        }
        if (const int outgoingResumeResult =
                resumesInterruptedOutgoingTransferAfterResumeAccept();
            outgoingResumeResult != 0) {
            return outgoingResumeResult;
        }
        if (const int incomingFolderResult =
                acceptsIncomingFolderTransferAndExtractsPayload();
            incomingFolderResult != 0) {
            return incomingFolderResult;
        }
        if (const int autoReceiveResult =
                autoAcceptsIncomingFileTransferWhenEnabled();
            autoReceiveResult != 0) {
            return autoReceiveResult;
        }
        if (const int resumeIncomingFolderResult =
                resumesInterruptedIncomingFolderTransferFromExistingFiles();
            resumeIncomingFolderResult != 0) {
            return resumeIncomingFolderResult;
        }
        if (const int outgoingFolderResult =
                sendsOutgoingFolderTransferWithPackageProgressAndSourcePath();
            outgoingFolderResult != 0) {
            return outgoingFolderResult;
        }
        if (const int tcpBreakResult =
                interruptsIncomingTransferWhenTcpStreamBreaksMidFrame();
            tcpBreakResult != 0) {
            return tcpBreakResult;
        }
        if (const int appUpdateProcessResult =
                runsAppUpdateAcrossProcesses();
            appUpdateProcessResult != 0) {
            return appUpdateProcessResult;
        }
        if (const int updateDataGuardResult =
                rejectsAppUpdateTargetInsideDataDirectory();
            updateDataGuardResult != 0) {
            return updateDataGuardResult;
        }
        if (const int appUpdateRestartResult =
                skiaUiUpdateHelperRestartsUpdatedAppVisible();
            appUpdateRestartResult != 0) {
            return appUpdateRestartResult;
        }
        if (const int fileTransferProcessResult =
                sendsFileTransferBetweenTwoRuntimeProcesses();
            fileTransferProcessResult != 0) {
            return fileTransferProcessResult;
        }
        if (const int folderTransferProcessResult =
                sendsFolderTransferBetweenTwoRuntimeProcesses();
            folderTransferProcessResult != 0) {
            return folderTransferProcessResult;
        }
        if (const int realResumeProcessResult =
                resumesInterruptedFileTransferBetweenTwoRuntimeProcesses();
            realResumeProcessResult != 0) {
            return realResumeProcessResult;
        }
        if (const int processResumeResult =
                resumesInterruptedOutgoingTransferAcrossProcesses();
            processResumeResult != 0) {
            return processResumeResult;
        }
        return resumesInterruptedOutgoingFolderTransferAcrossProcesses();
    } catch (const std::exception& error) {
        return fail(std::string("Unhandled exception: ") + error.what());
    } catch (...) {
        return fail("Unhandled non-standard exception.");
    }
}
