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
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include <windows.h>

namespace {

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

void appendUnsigned32LittleEndian(std::vector<std::uint8_t>& bytes,
                                  std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        bytes.push_back(
            static_cast<std::uint8_t>((value >> (index * 8u)) & 0xFFu));
    }
}

void appendUnsigned64LittleEndian(std::vector<std::uint8_t>& bytes,
                                  std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        bytes.push_back(
            static_cast<std::uint8_t>((value >> (index * 8u)) & 0xFFu));
    }
}

void appendFolderPackageEntry(std::vector<std::uint8_t>& bytes,
                              std::uint8_t type,
                              const std::string& relativePath,
                              const std::string& content)
{
    bytes.push_back(type);
    appendUnsigned32LittleEndian(
        bytes, static_cast<std::uint32_t>(relativePath.size()));
    appendUnsigned64LittleEndian(
        bytes, static_cast<std::uint64_t>(content.size()));
    bytes.insert(bytes.end(), relativePath.begin(), relativePath.end());
    bytes.insert(bytes.end(), content.begin(), content.end());
}

std::vector<std::uint8_t> makeFolderPackagePayload()
{
    std::vector<std::uint8_t> bytes;
    constexpr std::array<char, 8> magic{
        'R',
        'D',
        'F',
        'O',
        'L',
        'D',
        'R',
        '1',
    };
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    appendUnsigned64LittleEndian(bytes, 3);
    appendFolderPackageEntry(bytes, 1, "nested", "");
    appendFolderPackageEntry(bytes, 2, "root.txt", "AB");
    appendFolderPackageEntry(bytes, 2, "nested/child.txt", "CD");
    return bytes;
}

std::filesystem::path processTestRoot()
{
    return std::filesystem::path(RELAYDESK_APP_RUNTIME_PROCESS_TEST_WORK_DIR);
}

std::filesystem::path makeUniqueProcessScenarioRoot(const std::string& prefix)
{
    return processTestRoot()
        / (prefix + "-"
           + std::to_string(
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
    quoted += value;
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

void removeScenarioRoot(const std::filesystem::path& scenarioRoot)
{
    std::error_code error;
    std::filesystem::remove_all(scenarioRoot, error);
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

relaydesk::runtime::RelayDeskRuntimeOptions makeTransferRuntimeOptions()
{
    relaydesk::runtime::RelayDeskRuntimeOptions options = makeTestRuntimeOptions();
    options.SetNetworkEnabled(true);
    return options;
}

relaydesk::storage::PeerProfile makeTransferPeerProfile(std::uint16_t tcpPort)
{
    relaydesk::storage::PeerProfile profile;
    profile.SetDeviceId("runtime-transfer-peer");
    profile.SetHostName("RUNTIME-TRANSFER-HOST");
    profile.SetDisplayName("Runtime Transfer Peer");
    profile.SetLastAddresses({"127.0.0.1"});
    profile.SetTcpPort(tcpPort);
    profile.SetAppVersion(relaydesk::core::kAppVersion);
    profile.SetCapabilities({"text", "file"});
    profile.SetFirstSeenAt("2026-06-21T10:00:00Z");
    profile.SetLastSeenAt("2026-06-21T11:00:00Z");
    return profile;
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
    }

    explicit TestableRelayDeskRuntime(relaydesk::runtime::RelayDeskRuntimeOptions options)
        : RelayDeskRuntime(std::move(options))
    {
    }

    void receivePeerProfile(relaydesk::storage::PeerProfile profile, bool online)
    {
        enqueuePeerProfile(std::move(profile), online);
        refreshPeersIfNeeded();
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
};

template <typename Predicate>
bool waitForCondition(TestableRelayDeskRuntime& runtime, Predicate&& predicate)
{
    for (int attempt = 0; attempt < 300; ++attempt) {
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

int offersAppUpdatePromptWhenOnlinePeerVersionIncreases()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());

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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int suppressesScheduledInstallOnExitVersionUntilHigherVersionArrives()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());

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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int continuesInterruptedIncomingTransferFromAcceptedOffset()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int restartsInterruptedIncomingTransferWhenFreshOfferStartsAtZero()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int answersResumeRequestFromStoredHistoryOffset()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int answersRepeatedResumeRequestsWithStoredOffset()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int acceptsRepeatedInterruptedIncomingTransferWithStoredOffset()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int resumesInterruptedOutgoingTransferAfterResumeAccept()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int acceptsIncomingFolderTransferAndExtractsPayload()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

    const std::vector<std::uint8_t> package = makeFolderPackagePayload();
    const std::string messageId = "incoming-folder-message";
    const std::string partId = "incoming-folder-part";
    const std::string transferId = "incoming-folder-transfer";

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
                                        package.size())));
        payloadFrames.push_back(
            relaydesk::net::makeTransferChunkFrame(
                makeTransferChunk(messageId, partId, transferId, 0),
                package));
        payloadFrames.push_back(
            relaydesk::net::makeTransferCompleteFrame(
                makeFolderTransferComplete(messageId,
                                           partId,
                                           transferId,
                                           package.size())));
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
                           && part.GetFileSize().value() == package.size(),
                       "Incoming folder file size did not use package size.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(part.GetTransferredSize().has_value()
                           && part.GetTransferredSize().value()
                               == package.size(),
                       "Incoming folder progress did not reach package size.");
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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int sendsOutgoingFolderTransferWithPackageProgressAndSourcePath()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

        std::uintmax_t packageSize = 0;
        {
            std::lock_guard lock(received.mutex);
            const relaydesk::net::TransferOfferMessage offer =
                relaydesk::net::parseTransferOfferFrame(received.frames[0]);
            if (const int result = expect(offer.GetFolderTransfer(),
                                          "Outgoing folder offer flag missing.");
                result != 0) {
                return result;
            }
            packageSize = offer.GetFileSize();

            const relaydesk::net::TransferCompleteMessage complete =
                relaydesk::net::parseTransferCompleteFrame(received.frames[2]);
            if (const int result =
                    expect(complete.GetFileSize() == packageSize,
                           "Outgoing folder complete size mismatch.");
                result != 0) {
                return result;
            }
        }

        if (const int result = expect(packageSize > 4,
                                      "Folder package size did not include metadata.");
            result != 0) {
            return result;
        }

        const auto& part = runtime.GetSelectedPeerMessages()
                               .front()
                               .GetParts()
                               .front();
        if (const int result =
                expect(part.GetFileSize().has_value()
                           && part.GetFileSize().value() == packageSize,
                       "Outgoing folder progress total did not use package size.");
            result != 0) {
            return result;
        }
        if (const int result =
                expect(part.GetTransferredSize().has_value()
                           && part.GetTransferredSize().value() == packageSize,
                       "Outgoing folder progress did not reach package size.");
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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
    return 0;
}

int interruptsIncomingTransferWhenTcpStreamBreaksMidFrame()
{
    const relaydesk::storage::AppPaths appPaths =
        relaydesk::storage::createAppPaths();
    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

    std::filesystem::remove_all(appPaths.GetDataDirectory());
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

    writeTextFile(statusFile,
                  "ok=1\nreceived_size="
                      + std::to_string(scheduledUpdate->GetReceivedSize())
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

int runsAppUpdateAcrossProcesses()
{
    const std::filesystem::path scenarioRoot =
        makeUniqueProcessScenarioRoot("app-update");
    const std::filesystem::path childExecutablePath =
        copyExecutableToScenarioRoot(scenarioRoot);
    const std::filesystem::path statusFile = scenarioRoot / "child-status.txt";
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

} // namespace

int main(int argc, char** argv)
{
    struct AsyncShutdownGuard {
        ~AsyncShutdownGuard()
        {
            ::core::async::shutdown();
        }
    } asyncShutdownGuard;

    try {
        if (argc > 1) {
            const std::string mode = argv[1];
            if (mode == "--process-update-child") {
                if (argc != 5) {
                    return fail("Update child mode expects ports and status file.");
                }
                return runProcessUpdateChild(
                    static_cast<std::uint16_t>(std::stoi(argv[2])),
                    static_cast<std::uint16_t>(std::stoi(argv[3])),
                    std::filesystem::path(argv[4]));
            }
            if (mode == "--process-resume-child") {
                if (argc != 5) {
                    return fail("Resume child mode expects ports and status file.");
                }
                return runProcessResumeChild(
                    static_cast<std::uint16_t>(std::stoi(argv[2])),
                    static_cast<std::uint16_t>(std::stoi(argv[3])),
                    std::filesystem::path(argv[4]));
            }
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
        return resumesInterruptedOutgoingTransferAcrossProcesses();
    } catch (const std::exception& error) {
        return fail(std::string("Unhandled exception: ") + error.what());
    } catch (...) {
        return fail("Unhandled non-standard exception.");
    }
}
