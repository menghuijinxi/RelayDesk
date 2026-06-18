#include "main/app_runtime.h"

#include "core/app_version.h"
#include "core/diagnostic_log.h"
#include "core/platform/async.h"
#include "core/time.h"
#include "core/uuid.h"
#include "main/image_attachment_store.h"
#include "platform/computer_name.h"
#include "platform/text_encoding.h"
#include "platform/windows_install_id.h"
#include "storage/app_paths.h"
#include "storage/history_store.h"
#include "storage/local_identity.h"
#include "storage/peer_profile.h"

#if defined(RELAYDESK_HAS_BOOST_ASIO)
#include "net/boost_asio_tcp_peer_transport.h"
#include "net/discovery_message.h"
#include "net/discovery_service.h"
#include "net/discovery_worker.h"
#include "net/peer_message.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace relaydesk::runtime {
namespace {

using namespace std::chrono_literals;

constexpr const char* kDiscoveryLogFileName = "discovery.log";
constexpr auto kPeerOnlineTimeout = 10s;
constexpr auto kPeerStatusRefreshInterval = 1s;
#if defined(RELAYDESK_HAS_BOOST_ASIO)
constexpr std::uintmax_t kTransferChunkSize = 256u * 1024u;
constexpr const char* kAppUpdateTempDirectoryName = "updates";
constexpr const char* kAppUpdateTempFileName = "relaydesk-update.exe";
constexpr const char* kAppUpdateScriptFileName = "apply-update.cmd";
constexpr const char* kAppUpdateScriptLogFileName = "apply-update.log";
constexpr std::array<char, 8> kFolderPackageMagic{
    'R',
    'D',
    'F',
    'O',
    'L',
    'D',
    'R',
    '1',
};
constexpr std::size_t kMaxFolderPackageRelativePathBytes = 32u * 1024u;
using TransferProgressCallback = std::function<void(const std::string&,
                                                    const std::string&,
                                                    const std::string&,
                                                    std::uintmax_t)>;
#endif

constexpr bool discoveryTraceEnabled()
{
#if defined(RELAYDESK_ENABLE_DISCOVERY_TRACE)
    return true;
#else
    return false;
#endif
}

std::string choosePeerAddress(const relaydesk::storage::PeerProfile& profile)
{
    if (profile.GetLastAddresses().empty()) {
        return "unknown";
    }

    return profile.GetLastAddresses().front();
}

bool isPeerOnline(std::chrono::steady_clock::time_point lastOnlineSignalAt,
                  std::chrono::steady_clock::time_point now)
{
    return lastOnlineSignalAt != std::chrono::steady_clock::time_point{}
        && lastOnlineSignalAt + kPeerOnlineTimeout >= now;
}

bool hasPeerAddress(const relaydesk::storage::PeerProfile& profile,
                    const std::string& address)
{
    const auto& addresses = profile.GetLastAddresses();
    return std::find(addresses.begin(), addresses.end(), address) != addresses.end();
}

bool isStalePeerForProfile(const PeerListItem& peer,
                           const relaydesk::storage::PeerProfile& profile)
{
    if (peer.GetDeviceId() == profile.GetDeviceId()
        || peer.GetHostName() != profile.GetHostName()
        || peer.GetAddress().empty()
        || peer.GetAddress() == "unknown") {
        return false;
    }

    return hasPeerAddress(profile, peer.GetAddress());
}

bool isStaleOnlineProfile(const PeerListItem& peer,
                          const relaydesk::storage::PeerProfile& profile,
                          bool online)
{
    return online
        && !peer.GetLastSeenAt().empty()
        && !profile.GetLastSeenAt().empty()
        && profile.GetLastSeenAt() <= peer.GetLastSeenAt();
}

PeerListItem makePeerListItem(const relaydesk::storage::PeerProfile& profile,
                              bool online)
{
    PeerListItem item;
    item.SetDeviceId(profile.GetDeviceId());
    item.SetDisplayName(profile.GetDisplayName());
    item.SetHostName(profile.GetHostName());
    item.SetAddress(choosePeerAddress(profile));
    item.SetLastSeenAt(profile.GetLastSeenAt());
    item.SetTcpPort(profile.GetTcpPort());
    item.SetAppVersion(profile.GetAppVersion());
    item.SetOnline(online);
    return item;
}

std::string peerSortName(const PeerListItem& peer)
{
    if (!peer.GetDisplayName().empty()) {
        return peer.GetDisplayName();
    }
    if (!peer.GetHostName().empty()) {
        return peer.GetHostName();
    }
    if (!peer.GetAddress().empty()) {
        return peer.GetAddress();
    }
    return peer.GetDeviceId();
}

bool isPeerListItemBefore(const PeerListItem& left,
                          const PeerListItem& right)
{
    if (left.GetOnline() != right.GetOnline()) {
        return left.GetOnline();
    }
    if (left.GetLastConversationAt() != right.GetLastConversationAt()) {
        return left.GetLastConversationAt() > right.GetLastConversationAt();
    }
    if (left.GetLastSeenAt() != right.GetLastSeenAt()) {
        return left.GetLastSeenAt() > right.GetLastSeenAt();
    }

    const std::string leftName = peerSortName(left);
    const std::string rightName = peerSortName(right);
    if (leftName != rightName) {
        return leftName < rightName;
    }
    return left.GetDeviceId() < right.GetDeviceId();
}

std::string latestConversationAt(
    const std::vector<relaydesk::storage::ChatMessageRecord>& records)
{
    std::string result;
    for (const auto& record : records) {
        if (record.GetCreatedAt() > result) {
            result = record.GetCreatedAt();
        }
    }
    return result;
}

std::string loadPeerLastConversationAt(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& peerDeviceId)
{
    return latestConversationAt(
        relaydesk::storage::loadChatHistory(appPaths, peerDeviceId).GetRecords());
}

#if defined(RELAYDESK_HAS_BOOST_ASIO)
relaydesk::net::DiscoveryWorkerConfig makeDiscoveryWorkerConfig()
{
    relaydesk::net::DiscoveryWorkerConfig config;
    config.SetBroadcastInterval(2s);
    config.SetStartupBroadcastInterval(500ms);
    config.SetStartupBroadcastCount(8);
    config.SetPollTimeout(100ms);
    config.SetBroadcastEnabled(true);
    config.SetAnnounceOnStart(true);
    return config;
}
#endif

std::filesystem::path makeDiscoveryLogFilePath(
    const relaydesk::storage::AppPaths& appPaths)
{
    return appPaths.GetLogsDirectory() / kDiscoveryLogFileName;
}

std::string getStableDeviceId()
{
    try {
        return relaydesk::platform::getWindowsInstallId();
    } catch (const std::exception&) {
        return {};
    }
}

bool isBlankText(const std::string& text)
{
    return std::all_of(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
}

bool isFileTransferPart(const relaydesk::storage::ChatMessagePart& part)
{
    return part.GetType() == relaydesk::storage::MessagePartType::Image
        || part.GetType() == relaydesk::storage::MessagePartType::File
        || part.GetType() == relaydesk::storage::MessagePartType::Folder;
}

bool isImmediateTransferPart(const relaydesk::storage::ChatMessagePart& part)
{
    return part.GetType() == relaydesk::storage::MessagePartType::Image;
}

bool isManualTransferInvitePart(const relaydesk::storage::ChatMessagePart& part)
{
    return part.GetType() == relaydesk::storage::MessagePartType::File
        || part.GetType() == relaydesk::storage::MessagePartType::Folder;
}

bool isCancellableFileTransferState(relaydesk::storage::TransferState state)
{
    return state == relaydesk::storage::TransferState::Offered
        || state == relaydesk::storage::TransferState::Transferring;
}

bool isTerminalTransferState(relaydesk::storage::TransferState state)
{
    return state == relaydesk::storage::TransferState::Completed
        || state == relaydesk::storage::TransferState::Failed
        || state == relaydesk::storage::TransferState::Cancelled
        || state == relaydesk::storage::TransferState::Rejected;
}

bool shouldIgnoreTransferStateUpdate(
    const relaydesk::storage::ChatMessagePart& part,
    relaydesk::storage::TransferState nextState)
{
    if (!part.GetTransferState().has_value()) {
        return false;
    }

    const relaydesk::storage::TransferState currentState =
        part.GetTransferState().value();
    const bool nextStateIsMoreSpecific =
        currentState == relaydesk::storage::TransferState::Failed
        && (nextState == relaydesk::storage::TransferState::Rejected
            || nextState == relaydesk::storage::TransferState::Cancelled);
    return currentState != nextState
        && isTerminalTransferState(currentState)
        && !nextStateIsMoreSpecific;
}

bool isSendableMessagePart(const relaydesk::storage::ChatMessagePart& part)
{
    switch (part.GetType()) {
    case relaydesk::storage::MessagePartType::Text:
        return part.GetText().has_value() && !isBlankText(part.GetText().value());
    case relaydesk::storage::MessagePartType::Emoji:
        return part.GetEmoji().has_value() && !isBlankText(part.GetEmoji().value());
    case relaydesk::storage::MessagePartType::Image:
    case relaydesk::storage::MessagePartType::File:
    case relaydesk::storage::MessagePartType::Folder:
        return true;
    }

    return false;
}

void setTransferProgress(
    relaydesk::storage::ChatMessagePart& part,
    std::uintmax_t transferredSize)
{
    if (part.GetFileSize().has_value()) {
        transferredSize = std::min(transferredSize, part.GetFileSize().value());
    }
    part.SetTransferredSize(transferredSize);
}

void setTransferProgressForState(
    relaydesk::storage::ChatMessagePart& part,
    relaydesk::storage::TransferState transferState)
{
    switch (transferState) {
    case relaydesk::storage::TransferState::Pending:
    case relaydesk::storage::TransferState::Offered:
        setTransferProgress(part, 0);
        return;
    case relaydesk::storage::TransferState::Transferring:
        if (!part.GetTransferredSize().has_value()) {
            setTransferProgress(part, 0);
        }
        return;
    case relaydesk::storage::TransferState::Completed:
        if (part.GetFileSize().has_value()) {
            setTransferProgress(part, part.GetFileSize().value());
        }
        return;
    case relaydesk::storage::TransferState::Failed:
    case relaydesk::storage::TransferState::Cancelled:
    case relaydesk::storage::TransferState::Rejected:
        return;
    }
}

relaydesk::storage::ChatMessageRecord recordWithTransferState(
    relaydesk::storage::ChatMessageRecord record,
    relaydesk::storage::TransferState transferState)
{
    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    for (auto& part : parts) {
        if (isFileTransferPart(part)) {
            part.SetTransferState(transferState);
            setTransferProgressForState(part, transferState);
        }
    }
    record.SetParts(std::move(parts));
    return record;
}

relaydesk::storage::ChatMessageRecord recordAfterOutgoingChatSend(
    relaydesk::storage::ChatMessageRecord record,
    bool sent)
{
    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    for (auto& part : parts) {
        if (isImmediateTransferPart(part)) {
            part.SetTransferState(sent
                                      ? relaydesk::storage::TransferState::Completed
                                      : relaydesk::storage::TransferState::Failed);
            setTransferProgressForState(part, part.GetTransferState().value());
        } else if (isManualTransferInvitePart(part)) {
            part.SetTransferState(sent
                                      ? relaydesk::storage::TransferState::Offered
                                      : relaydesk::storage::TransferState::Failed);
            setTransferProgressForState(part, part.GetTransferState().value());
        }
    }
    record.SetParts(std::move(parts));
    return record;
}

std::vector<relaydesk::storage::ChatMessagePart> normalizeOutgoingParts(
    std::vector<relaydesk::storage::ChatMessagePart> parts)
{
    std::vector<relaydesk::storage::ChatMessagePart> result;
    result.reserve(parts.size());
    for (auto& part : parts) {
        if (!isSendableMessagePart(part)) {
            continue;
        }
        if (part.GetPartId().empty()) {
            part.SetPartId("p" + std::to_string(result.size() + 1));
        }
        result.push_back(std::move(part));
    }
    return result;
}

std::string peerDeviceIdForRecord(
    const relaydesk::storage::ChatMessageRecord& record,
    const std::string& localDeviceId)
{
    if (record.GetSenderDeviceId() == localDeviceId) {
        return record.GetReceiverDeviceId();
    }
    return record.GetSenderDeviceId();
}

relaydesk::storage::ChatMessagePart makeTextPart(std::string text)
{
    relaydesk::storage::ChatMessagePart part;
    part.SetType(relaydesk::storage::MessagePartType::Text);
    part.SetText(std::move(text));
    return part;
}

relaydesk::storage::ChatMessageRecord makeOutgoingMessageRecord(
    const LocalUserSummary& localUser,
    const PeerListItem& peer,
    std::vector<relaydesk::storage::ChatMessagePart> parts)
{
    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(relaydesk::core::createUuidV4());
    record.SetConversationId(relaydesk::storage::makeDirectConversationId(
        localUser.GetDeviceId(),
        peer.GetDeviceId()));
    record.SetDirection(relaydesk::storage::MessageDirection::Outgoing);
    record.SetSenderDeviceId(localUser.GetDeviceId());
    record.SetReceiverDeviceId(peer.GetDeviceId());
    record.SetSenderDisplayNameSnapshot(localUser.GetDisplayName());
    record.SetReceiverDisplayNameSnapshot(
        peer.GetDisplayName().empty() ? peer.GetHostName() : peer.GetDisplayName());
    record.SetCreatedAt(relaydesk::core::currentUtcTimestamp());
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Pending);
    record.SetParts(std::move(parts));
    return record;
}

relaydesk::storage::ChatMessageRecord makeRetryMessageRecord(
    relaydesk::storage::ChatMessageRecord record)
{
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Pending);
    return recordWithTransferState(
        std::move(record),
        relaydesk::storage::TransferState::Pending);
}

relaydesk::storage::ChatMessageRecord makeIncomingRecordForLocalDevice(
    relaydesk::storage::ChatMessageRecord record)
{
    record.SetDirection(relaydesk::storage::MessageDirection::Incoming);
    record.SetDeliveryState(relaydesk::storage::DeliveryState::Received);
    record = recordWithTransferState(
        std::move(record),
        relaydesk::storage::TransferState::Offered);
    return record;
}

bool applyTransferPartUpdate(
    relaydesk::storage::ChatMessageRecord& record,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    const std::string& fileName,
    std::uintmax_t fileSize,
    const std::string& localPath,
    const std::optional<std::string>& sha256,
    relaydesk::storage::TransferState transferState)
{
    if (record.GetMessageId() != messageId) {
        return false;
    }

    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    bool updated = false;
    for (auto& part : parts) {
        const bool partMatches = part.GetPartId() == partId
            || (part.GetTransferId().has_value()
                && part.GetTransferId().value() == transferId);
        if (!partMatches || !isFileTransferPart(part)) {
            continue;
        }
        if (shouldIgnoreTransferStateUpdate(part, transferState)) {
            return false;
        }

        part.SetTransferId(transferId);
        part.SetTransferState(transferState);
        part.SetFileName(fileName);
        part.SetFileSize(fileSize);
        part.SetLocalPath(localPath);
        if (sha256.has_value()) {
            part.SetSha256(sha256.value());
        }
        setTransferProgressForState(part, transferState);
        updated = true;
        break;
    }

    if (updated) {
        record.SetParts(std::move(parts));
    }
    return updated;
}

bool applyTransferPartStateUpdate(
    relaydesk::storage::ChatMessageRecord& record,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    relaydesk::storage::TransferState transferState)
{
    if (record.GetMessageId() != messageId) {
        return false;
    }

    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    bool updated = false;
    for (auto& part : parts) {
        const bool partMatches = part.GetPartId() == partId
            || (part.GetTransferId().has_value()
                && part.GetTransferId().value() == transferId);
        if (!partMatches || !isFileTransferPart(part)) {
            continue;
        }
        if (shouldIgnoreTransferStateUpdate(part, transferState)) {
            return false;
        }

        part.SetTransferState(transferState);
        setTransferProgressForState(part, transferState);
        updated = true;
        break;
    }

    if (updated) {
        record.SetParts(std::move(parts));
    }
    return updated;
}

bool applyTransferPartProgressUpdate(
    relaydesk::storage::ChatMessageRecord& record,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    std::uintmax_t transferredSize)
{
    if (record.GetMessageId() != messageId) {
        return false;
    }

    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    bool updated = false;
    for (auto& part : parts) {
        const bool partMatches = part.GetPartId() == partId
            || (part.GetTransferId().has_value()
                && part.GetTransferId().value() == transferId);
        if (!partMatches || !isFileTransferPart(part)) {
            continue;
        }

        setTransferProgress(part, transferredSize);
        updated = true;
        break;
    }

    if (updated) {
        record.SetParts(std::move(parts));
    }
    return updated;
}

bool hasTransferPartState(
    const relaydesk::storage::ChatMessageRecord& record,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    relaydesk::storage::TransferState transferState)
{
    if (record.GetMessageId() != messageId) {
        return false;
    }

    for (const auto& part : record.GetParts()) {
        const bool partMatches = part.GetPartId() == partId
            || (part.GetTransferId().has_value()
                && part.GetTransferId().value() == transferId);
        if (partMatches
            && isFileTransferPart(part)
            && part.GetTransferState().has_value()
            && part.GetTransferState().value() == transferState) {
            return true;
        }
    }

    return false;
}

#if defined(RELAYDESK_HAS_BOOST_ASIO)
std::filesystem::path filesystemPathFromUtf8String(const std::string& pathText)
{
#if defined(_WIN32)
    return std::filesystem::path(relaydesk::platform::utf8ToWide(pathText));
#else
    return std::filesystem::path(pathText);
#endif
}

std::string filesystemPathToUtf8String(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

std::string filesystemPathToGenericUtf8String(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
}

std::filesystem::path resolveLocalPath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& pathText)
{
    std::filesystem::path path = filesystemPathFromUtf8String(pathText);
    if (path.is_absolute()) {
        return path;
    }
    return appPaths.GetWorkDirectory() / path;
}

std::string makeWorkRelativePath(const relaydesk::storage::AppPaths& appPaths,
                                 const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path relativePath =
        std::filesystem::relative(path, appPaths.GetWorkDirectory(), error);
    if (error || relativePath.empty()) {
        return filesystemPathToUtf8String(path);
    }
    return filesystemPathToGenericUtf8String(relativePath);
}

std::string fileNameFromPathText(const std::string& pathText)
{
    const std::size_t position = pathText.find_last_of("/\\");
    if (position == std::string::npos) {
        return pathText;
    }
    return pathText.substr(position + 1);
}

std::string sanitizeFileName(std::string fileName)
{
    fileName = fileNameFromPathText(fileName);
    if (fileName.empty() || fileName == "." || fileName == "..") {
        return "transfer.bin";
    }

    for (char& value : fileName) {
        if (value == '/' || value == '\\' || value == ':' || value == '*'
            || value == '?' || value == '"' || value == '<' || value == '>'
            || value == '|') {
            value = '_';
        }
    }
    return fileName;
}

std::filesystem::path makeIncomingTempFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& transferId,
    const std::string& fileName)
{
    return appPaths.GetTempTransfersDirectory()
        / filesystemPathFromUtf8String(sanitizeFileName(transferId))
        / filesystemPathFromUtf8String(sanitizeFileName(fileName) + ".part");
}

std::filesystem::path makeAvailableSiblingPath(
    const std::filesystem::path& desiredPath)
{
    std::error_code error;
    if (!std::filesystem::exists(desiredPath, error)) {
        return desiredPath;
    }

    const std::filesystem::path directory = desiredPath.parent_path();
    const std::wstring stem = desiredPath.stem().wstring();
    const std::wstring extension = desiredPath.extension().wstring();
    for (int index = 1; index < 10000; ++index) {
        const std::filesystem::path candidate =
            directory / (stem + L"(" + std::to_wstring(index) + L")" + extension);
        error.clear();
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }

    throw std::runtime_error("No available incoming transfer file name.");
}

std::filesystem::path makeIncomingFinalFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const PendingIncomingTransfer& transfer)
{
    return makeAvailableSiblingPath(
        appPaths.GetInboxDirectory()
        / filesystemPathFromUtf8String(sanitizeFileName(transfer.GetFileName())));
}

std::filesystem::path makeIncomingDesiredFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& fileName)
{
    return appPaths.GetInboxDirectory()
        / filesystemPathFromUtf8String(sanitizeFileName(fileName));
}

std::filesystem::path makeAppUpdateDirectory(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& requestId)
{
    return appPaths.GetTempTransfersDirectory()
        / kAppUpdateTempDirectoryName
        / filesystemPathFromUtf8String(sanitizeFileName(requestId));
}

std::filesystem::path makeAppUpdateTempFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& requestId)
{
    return makeAppUpdateDirectory(appPaths, requestId)
        / filesystemPathFromUtf8String(kAppUpdateTempFileName);
}

std::filesystem::path makeAppUpdateScriptFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& requestId)
{
    return makeAppUpdateDirectory(appPaths, requestId)
        / filesystemPathFromUtf8String(kAppUpdateScriptFileName);
}

std::string appUpdatePackageFileName(
    const relaydesk::storage::AppPaths& appPaths)
{
    const std::string executableFileName =
        filesystemPathToUtf8String(appPaths.GetExecutablePath().filename());
    if (executableFileName.empty()) {
        return "relaydesk.exe";
    }
    return sanitizeFileName(executableFileName);
}

std::string escapedBatchValue(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (char character : value) {
        if (character == '%') {
            result += "%%";
        } else {
            result.push_back(character);
        }
    }
    return result;
}

std::string batchPathFromScriptDirectory(
    const std::filesystem::path& scriptDirectory,
    const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path relativePath =
        std::filesystem::relative(path, scriptDirectory, error);
    if (!error && !relativePath.empty()) {
        return "!SCRIPT_DIR!" + filesystemPathToUtf8String(relativePath);
    }
    return filesystemPathToUtf8String(path);
}

std::uint32_t currentProcessId()
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return 0;
#endif
}

void writeAppUpdateScript(const std::filesystem::path& scriptPath,
                          const std::filesystem::path& targetPath,
                          const std::filesystem::path& updatePath,
                          const std::filesystem::path& startDirectory,
                          bool restartAfterApply)
{
    std::filesystem::create_directories(scriptPath.parent_path());
    std::ofstream output(scriptPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create app update script.");
    }

    const std::filesystem::path scriptDirectory = scriptPath.parent_path();
    const std::filesystem::path logPath =
        scriptDirectory /
        filesystemPathFromUtf8String(kAppUpdateScriptLogFileName);
    const std::string targetBatchPath =
        batchPathFromScriptDirectory(scriptDirectory, targetPath);
    const std::string updateBatchPath =
        batchPathFromScriptDirectory(scriptDirectory, updatePath);
    const std::string startDirectoryBatchPath =
        batchPathFromScriptDirectory(scriptDirectory, startDirectory);
    const std::string logBatchPath =
        batchPathFromScriptDirectory(scriptDirectory, logPath);

    const std::array<unsigned char, 3> bom{0xEF, 0xBB, 0xBF};
    output.write(reinterpret_cast<const char*>(bom.data()),
                 static_cast<std::streamsize>(bom.size()));

    output << "@echo off\n";
    output << "setlocal EnableExtensions DisableDelayedExpansion\n";
    output << "chcp 65001 >nul\n";
    output << "set \"SCRIPT_DIR=%~dp0\"\n";
    output << "set \"PID=" << currentProcessId() << "\"\n";
    output << "setlocal EnableDelayedExpansion\n";
    output << "set \"TARGET="
           << escapedBatchValue(targetBatchPath) << "\"\n";
    output << "set \"UPDATE="
           << escapedBatchValue(updateBatchPath) << "\"\n";
    output << "set \"STARTDIR="
           << escapedBatchValue(startDirectoryBatchPath) << "\"\n";
    output << "set \"LOG="
           << escapedBatchValue(logBatchPath) << "\"\n";
    output << "> \"!LOG!\" echo [%date% %time%] RelayDesk update started\n";
    output << ">> \"!LOG!\" echo script=%~f0\n";
    output << ">> \"!LOG!\" echo target=!TARGET!\n";
    output << ">> \"!LOG!\" echo update=!UPDATE!\n";
    output << ">> \"!LOG!\" echo startdir=!STARTDIR!\n";
    output << ">> \"!LOG!\" echo pid=!PID!\n";
    output << "if not exist \"!UPDATE!\" (\n";
    output << "  >> \"!LOG!\" echo update file missing\n";
    output << "  exit /b 2\n";
    output << ")\n";
    output << "taskkill /pid !PID! /f >> \"!LOG!\" 2>&1\n";
    output << "for /l %%i in (1,1,60) do (\n";
    output << "  tasklist /fi \"PID eq !PID!\" | findstr /r /c:\"!PID!\" >nul\n";
    output << "  if errorlevel 1 goto copy_update\n";
    output << "  timeout /t 1 /nobreak >nul\n";
    output << ")\n";
    output << ">> \"!LOG!\" echo process did not exit\n";
    output << "exit /b 3\n";
    output << ":copy_update\n";
    output << "for /l %%i in (1,1,60) do (\n";
    output << "  copy /y \"!UPDATE!\" \"!TARGET!\" >> \"!LOG!\" 2>&1\n";
    output << "  if not errorlevel 1 goto copy_done\n";
    output << "  timeout /t 1 /nobreak >nul\n";
    output << ")\n";
    output << ">> \"!LOG!\" echo copy failed\n";
    output << "exit /b 4\n";
    output << ":copy_done\n";
    output << ">> \"!LOG!\" echo copy succeeded\n";
    if (restartAfterApply) {
        output << "start \"\" /D \"!STARTDIR!\" \"!TARGET!\"\n";
        output << "if errorlevel 1 (\n";
        output << "  >> \"!LOG!\" echo restart failed\n";
        output << "  exit /b 5\n";
        output << ")\n";
        output << ">> \"!LOG!\" echo restart requested\n";
    } else {
        output << ">> \"!LOG!\" echo restart skipped\n";
    }
    output << ">> \"!LOG!\" echo update script finished\n";
    output << "exit /b 0\n";
    if (!output) {
        throw std::runtime_error("Failed to write app update script.");
    }
}

bool launchAppUpdateScript(const std::filesystem::path& scriptPath)
{
#if defined(_WIN32)
    const std::wstring script = scriptPath.wstring();
    const std::wstring directory = scriptPath.parent_path().wstring();
    const HINSTANCE result = ShellExecuteW(nullptr,
                                           L"open",
                                           script.c_str(),
                                           nullptr,
                                           directory.c_str(),
                                           SW_HIDE);
    return reinterpret_cast<std::intptr_t>(result) > 32;
#else
    (void)scriptPath;
    return false;
#endif
}

enum class FolderPackageEntryType : std::uint8_t {
    Directory = 1,
    File = 2,
};

struct FolderPackageEntry {
    FolderPackageEntryType type = FolderPackageEntryType::File;
    std::filesystem::path sourcePath;
    std::string relativePath;
    std::uintmax_t fileSize = 0;
};

void writeUnsigned32LittleEndian(std::ostream& output, std::uint32_t value)
{
    std::array<char, 4> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8u)) & 0xFFu);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void writeUnsigned64LittleEndian(std::ostream& output, std::uint64_t value)
{
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8u)) & 0xFFu);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t readUnsigned32LittleEndian(std::istream& input)
{
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("Folder transfer package is truncated.");
    }

    std::uint32_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8u);
    }
    return value;
}

std::uint64_t readUnsigned64LittleEndian(std::istream& input)
{
    std::array<unsigned char, 8> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("Folder transfer package is truncated.");
    }

    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8u);
    }
    return value;
}

void copyExactBytes(std::istream& input,
                    std::ostream& output,
                    std::uint64_t byteCount)
{
    std::vector<char> buffer(static_cast<std::size_t>(kTransferChunkSize));
    std::uint64_t remaining = byteCount;
    while (remaining > 0) {
        const std::size_t chunkSize = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(chunkSize));
        if (input.gcount() != static_cast<std::streamsize>(chunkSize)) {
            throw std::runtime_error("Folder transfer package payload is truncated.");
        }

        output.write(buffer.data(), static_cast<std::streamsize>(chunkSize));
        if (!output) {
            throw std::runtime_error("Failed to write folder transfer payload.");
        }
        remaining -= chunkSize;
    }
}

bool isSafeFolderPackageRelativePath(const std::filesystem::path& relativePath)
{
    if (relativePath.empty() || relativePath.is_absolute()) {
        return false;
    }

    for (const auto& component : relativePath) {
        const std::string text = filesystemPathToGenericUtf8String(component);
        if (text.empty() || text == "." || text == ".."
            || text.find(':') != std::string::npos) {
            return false;
        }
    }
    return true;
}

std::string makeFolderPackageRelativePath(
    const std::filesystem::path& folderRoot,
    const std::filesystem::path& entryPath)
{
    std::error_code error;
    const std::filesystem::path relativePath =
        std::filesystem::relative(entryPath, folderRoot, error).lexically_normal();
    if (error || !isSafeFolderPackageRelativePath(relativePath)) {
        throw std::runtime_error("Folder transfer path is invalid.");
    }
    return filesystemPathToGenericUtf8String(relativePath);
}

std::vector<FolderPackageEntry> collectFolderPackageEntries(
    const std::filesystem::path& folderRoot)
{
    std::vector<FolderPackageEntry> entries;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        folderRoot,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        std::error_code entryError;
        if (iterator->is_symlink(entryError) && !entryError) {
            iterator.increment(error);
            if (error) {
                error.clear();
            }
            continue;
        }

        entryError.clear();
        if (iterator->is_directory(entryError) && !entryError) {
            entries.push_back(
                {FolderPackageEntryType::Directory,
                 iterator->path(),
                 makeFolderPackageRelativePath(folderRoot, iterator->path()),
                 0});
            iterator.increment(error);
            if (error) {
                error.clear();
            }
            continue;
        }

        entryError.clear();
        if (iterator->is_regular_file(entryError) && !entryError) {
            const std::uintmax_t fileSize =
                std::filesystem::file_size(iterator->path(), entryError);
            if (!entryError) {
                entries.push_back(
                    {FolderPackageEntryType::File,
                     iterator->path(),
                     makeFolderPackageRelativePath(folderRoot, iterator->path()),
                     fileSize});
            }
        }

        iterator.increment(error);
        if (error) {
            error.clear();
        }
    }

    std::sort(entries.begin(),
              entries.end(),
              [](const FolderPackageEntry& left,
                 const FolderPackageEntry& right) {
                  return left.relativePath < right.relativePath;
              });
    return entries;
}

void writeFolderPackageEntryHeader(std::ostream& output,
                                   const FolderPackageEntry& entry)
{
    if (entry.relativePath.size() > kMaxFolderPackageRelativePathBytes) {
        throw std::runtime_error("Folder transfer path is too long.");
    }

    output.put(static_cast<char>(entry.type));
    writeUnsigned32LittleEndian(
        output,
        static_cast<std::uint32_t>(entry.relativePath.size()));
    writeUnsigned64LittleEndian(
        output,
        static_cast<std::uint64_t>(entry.fileSize));
    output.write(entry.relativePath.data(),
                 static_cast<std::streamsize>(entry.relativePath.size()));
    if (!output) {
        throw std::runtime_error("Failed to write folder transfer package.");
    }
}

std::filesystem::path makeFolderPackagePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& transferId)
{
    return appPaths.GetTempTransfersDirectory()
        / filesystemPathFromUtf8String(sanitizeFileName(transferId))
        / "folder.package";
}

std::filesystem::path createFolderTransferPackage(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& sourceFolder,
    const std::string& transferId)
{
    std::error_code error;
    if (!std::filesystem::is_directory(sourceFolder, error) || error) {
        throw std::runtime_error("Transfer source folder does not exist.");
    }

    const std::filesystem::path packagePath =
        makeFolderPackagePath(appPaths, transferId);
    std::filesystem::create_directories(packagePath.parent_path());
    std::filesystem::remove(packagePath, error);
    const std::vector<FolderPackageEntry> entries =
        collectFolderPackageEntries(sourceFolder);

    std::ofstream output(packagePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create folder transfer package.");
    }

    output.write(kFolderPackageMagic.data(),
                 static_cast<std::streamsize>(kFolderPackageMagic.size()));
    writeUnsigned64LittleEndian(output,
                                static_cast<std::uint64_t>(entries.size()));
    for (const FolderPackageEntry& entry : entries) {
        writeFolderPackageEntryHeader(output, entry);
        if (entry.type != FolderPackageEntryType::File) {
            continue;
        }

        std::ifstream input(entry.sourcePath, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Failed to read folder transfer file.");
        }
        copyExactBytes(input, output, static_cast<std::uint64_t>(entry.fileSize));
        if (input.peek() != std::char_traits<char>::eof()) {
            throw std::runtime_error(
                "Folder transfer source file changed while packaging.");
        }
    }

    if (!output) {
        throw std::runtime_error("Failed to finish folder transfer package.");
    }
    return packagePath;
}

class ScopedTemporaryFile {
public:
    explicit ScopedTemporaryFile(std::filesystem::path path)
        : path_(std::move(path))
    {
    }

    ScopedTemporaryFile(const ScopedTemporaryFile&) = delete;
    ScopedTemporaryFile& operator=(const ScopedTemporaryFile&) = delete;

    ~ScopedTemporaryFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& GetPath() const { return path_; }

protected:
    std::filesystem::path path_;
};

void extractFolderTransferPackage(const std::filesystem::path& packagePath,
                                  const std::filesystem::path& targetRoot)
{
    std::ifstream input(packagePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open folder transfer package.");
    }

    std::array<char, kFolderPackageMagic.size()> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kFolderPackageMagic) {
        throw std::runtime_error("Folder transfer package header is invalid.");
    }

    std::error_code error;
    if (std::filesystem::exists(targetRoot, error)
        && !std::filesystem::is_directory(targetRoot, error)) {
        std::filesystem::remove(targetRoot, error);
        if (error) {
            throw std::runtime_error("Failed to replace folder transfer target.");
        }
    }
    std::filesystem::create_directories(targetRoot);

    const std::uint64_t entryCount = readUnsigned64LittleEndian(input);
    for (std::uint64_t index = 0; index < entryCount; ++index) {
        const int rawType = input.get();
        if (rawType == std::char_traits<char>::eof()) {
            throw std::runtime_error("Folder transfer package is truncated.");
        }
        const auto entryType =
            static_cast<FolderPackageEntryType>(
                static_cast<unsigned char>(rawType));
        const std::uint32_t pathSize = readUnsigned32LittleEndian(input);
        if (pathSize == 0
            || pathSize > kMaxFolderPackageRelativePathBytes) {
            throw std::runtime_error("Folder transfer path is invalid.");
        }

        const std::uint64_t fileSize = readUnsigned64LittleEndian(input);
        std::string pathText(pathSize, '\0');
        input.read(pathText.data(), static_cast<std::streamsize>(pathText.size()));
        if (!input) {
            throw std::runtime_error("Folder transfer package is truncated.");
        }

        const std::filesystem::path relativePath =
            filesystemPathFromUtf8String(pathText).lexically_normal();
        if (!isSafeFolderPackageRelativePath(relativePath)) {
            throw std::runtime_error("Folder transfer package contains unsafe path.");
        }

        const std::filesystem::path targetPath =
            (targetRoot / relativePath).lexically_normal();
        if (entryType == FolderPackageEntryType::Directory) {
            std::filesystem::create_directories(targetPath);
            continue;
        }
        if (entryType != FolderPackageEntryType::File) {
            throw std::runtime_error("Folder transfer package entry is invalid.");
        }

        std::filesystem::create_directories(targetPath.parent_path());
        std::ofstream output(targetPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Failed to create folder transfer file.");
        }
        copyExactBytes(input, output, fileSize);
    }
}

std::string chooseTransferFileName(
    const relaydesk::storage::ChatMessagePart& part,
    const std::filesystem::path& localPath)
{
    if (part.GetFileName().has_value() && !part.GetFileName().value().empty()) {
        return sanitizeFileName(part.GetFileName().value());
    }

    const std::string pathFileName = filesystemPathToUtf8String(
        localPath.filename());
    if (!pathFileName.empty()) {
        return pathFileName;
    }
    return sanitizeFileName(part.GetFileName().value_or("transfer.bin"));
}

relaydesk::net::TransferOfferMessage makeTransferOfferMessage(
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::ChatMessagePart& part,
    const std::string& fileName,
    std::uintmax_t fileSize)
{
    relaydesk::net::TransferOfferMessage message;
    message.SetMessageId(record.GetMessageId());
    message.SetPartId(part.GetPartId());
    message.SetTransferId(part.GetTransferId().value());
    message.SetSenderDeviceId(record.GetSenderDeviceId());
    message.SetFileName(fileName);
    message.SetFileSize(fileSize);
    message.SetImageTransfer(
        part.GetType() == relaydesk::storage::MessagePartType::Image);
    message.SetFolderTransfer(
        part.GetType() == relaydesk::storage::MessagePartType::Folder);
    if (part.GetSha256().has_value()) {
        message.SetSha256(part.GetSha256().value());
    }
    return message;
}

relaydesk::net::TransferChunkMessage makeTransferChunkMessage(
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::ChatMessagePart& part,
    std::uintmax_t offset)
{
    relaydesk::net::TransferChunkMessage message;
    message.SetMessageId(record.GetMessageId());
    message.SetPartId(part.GetPartId());
    message.SetTransferId(part.GetTransferId().value());
    message.SetOffset(offset);
    return message;
}

relaydesk::net::TransferCompleteMessage makeTransferCompleteMessage(
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::ChatMessagePart& part,
    std::uintmax_t fileSize)
{
    relaydesk::net::TransferCompleteMessage message;
    message.SetMessageId(record.GetMessageId());
    message.SetPartId(part.GetPartId());
    message.SetTransferId(part.GetTransferId().value());
    message.SetFileSize(fileSize);
    if (part.GetSha256().has_value()) {
        message.SetSha256(part.GetSha256().value());
    }
    return message;
}

relaydesk::net::AppUpdateRequestMessage makeAppUpdateRequestMessage(
    const std::string& requestId,
    const std::string& requesterDeviceId,
    int requestedAppVersion)
{
    relaydesk::net::AppUpdateRequestMessage message;
    message.SetRequestId(requestId);
    message.SetRequesterDeviceId(requesterDeviceId);
    message.SetCurrentAppVersion(relaydesk::core::kAppVersion);
    message.SetRequestedAppVersion(requestedAppVersion);
    return message;
}

relaydesk::net::AppUpdateChunkMessage makeAppUpdateChunkMessage(
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

relaydesk::net::AppUpdateCompleteMessage makeAppUpdateCompleteMessage(
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

relaydesk::net::TransferAcceptMessage makeTransferAcceptMessage(
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::ChatMessagePart& part,
    const std::string& receiverDeviceId,
    bool overwriteExisting)
{
    relaydesk::net::TransferAcceptMessage message;
    message.SetMessageId(record.GetMessageId());
    message.SetPartId(part.GetPartId());
    message.SetTransferId(part.GetTransferId().value());
    message.SetReceiverDeviceId(receiverDeviceId);
    message.SetSaveStrategy(overwriteExisting
                                ? relaydesk::net::TransferSaveStrategy::Overwrite
                                : relaydesk::net::TransferSaveStrategy::Unique);
    return message;
}

relaydesk::net::TransferRejectMessage makeTransferRejectMessage(
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::ChatMessagePart& part,
    const std::string& receiverDeviceId)
{
    relaydesk::net::TransferRejectMessage message;
    message.SetMessageId(record.GetMessageId());
    message.SetPartId(part.GetPartId());
    message.SetTransferId(part.GetTransferId().value());
    message.SetReceiverDeviceId(receiverDeviceId);
    message.SetReason("user_rejected");
    return message;
}

relaydesk::net::TransferCancelMessage makeTransferCancelMessage(
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::ChatMessagePart& part,
    const std::string& cancellerDeviceId)
{
    relaydesk::net::TransferCancelMessage message;
    message.SetMessageId(record.GetMessageId());
    message.SetPartId(part.GetPartId());
    message.SetTransferId(part.GetTransferId().value());
    message.SetCancellerDeviceId(cancellerDeviceId);
    message.SetReason("user_cancelled");
    return message;
}

PendingTransferProgressUpdate makeTransferProgressUpdate(
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    std::uintmax_t transferredSize)
{
    PendingTransferProgressUpdate update;
    update.SetMessageId(messageId);
    update.SetPartId(partId);
    update.SetTransferId(transferId);
    update.SetTransferredSize(transferredSize);
    return update;
}
#endif

} // namespace

class DiscoveryWorkerHandle {
public:
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    explicit DiscoveryWorkerHandle(
        std::unique_ptr<relaydesk::net::DiscoveryWorker> worker)
        : worker_(std::move(worker))
    {
    }

protected:
    std::unique_ptr<relaydesk::net::DiscoveryWorker> worker_;
#endif
};

class TcpPeerTransportHandle {
public:
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    explicit TcpPeerTransportHandle(
        std::unique_ptr<relaydesk::net::BoostAsioTcpPeerTransport> transport)
        : transport_(std::move(transport))
    {
    }

    std::uint16_t GetLocalPort() const
    {
        return transport_->GetLocalPort();
    }

    void sendFrameTo(const std::string& address,
                     std::uint16_t port,
                     const relaydesk::net::PeerFrame& frame)
    {
        transport_->sendFrameTo(address, port, frame);
    }

protected:
    std::unique_ptr<relaydesk::net::BoostAsioTcpPeerTransport> transport_;
#endif
};

#if defined(RELAYDESK_HAS_BOOST_ASIO)
void sendTransferPartFrames(
    TcpPeerTransportHandle& transport,
    const PeerListItem& peer,
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::ChatMessagePart& part,
    const relaydesk::storage::AppPaths& appPaths,
    const TransferProgressCallback& onProgress,
    const ::core::async::CancelToken& cancelToken)
{
    if (!part.GetTransferId().has_value() || !part.GetLocalPath().has_value()) {
        throw std::runtime_error("Transfer part is missing local file metadata.");
    }
    if (cancelToken.canceled()) {
        return;
    }

    const std::filesystem::path localPath =
        resolveLocalPath(appPaths, part.GetLocalPath().value());
    const bool folderTransfer =
        part.GetType() == relaydesk::storage::MessagePartType::Folder;
    std::unique_ptr<ScopedTemporaryFile> temporaryPayload;
    std::filesystem::path payloadPath = localPath;
    if (folderTransfer) {
        if (!std::filesystem::is_directory(localPath)) {
            throw std::runtime_error("Transfer source folder does not exist.");
        }
        payloadPath = createFolderTransferPackage(appPaths,
                                                  localPath,
                                                  part.GetTransferId().value());
        temporaryPayload = std::make_unique<ScopedTemporaryFile>(payloadPath);
    } else if (!std::filesystem::is_regular_file(localPath)) {
        throw std::runtime_error("Transfer source file does not exist.");
    }

    const std::string fileName = chooseTransferFileName(part, localPath);
    const std::uintmax_t fileSize = std::filesystem::file_size(payloadPath);
    if (cancelToken.canceled()) {
        return;
    }
    transport.sendFrameTo(
        peer.GetAddress(),
        peer.GetTcpPort(),
        relaydesk::net::makeTransferOfferFrame(
            makeTransferOfferMessage(record, part, fileName, fileSize)));

    std::ifstream input(payloadPath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open transfer source file.");
    }

    std::vector<std::uint8_t> buffer(
        static_cast<std::size_t>(kTransferChunkSize));
    std::uintmax_t offset = 0;
    while (input) {
        if (cancelToken.canceled()) {
            return;
        }
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize readSize = input.gcount();
        if (readSize <= 0) {
            break;
        }

        std::vector<std::uint8_t> chunk(
            buffer.begin(),
            buffer.begin() + readSize);
        if (cancelToken.canceled()) {
            return;
        }
        transport.sendFrameTo(
            peer.GetAddress(),
            peer.GetTcpPort(),
            relaydesk::net::makeTransferChunkFrame(
                makeTransferChunkMessage(record, part, offset),
                std::move(chunk)));
        offset += static_cast<std::uintmax_t>(readSize);
        onProgress(record.GetMessageId(),
                   part.GetPartId(),
                   part.GetTransferId().value(),
                   offset);
    }

    if (offset != fileSize) {
        throw std::runtime_error("Transfer source file changed while sending.");
    }
    if (cancelToken.canceled()) {
        return;
    }

    transport.sendFrameTo(
        peer.GetAddress(),
        peer.GetTcpPort(),
        relaydesk::net::makeTransferCompleteFrame(
            makeTransferCompleteMessage(record, part, fileSize)));
}

void sendImmediateTransferFrames(
    TcpPeerTransportHandle& transport,
    const PeerListItem& peer,
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::AppPaths& appPaths,
    const TransferProgressCallback& onProgress,
    const ::core::async::CancelToken& cancelToken)
{
    for (const auto& part : record.GetParts()) {
        if (isImmediateTransferPart(part)) {
            if (cancelToken.canceled()) {
                return;
            }
            sendTransferPartFrames(transport,
                                   peer,
                                   record,
                                   part,
                                   appPaths,
                                   onProgress,
                                   cancelToken);
        }
    }
}

void sendRequestedFileTransferFrames(
    TcpPeerTransportHandle& transport,
    const PeerListItem& peer,
    const relaydesk::storage::ChatMessageRecord& record,
    const std::string& partId,
    const std::string& transferId,
    const relaydesk::storage::AppPaths& appPaths,
    const TransferProgressCallback& onProgress,
    const ::core::async::CancelToken& cancelToken)
{
    for (const auto& part : record.GetParts()) {
        const bool partMatches = part.GetPartId() == partId
            || (part.GetTransferId().has_value()
                && part.GetTransferId().value() == transferId);
        if (partMatches && isManualTransferInvitePart(part)) {
            sendTransferPartFrames(transport,
                                   peer,
                                   record,
                                   part,
                                   appPaths,
                                   onProgress,
                                   cancelToken);
            return;
        }
    }

    throw std::runtime_error("Accepted transfer part was not found.");
}

void sendAppUpdatePackageFrames(
    TcpPeerTransportHandle& transport,
    const PeerListItem& peer,
    const relaydesk::net::AppUpdateRequestMessage& request,
    const relaydesk::storage::AppPaths& appPaths,
    const ::core::async::CancelToken& cancelToken)
{
    if (request.GetRequestedAppVersion() > relaydesk::core::kAppVersion) {
        throw std::runtime_error("Requested app update version is not available.");
    }
    if (request.GetCurrentAppVersion() >= relaydesk::core::kAppVersion) {
        return;
    }
    if (cancelToken.canceled()) {
        return;
    }

    const std::filesystem::path sourcePath = appPaths.GetExecutablePath();
    if (!std::filesystem::is_regular_file(sourcePath)) {
        throw std::runtime_error("App update source executable does not exist.");
    }

    const std::string fileName = appUpdatePackageFileName(appPaths);
    const std::uintmax_t fileSize = std::filesystem::file_size(sourcePath);
    std::ifstream input(sourcePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open app update source executable.");
    }

    std::vector<std::uint8_t> buffer(
        static_cast<std::size_t>(kTransferChunkSize));
    std::uintmax_t offset = 0;
    while (input) {
        if (cancelToken.canceled()) {
            return;
        }
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize readSize = input.gcount();
        if (readSize <= 0) {
            break;
        }

        std::vector<std::uint8_t> chunk(buffer.begin(),
                                        buffer.begin() + readSize);
        if (cancelToken.canceled()) {
            return;
        }
        transport.sendFrameTo(
            peer.GetAddress(),
            peer.GetTcpPort(),
            relaydesk::net::makeAppUpdateChunkFrame(
                makeAppUpdateChunkMessage(request.GetRequestId(), offset, fileSize),
                std::move(chunk)));
        offset += static_cast<std::uintmax_t>(readSize);
    }

    if (offset != fileSize) {
        throw std::runtime_error(
            "App update source executable changed while sending.");
    }
    if (cancelToken.canceled()) {
        return;
    }

    transport.sendFrameTo(
        peer.GetAddress(),
        peer.GetTcpPort(),
        relaydesk::net::makeAppUpdateCompleteFrame(
            makeAppUpdateCompleteMessage(request.GetRequestId(),
                                         relaydesk::core::kAppVersion,
                                         fileName,
                                         fileSize)));
}
#endif

void LocalUserSummary::SetDisplayName(std::string displayName)
{
    displayName_ = std::move(displayName);
}

void LocalUserSummary::SetHostName(std::string hostName)
{
    hostName_ = std::move(hostName);
}

void LocalUserSummary::SetDeviceId(std::string deviceId)
{
    deviceId_ = std::move(deviceId);
}

void PeerListItem::SetDeviceId(std::string deviceId)
{
    deviceId_ = std::move(deviceId);
}

void PeerListItem::SetDisplayName(std::string displayName)
{
    displayName_ = std::move(displayName);
}

void PeerListItem::SetHostName(std::string hostName)
{
    hostName_ = std::move(hostName);
}

void PeerListItem::SetAddress(std::string address)
{
    address_ = std::move(address);
}

void PeerListItem::SetLastSeenAt(std::string lastSeenAt)
{
    lastSeenAt_ = std::move(lastSeenAt);
}

void PeerListItem::SetLastConversationAt(std::string lastConversationAt)
{
    lastConversationAt_ = std::move(lastConversationAt);
}

void PeerListItem::SetTcpPort(std::uint16_t tcpPort)
{
    tcpPort_ = tcpPort;
}

void PeerListItem::SetAppVersion(int appVersion)
{
    appVersion_ = appVersion;
}

void PeerListItem::SetLastOnlineSignalAt(
    std::chrono::steady_clock::time_point signalAt)
{
    lastOnlineSignalAt_ = signalAt;
}

void PeerListItem::SetOnline(bool online)
{
    online_ = online;
}

PendingPeerProfile::PendingPeerProfile(
    relaydesk::storage::PeerProfile profile,
    bool online)
    : profile_(std::move(profile)),
      online_(online)
{
}

RelayDeskRuntime::RelayDeskRuntime()
{
    initialize();
}

RelayDeskRuntime::~RelayDeskRuntime()
{
    launchScheduledAppUpdateOnExit();
}

std::optional<PeerListItem> RelayDeskRuntime::GetSelectedPeer() const
{
    const auto selected = std::find_if(
        peers_.begin(),
        peers_.end(),
        [this](const PeerListItem& peer) {
            return peer.GetDeviceId() == selectedPeerDeviceId_;
        });
    if (selected == peers_.end()) {
        return std::nullopt;
    }

    return *selected;
}

std::optional<AppUpdatePrompt> RelayDeskRuntime::GetAppUpdatePrompt()
{
    std::lock_guard lock(pendingAppUpdateMutex_);
    return appUpdatePrompt_;
}

std::optional<PeerListItem> RelayDeskRuntime::findPeerByDeviceId(
    const std::string& peerDeviceId) const
{
    const auto existing = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&peerDeviceId](const PeerListItem& peer) {
            return peer.GetDeviceId() == peerDeviceId;
        });
    if (existing != peers_.end()) {
        return *existing;
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        return makePeerListItem(
            relaydesk::storage::loadPeerProfile(appPaths, peerDeviceId),
            false);
    } catch (const std::exception& error) {
        logDiagnostic("runtime.peer.lookup_failed device_id=" + peerDeviceId
                      + " message=" + error.what());
    }

    return std::nullopt;
}

void RelayDeskRuntime::maybeOfferAppUpdateFromPeer(const PeerListItem& peer)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    if (!tcpPeerTransport_
        || !peer.GetOnline()
        || peer.GetDeviceId().empty()
        || peer.GetDeviceId() == localUser_.GetDeviceId()
        || peer.GetAppVersion() <= relaydesk::core::kAppVersion
        || peer.GetAddress().empty()
        || peer.GetAddress() == "unknown"
        || peer.GetTcpPort() == 0) {
        return;
    }

    {
        std::lock_guard lock(pendingAppUpdateMutex_);
        const auto dismissedVersion =
            dismissedAppUpdateVersions_.find(peer.GetDeviceId());
        if (dismissedVersion != dismissedAppUpdateVersions_.end()
            && dismissedVersion->second >= peer.GetAppVersion()) {
            return;
        }
        const auto requestedVersion =
            requestedAppUpdateVersions_.find(peer.GetDeviceId());
        if (requestedVersion != requestedAppUpdateVersions_.end()
            && requestedVersion->second >= peer.GetAppVersion()) {
            return;
        }
        if (appUpdatePrompt_.has_value()
            && appUpdatePrompt_->GetSourceDeviceId() == peer.GetDeviceId()
            && appUpdatePrompt_->GetAppVersion() >= peer.GetAppVersion()) {
            return;
        }
        if (appUpdatePrompt_.has_value()
            && appUpdatePrompt_->GetState() == AppUpdatePromptState::Downloading) {
            return;
        }

        AppUpdatePrompt prompt;
        prompt.SetSourceDeviceId(peer.GetDeviceId());
        prompt.SetSourceDisplayName(peer.GetDisplayName().empty()
                                        ? peer.GetHostName()
                                        : peer.GetDisplayName());
        prompt.SetFileName("relaydesk.exe");
        prompt.SetAppVersion(peer.GetAppVersion());
        prompt.SetState(AppUpdatePromptState::Available);
        appUpdatePrompt_ = std::move(prompt);
    }

    logDiagnostic("runtime.update.available device_id=" + peer.GetDeviceId()
                  + " peer_app_version="
                  + std::to_string(peer.GetAppVersion())
                  + " local_app_version="
                  + std::to_string(relaydesk::core::kAppVersion));
    requestUiRefresh();
#else
    (void)peer;
#endif
}

void RelayDeskRuntime::requestAppUpdateFromPeer(const PeerListItem& peer,
                                                AppUpdateInstallMode installMode)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    const std::string requestId = relaydesk::core::createUuidV4();
    const int appVersion = peer.GetAppVersion();
    try {
        if (!tcpPeerTransport_
            || !peer.GetOnline()
            || peer.GetAddress().empty()
            || peer.GetAddress() == "unknown"
            || peer.GetTcpPort() == 0
            || appVersion <= relaydesk::core::kAppVersion) {
            throw std::runtime_error("App update source peer is not available.");
        }

        const auto appPaths = relaydesk::storage::createAppPaths();
        const std::string fileName = appUpdatePackageFileName(appPaths);
        const std::filesystem::path tempFilePath =
            makeAppUpdateTempFilePath(appPaths, requestId);
        std::filesystem::create_directories(tempFilePath.parent_path());
        {
            std::ofstream output(tempFilePath,
                                 std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error(
                    "Failed to create incoming app update file.");
            }
        }

        PendingIncomingAppUpdate update;
        update.SetRequestId(requestId);
        update.SetSourceDeviceId(peer.GetDeviceId());
        update.SetAppVersion(appVersion);
        update.SetFileName(fileName);
        update.SetTempFilePath(tempFilePath);
        update.SetInstallMode(installMode);
        update.SetStartedAt(std::chrono::steady_clock::now());
        {
            std::lock_guard lock(pendingAppUpdateMutex_);
            pendingIncomingAppUpdates_[requestId] = update;
            requestedAppUpdateVersions_[peer.GetDeviceId()] = appVersion;
            if (appUpdatePrompt_.has_value()
                && appUpdatePrompt_->GetSourceDeviceId() == peer.GetDeviceId()
                && appUpdatePrompt_->GetAppVersion() == appVersion) {
                appUpdatePrompt_->SetState(AppUpdatePromptState::Downloading);
                appUpdatePrompt_->SetInstallMode(installMode);
                appUpdatePrompt_->SetFileName(fileName);
                appUpdatePrompt_->SetExpectedSize(0);
                appUpdatePrompt_->SetReceivedSize(0);
                appUpdatePrompt_->SetBytesPerSecond(0.0);
                appUpdatePrompt_->SetErrorMessage({});
            }
        }
        requestUiRefresh();

        const relaydesk::net::PeerFrame frame =
            relaydesk::net::makeAppUpdateRequestFrame(
                makeAppUpdateRequestMessage(requestId,
                                            localUser_.GetDeviceId(),
                                            appVersion));
        const bool accepted = ::core::async::runOnce(
            "relaydesk.update.request." + requestId,
            [this, peer, frame] {
                try {
                    if (!tcpPeerTransport_) {
                        return ::core::async::failure(
                            "TCP peer transport is not available.");
                    }
                    tcpPeerTransport_->sendFrameTo(peer.GetAddress(),
                                                   peer.GetTcpPort(),
                                                   frame);
                    return ::core::async::success();
                } catch (const std::exception& error) {
                    return ::core::async::failure(error.what());
                }
            },
            [this,
             requestId,
             peerDeviceId = peer.GetDeviceId(),
             appVersion](
                const ::core::async::Result<void>& result) {
                if (result.ok) {
                    return;
                }
                {
                    std::lock_guard lock(pendingAppUpdateMutex_);
                    pendingIncomingAppUpdates_.erase(requestId);
                    requestedAppUpdateVersions_.erase(peerDeviceId);
                }
                markAppUpdateFailed(peerDeviceId, appVersion, result.error);
            });
        if (!accepted) {
            {
                std::lock_guard lock(pendingAppUpdateMutex_);
                pendingIncomingAppUpdates_.erase(requestId);
                requestedAppUpdateVersions_.erase(peer.GetDeviceId());
            }
            markAppUpdateFailed(peer.GetDeviceId(),
                                appVersion,
                                "更新任务未能启动。");
        }
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(pendingAppUpdateMutex_);
            pendingIncomingAppUpdates_.erase(requestId);
            requestedAppUpdateVersions_.erase(peer.GetDeviceId());
        }
        markAppUpdateFailed(peer.GetDeviceId(), appVersion, error.what());
    }
#else
    (void)peer;
    (void)installMode;
#endif
}

void RelayDeskRuntime::sendAppUpdatePackageToPeer(
    const PeerListItem& peer,
    const relaydesk::net::AppUpdateRequestMessage& request)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    if (peer.GetAddress().empty()
        || peer.GetAddress() == "unknown"
        || peer.GetTcpPort() == 0) {
        logDiagnostic("runtime.update.send_failed request_id="
                      + request.GetRequestId()
                      + " message=peer_address_unavailable");
        return;
    }

    const bool accepted = ::core::async::runOnce(
        "relaydesk.update.send." + request.GetRequestId(),
        [this, peer, request](const ::core::async::CancelToken& token) {
            try {
                if (!tcpPeerTransport_) {
                    return ::core::async::failure(
                        "TCP peer transport is not available.");
                }
                const auto appPaths = relaydesk::storage::createAppPaths();
                sendAppUpdatePackageFrames(*tcpPeerTransport_,
                                           peer,
                                           request,
                                           appPaths,
                                           token);
                return ::core::async::success();
            } catch (const std::exception& error) {
                return ::core::async::failure(error.what());
            }
        },
        [this,
         requestId = request.GetRequestId(),
         peerDeviceId = peer.GetDeviceId()](
            const ::core::async::Result<void>& result) {
            if (result.ok) {
                logDiagnostic("runtime.update.send_complete request_id="
                              + requestId
                              + " peer_device_id=" + peerDeviceId);
                return;
            }
            logDiagnostic("runtime.update.send_failed request_id=" + requestId
                          + " peer_device_id=" + peerDeviceId
                          + " message=" + result.error);
        });
    if (!accepted) {
        logDiagnostic("runtime.update.send_failed request_id="
                      + request.GetRequestId()
                      + " peer_device_id=" + peer.GetDeviceId()
                      + " message=task_not_accepted");
    }
#else
    (void)peer;
    (void)request;
#endif
}

void RelayDeskRuntime::completeDownloadedAppUpdate(
    const PendingIncomingAppUpdate& update)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    if (update.GetInstallMode() == AppUpdateInstallMode::InstallOnExit) {
        {
            std::lock_guard lock(pendingAppUpdateMutex_);
            scheduledAppUpdate_ = update;
            if (appUpdatePrompt_.has_value()
                && appUpdatePrompt_->GetSourceDeviceId()
                    == update.GetSourceDeviceId()
                && appUpdatePrompt_->GetAppVersion() == update.GetAppVersion()) {
                appUpdatePrompt_.reset();
            }
        }
        logDiagnostic("runtime.update.scheduled_on_exit request_id="
                      + update.GetRequestId()
                      + " app_version="
                      + std::to_string(update.GetAppVersion()));
        requestUiRefresh();
        return;
    }

    applyDownloadedAppUpdate(update, true);
#else
    (void)update;
#endif
}

void RelayDeskRuntime::applyDownloadedAppUpdate(
    const PendingIncomingAppUpdate& update,
    bool restartAfterApply)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    if (update.GetAppVersion() <= relaydesk::core::kAppVersion) {
        logDiagnostic("runtime.update.apply_ignored request_id="
                      + update.GetRequestId()
                      + " app_version="
                      + std::to_string(update.GetAppVersion()));
        return;
    }
    if (update.GetExpectedSize() == 0
        || !std::filesystem::is_regular_file(update.GetTempFilePath())
        || std::filesystem::file_size(update.GetTempFilePath())
            != update.GetExpectedSize()) {
        throw std::runtime_error("Downloaded app update file is incomplete.");
    }

    const auto appPaths = relaydesk::storage::createAppPaths();
    const std::filesystem::path scriptPath =
        makeAppUpdateScriptFilePath(appPaths, update.GetRequestId());
    writeAppUpdateScript(scriptPath,
                         appPaths.GetExecutablePath(),
                         update.GetTempFilePath(),
                         appPaths.GetWorkDirectory(),
                         restartAfterApply);
    if (!launchAppUpdateScript(scriptPath)) {
        throw std::runtime_error("Failed to launch app update script.");
    }

    logDiagnostic("runtime.update.apply_started request_id="
                  + update.GetRequestId()
                  + " app_version="
                  + std::to_string(update.GetAppVersion())
                  + " restart_after_apply="
                  + std::to_string(restartAfterApply));
#else
    (void)update;
    (void)restartAfterApply;
#endif
}

void RelayDeskRuntime::launchScheduledAppUpdateOnExit() noexcept
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    std::optional<PendingIncomingAppUpdate> update;
    {
        std::lock_guard lock(pendingAppUpdateMutex_);
        update = scheduledAppUpdate_;
        scheduledAppUpdate_.reset();
    }
    if (!update.has_value()) {
        return;
    }

    try {
        applyDownloadedAppUpdate(update.value(), false);
    } catch (const std::exception& error) {
        logDiagnostic("runtime.update.scheduled_apply_failed request_id="
                      + update->GetRequestId()
                      + " message=" + error.what());
    }
#endif
}

void RelayDeskRuntime::markAppUpdateFailed(const std::string& sourceDeviceId,
                                           int appVersion,
                                           std::string errorMessage)
{
    std::vector<std::filesystem::path> tempDirectories;
    {
        std::lock_guard lock(pendingAppUpdateMutex_);
        requestedAppUpdateVersions_.erase(sourceDeviceId);
        for (auto iterator = pendingIncomingAppUpdates_.begin();
             iterator != pendingIncomingAppUpdates_.end();) {
            const bool sameSource =
                iterator->second.GetSourceDeviceId() == sourceDeviceId;
            const bool sameVersion = iterator->second.GetAppVersion() == appVersion;
            if (sameSource && sameVersion) {
                tempDirectories.push_back(
                    iterator->second.GetTempFilePath().parent_path());
                iterator = pendingIncomingAppUpdates_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        if (appUpdatePrompt_.has_value()
            && appUpdatePrompt_->GetSourceDeviceId() == sourceDeviceId
            && appUpdatePrompt_->GetAppVersion() == appVersion) {
            appUpdatePrompt_->SetState(AppUpdatePromptState::Failed);
            appUpdatePrompt_->SetErrorMessage(std::move(errorMessage));
        }
    }

    for (const auto& directory : tempDirectories) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    logDiagnostic("runtime.update.failed device_id=" + sourceDeviceId
                  + " app_version=" + std::to_string(appVersion));
    requestUiRefresh();
}

void RelayDeskRuntime::refreshPeersIfNeeded()
{
    if (!storageAvailable_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    drainPendingPeerProfiles();
    drainPendingChatMessages();
    drainPendingTransferUpdates();
    drainPendingTransferStateUpdates();
    drainPendingTransferProgressUpdates();
    drainPendingOutgoingTransferRequests();
    if (now >= nextPeerStatusRefreshAt_) {
        refreshPeerOnlineStates();
        nextPeerStatusRefreshAt_ = now + kPeerStatusRefreshInterval;
    }
    requestPeerStatusRefresh();
}

void RelayDeskRuntime::selectPeer(std::string deviceId)
{
    const auto selected = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&deviceId](const PeerListItem& peer) {
            return peer.GetDeviceId() == deviceId;
        });
    if (selected == peers_.end()) {
        return;
    }

    setSelectedPeerDeviceId(std::move(deviceId));
}

void RelayDeskRuntime::sendMessagePartsToSelectedPeer(
    std::vector<relaydesk::storage::ChatMessagePart> parts)
{
    parts = normalizeOutgoingParts(std::move(parts));
    if (parts.empty()) {
        return;
    }

    const std::optional<PeerListItem> selectedPeer = GetSelectedPeer();
    if (!selectedPeer.has_value()) {
        return;
    }

    relaydesk::storage::ChatMessageRecord record =
        makeOutgoingMessageRecord(localUser_, selectedPeer.value(), std::move(parts));
    selectedPeerMessages_.push_back(record);
    updatePeerLastConversationAt(selectedPeer->GetDeviceId(),
                                 record.GetCreatedAt());
    requestUiRefresh();

    sendOutgoingMessageRecordToPeer(std::move(record), selectedPeer.value(), false);
}

void RelayDeskRuntime::resendSelectedPeerMessage(const std::string& messageId)
{
    const std::optional<PeerListItem> selectedPeer = GetSelectedPeer();
    if (!selectedPeer.has_value()) {
        return;
    }

    const auto message = std::find_if(
        selectedPeerMessages_.begin(),
        selectedPeerMessages_.end(),
        [&messageId](const relaydesk::storage::ChatMessageRecord& record) {
            return record.GetMessageId() == messageId;
        });
    if (message == selectedPeerMessages_.end()
        || message->GetDirection()
            != relaydesk::storage::MessageDirection::Outgoing
        || message->GetDeliveryState()
            != relaydesk::storage::DeliveryState::Failed) {
        return;
    }

    relaydesk::storage::ChatMessageRecord record =
        makeRetryMessageRecord(*message);
    *message = record;
    try {
        persistChatMessageRecord(
            selectedPeer->GetDeviceId(),
            record,
            true);
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
    requestUiRefresh();

    sendOutgoingMessageRecordToPeer(std::move(record), selectedPeer.value(), true);
}

void RelayDeskRuntime::acceptSelectedPeerFileTransfer(const std::string& messageId,
                                                      const std::string& partId,
                                                      bool overwriteExisting)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    const std::optional<PeerListItem> selectedPeer = GetSelectedPeer();
    if (!selectedPeer.has_value()) {
        return;
    }

    const auto message = std::find_if(
        selectedPeerMessages_.begin(),
        selectedPeerMessages_.end(),
        [&messageId](const relaydesk::storage::ChatMessageRecord& record) {
            return record.GetMessageId() == messageId;
        });
    if (message == selectedPeerMessages_.end()
        || message->GetDirection()
            != relaydesk::storage::MessageDirection::Incoming) {
        return;
    }

    relaydesk::storage::ChatMessageRecord record = *message;
    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    auto part = std::find_if(
        parts.begin(),
        parts.end(),
        [&partId](const relaydesk::storage::ChatMessagePart& candidate) {
            return candidate.GetPartId() == partId;
        });
    const bool folderTransfer = part != parts.end()
        && part->GetType() == relaydesk::storage::MessagePartType::Folder;
    if (part == parts.end()
        || (part->GetType() != relaydesk::storage::MessagePartType::File
            && !folderTransfer)
        || !part->GetTransferId().has_value()
        || !part->GetTransferState().has_value()
        || part->GetTransferState().value()
            != relaydesk::storage::TransferState::Offered
        || !part->GetFileName().has_value()) {
        return;
    }

    const auto appPaths = relaydesk::storage::createAppPaths();
    const std::filesystem::path desiredPath =
        makeIncomingDesiredFilePath(appPaths, part->GetFileName().value());
    const std::filesystem::path finalPath = overwriteExisting
        ? desiredPath
        : makeAvailableSiblingPath(desiredPath);
    const std::filesystem::path tempPath =
        makeIncomingTempFilePath(appPaths,
                                 part->GetTransferId().value(),
                                 part->GetFileName().value());

    PendingIncomingTransfer transfer;
    transfer.SetSenderDeviceId(selectedPeer->GetDeviceId());
    transfer.SetMessageId(record.GetMessageId());
    transfer.SetPartId(part->GetPartId());
    transfer.SetTransferId(part->GetTransferId().value());
    transfer.SetFileName(part->GetFileName().value());
    transfer.SetExpectedSize(part->GetFileSize().value_or(0));
    transfer.SetReceivedSize(0);
    transfer.SetTempFilePath(tempPath);
    transfer.SetFinalFilePath(finalPath);
    transfer.SetImageTransfer(false);
    transfer.SetFolderTransfer(folderTransfer);
    {
        std::lock_guard lock(pendingTransferMutex_);
        pendingIncomingTransfers_[transfer.GetTransferId()] = transfer;
    }

    part->SetTransferState(relaydesk::storage::TransferState::Transferring);
    part->SetTransferredSize(0);
    part->SetLocalPath(makeWorkRelativePath(appPaths, finalPath));
    record.SetParts(std::move(parts));
    *message = record;
    try {
        persistChatMessageRecord(selectedPeer->GetDeviceId(), record, true);
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
    requestUiRefresh();

    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferAcceptFrame(
            makeTransferAcceptMessage(record,
                                      *std::find_if(
                                          record.GetParts().begin(),
                                          record.GetParts().end(),
                                          [&partId](const auto& candidate) {
                                              return candidate.GetPartId()
                                                  == partId;
                                          }),
                                      localUser_.GetDeviceId(),
                                      overwriteExisting));
    const bool accepted = ::core::async::runOnce(
        "relaydesk.transfer.accept." + transfer.GetTransferId(),
        [this, peer = selectedPeer.value(), frame] {
            try {
                if (!tcpPeerTransport_) {
                    return ::core::async::failure(
                        "TCP peer transport is not available.");
                }
                tcpPeerTransport_->sendFrameTo(peer.GetAddress(),
                                               peer.GetTcpPort(),
                                               frame);
                return ::core::async::success();
            } catch (const std::exception& error) {
                return ::core::async::failure(error.what());
            }
        },
        [this,
         peerDeviceId = selectedPeer->GetDeviceId(),
         messageId,
         partId,
         transferId = transfer.GetTransferId()](
            const ::core::async::Result<void>& result) {
            if (result.ok) {
                return;
            }
            logDiagnostic("runtime.transfer.accept_failed message=" + result.error);
            PendingTransferStateUpdate update;
            update.SetPeerDeviceId(peerDeviceId);
            update.SetMessageId(messageId);
            update.SetPartId(partId);
            update.SetTransferId(transferId);
            update.SetTransferState(relaydesk::storage::TransferState::Failed);
            enqueueTransferStateUpdate(std::move(update));
        });
    if (!accepted) {
        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(selectedPeer->GetDeviceId());
        update.SetMessageId(messageId);
        update.SetPartId(partId);
        update.SetTransferId(transfer.GetTransferId());
        update.SetTransferState(relaydesk::storage::TransferState::Failed);
        enqueueTransferStateUpdate(std::move(update));
    }
#else
    (void)messageId;
    (void)partId;
    (void)overwriteExisting;
#endif
}

void RelayDeskRuntime::rejectSelectedPeerFileTransfer(const std::string& messageId,
                                                      const std::string& partId)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    const std::optional<PeerListItem> selectedPeer = GetSelectedPeer();
    if (!selectedPeer.has_value()) {
        return;
    }

    const auto message = std::find_if(
        selectedPeerMessages_.begin(),
        selectedPeerMessages_.end(),
        [&messageId](const relaydesk::storage::ChatMessageRecord& record) {
            return record.GetMessageId() == messageId;
        });
    if (message == selectedPeerMessages_.end()
        || message->GetDirection()
            != relaydesk::storage::MessageDirection::Incoming) {
        return;
    }

    relaydesk::storage::ChatMessageRecord record = *message;
    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    auto part = std::find_if(
        parts.begin(),
        parts.end(),
        [&partId](const relaydesk::storage::ChatMessagePart& candidate) {
            return candidate.GetPartId() == partId;
        });
    if (part == parts.end()
        || part->GetType() != relaydesk::storage::MessagePartType::File
        || !part->GetTransferId().has_value()
        || !part->GetTransferState().has_value()
        || part->GetTransferState().value()
            != relaydesk::storage::TransferState::Offered) {
        return;
    }

    const std::string transferId = part->GetTransferId().value();
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferRejectFrame(
            makeTransferRejectMessage(record,
                                      *part,
                                      localUser_.GetDeviceId()));
    part->SetTransferState(relaydesk::storage::TransferState::Rejected);
    record.SetParts(std::move(parts));
    *message = record;
    try {
        persistChatMessageRecord(selectedPeer->GetDeviceId(), record, true);
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
    {
        std::lock_guard lock(pendingTransferMutex_);
        pendingIncomingTransfers_.erase(transferId);
    }
    requestUiRefresh();

    const bool accepted = ::core::async::runOnce(
        "relaydesk.transfer.reject." + transferId,
        [this, peer = selectedPeer.value(), frame] {
            try {
                if (!tcpPeerTransport_) {
                    return ::core::async::failure(
                        "TCP peer transport is not available.");
                }
                tcpPeerTransport_->sendFrameTo(peer.GetAddress(),
                                               peer.GetTcpPort(),
                                               frame);
                return ::core::async::success();
            } catch (const std::exception& error) {
                return ::core::async::failure(error.what());
            }
        },
        [this](const ::core::async::Result<void>& result) {
            if (!result.ok) {
                logDiagnostic("runtime.transfer.reject_failed message="
                              + result.error);
            }
        });
    if (!accepted) {
        logDiagnostic("runtime.transfer.reject_failed message=task_not_accepted");
    }
#else
    (void)messageId;
    (void)partId;
#endif
}

void RelayDeskRuntime::cancelSelectedPeerFileTransfer(const std::string& messageId,
                                                      const std::string& partId)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    const std::optional<PeerListItem> selectedPeer = GetSelectedPeer();
    if (!selectedPeer.has_value()) {
        return;
    }

    const auto message = std::find_if(
        selectedPeerMessages_.begin(),
        selectedPeerMessages_.end(),
        [&messageId](const relaydesk::storage::ChatMessageRecord& record) {
            return record.GetMessageId() == messageId;
        });
    if (message == selectedPeerMessages_.end()) {
        return;
    }

    relaydesk::storage::ChatMessageRecord record = *message;
    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    auto part = std::find_if(
        parts.begin(),
        parts.end(),
        [&partId](const relaydesk::storage::ChatMessagePart& candidate) {
            return candidate.GetPartId() == partId;
        });
    if (part == parts.end()
        || part->GetType() != relaydesk::storage::MessagePartType::File
        || !part->GetTransferId().has_value()
        || !part->GetTransferState().has_value()
        || !isCancellableFileTransferState(part->GetTransferState().value())) {
        return;
    }

    const relaydesk::storage::TransferState transferState =
        part->GetTransferState().value();
    if (record.GetDirection() == relaydesk::storage::MessageDirection::Incoming
        && transferState != relaydesk::storage::TransferState::Transferring) {
        return;
    }

    const std::string transferId = part->GetTransferId().value();
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferCancelFrame(
            makeTransferCancelMessage(record,
                                      *part,
                                      localUser_.GetDeviceId()));

    part->SetTransferState(relaydesk::storage::TransferState::Cancelled);
    record.SetParts(std::move(parts));
    *message = record;
    try {
        persistChatMessageRecord(selectedPeer->GetDeviceId(), record, true);
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
    requestUiRefresh();

    if (record.GetDirection() == relaydesk::storage::MessageDirection::Outgoing) {
        (void)::core::async::cancel("relaydesk.transfer.send." + transferId);
    } else {
        std::lock_guard lock(pendingTransferMutex_);
        pendingIncomingTransfers_.erase(transferId);
    }

    const bool accepted = ::core::async::runOnce(
        "relaydesk.transfer.cancel." + transferId,
        [this, peer = selectedPeer.value(), frame] {
            try {
                if (!tcpPeerTransport_) {
                    return ::core::async::failure(
                        "TCP peer transport is not available.");
                }
                tcpPeerTransport_->sendFrameTo(peer.GetAddress(),
                                               peer.GetTcpPort(),
                                               frame);
                return ::core::async::success();
            } catch (const std::exception& error) {
                return ::core::async::failure(error.what());
            }
        },
        [this](const ::core::async::Result<void>& result) {
            if (result.ok) {
                return;
            }
            logDiagnostic("runtime.transfer.cancel_failed message="
                          + result.error);
        });
    if (!accepted) {
        logDiagnostic("runtime.transfer.cancel_failed message=task_not_accepted");
    }
#else
    (void)messageId;
    (void)partId;
#endif
}

void RelayDeskRuntime::sendOutgoingMessageRecordToPeer(
    relaydesk::storage::ChatMessageRecord record,
    const PeerListItem& peer,
    bool replaceExistingRecord)
{
    const std::string peerDeviceId = peer.GetDeviceId();
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    const std::string messageId = record.GetMessageId();
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeChatMessageFrame(record);
    const bool accepted = ::core::async::runOnce(
        "relaydesk.chat.send." + messageId,
        [this, peer, frame, record](const ::core::async::CancelToken& token) {
            try {
                if (!tcpPeerTransport_) {
                    return ::core::async::failure(
                        "TCP peer transport is not available.");
                }
                if (token.canceled()) {
                    return ::core::async::success();
                }
                tcpPeerTransport_->sendFrameTo(peer.GetAddress(),
                                               peer.GetTcpPort(),
                                               frame);
                const auto appPaths = relaydesk::storage::createAppPaths();
                sendImmediateTransferFrames(*tcpPeerTransport_,
                                            peer,
                                            record,
                                            appPaths,
                                            [this](const std::string& messageId,
                                                   const std::string& partId,
                                                   const std::string& transferId,
                                                   std::uintmax_t transferredSize) {
                                                enqueueTransferProgressUpdate(
                                                    makeTransferProgressUpdate(
                                                        messageId,
                                                        partId,
                                                        transferId,
                                                        transferredSize));
                                            },
                                            token);
                return ::core::async::success();
            } catch (const std::exception& error) {
                return ::core::async::failure(error.what());
            }
        },
        [this,
         peerDeviceId,
         replaceExistingRecord,
         record](
            const ::core::async::Result<void>& result) mutable {
            record.SetDeliveryState(result.ok
                                        ? relaydesk::storage::DeliveryState::Sent
                                        : relaydesk::storage::DeliveryState::Failed);
            record = recordAfterOutgoingChatSend(std::move(record), result.ok);
            if (!result.ok) {
                logDiagnostic("runtime.chat.send_failed message=" + result.error);
            }
            try {
                persistChatMessageRecord(peerDeviceId,
                                         record,
                                         replaceExistingRecord);
            } catch (const std::exception& error) {
                setStartupError(error.what());
            }
            updateSelectedPeerMessageRecord(record);
            requestUiRefresh();
        });
    if (accepted) {
        return;
    }
#else
#endif

    record.SetDeliveryState(relaydesk::storage::DeliveryState::Failed);
    record = recordWithTransferState(
        std::move(record),
        relaydesk::storage::TransferState::Failed);
    try {
        persistChatMessageRecord(peerDeviceId,
                                 record,
                                 replaceExistingRecord);
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
    updateSelectedPeerMessageRecord(record);
    requestUiRefresh();
}

void RelayDeskRuntime::sendTextMessageToSelectedPeer(std::string text)
{
    std::vector<relaydesk::storage::ChatMessagePart> parts;
    parts.push_back(makeTextPart(std::move(text)));
    sendMessagePartsToSelectedPeer(std::move(parts));
}

void RelayDeskRuntime::startAppUpdate(AppUpdateInstallMode installMode)
{
    std::optional<AppUpdatePrompt> prompt;
    {
        std::lock_guard lock(pendingAppUpdateMutex_);
        if (!appUpdatePrompt_.has_value()
            || appUpdatePrompt_->GetState() == AppUpdatePromptState::Downloading) {
            return;
        }
        prompt = appUpdatePrompt_;
    }

    const std::optional<PeerListItem> peer =
        findPeerByDeviceId(prompt->GetSourceDeviceId());
    if (!peer.has_value()) {
        markAppUpdateFailed(prompt->GetSourceDeviceId(),
                            prompt->GetAppVersion(),
                            "更新来源设备不可用。");
        return;
    }

    requestAppUpdateFromPeer(peer.value(), installMode);
}

void RelayDeskRuntime::dismissAppUpdatePrompt()
{
    std::vector<std::filesystem::path> tempDirectories;
    {
        std::lock_guard lock(pendingAppUpdateMutex_);
        if (appUpdatePrompt_.has_value()) {
            dismissedAppUpdateVersions_[appUpdatePrompt_->GetSourceDeviceId()] =
                appUpdatePrompt_->GetAppVersion();
            requestedAppUpdateVersions_.erase(appUpdatePrompt_->GetSourceDeviceId());
            for (auto iterator = pendingIncomingAppUpdates_.begin();
                 iterator != pendingIncomingAppUpdates_.end();) {
                if (iterator->second.GetSourceDeviceId()
                    == appUpdatePrompt_->GetSourceDeviceId()) {
                    tempDirectories.push_back(
                        iterator->second.GetTempFilePath().parent_path());
                    iterator = pendingIncomingAppUpdates_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        }
        appUpdatePrompt_.reset();
    }

    for (const auto& directory : tempDirectories) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    requestUiRefresh();
}

void RelayDeskRuntime::initialize()
{
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(appPaths);
        diagnosticLogFilePath_ = makeDiscoveryLogFilePath(appPaths);
        logDiagnostic("runtime.initialize.begin");
        std::error_code currentPathError;
        std::filesystem::current_path(appPaths.GetWorkDirectory(),
                                      currentPathError);
        if (currentPathError) {
            logDiagnostic("runtime.current_path_failed message="
                          + currentPathError.message());
        }

        const std::string hostName = relaydesk::platform::getComputerNameUtf8();
        const auto identity = relaydesk::storage::loadOrCreateLocalIdentity(
            appPaths,
            hostName,
            getStableDeviceId());

        localUser_.SetDisplayName(identity.GetDisplayName());
        localUser_.SetHostName(identity.GetHostName());
        localUser_.SetDeviceId(identity.GetDeviceId());
        storageAvailable_ = true;
        logDiagnostic("runtime.identity.loaded device_id=" + identity.GetDeviceId()
                      + " host_name=" + identity.GetHostName()
                      + " display_name=" + identity.GetDisplayName());

#if defined(RELAYDESK_HAS_BOOST_ASIO)
        auto tcpTransport =
            std::make_unique<relaydesk::net::BoostAsioTcpPeerTransport>(
                relaydesk::net::kDefaultAdvertisedTcpPort);
        tcpTransport->SetFrameCallback(
            [this](relaydesk::net::PeerFrame frame,
                   std::string,
                   std::uint16_t) {
                try {
                    handleIncomingPeerFrame(std::move(frame));
                } catch (const std::exception& error) {
                    logDiagnostic(std::string("runtime.tcp.frame_error message=")
                                  + error.what());
                }
            });
        tcpTransport->SetErrorCallback(
            [this](std::string message) {
                logDiagnostic("runtime.tcp.error message=" + message);
            });
        tcpTransport->start();
        const std::uint16_t tcpPort = tcpTransport->GetLocalPort();
        tcpPeerTransport_ =
            std::make_unique<TcpPeerTransportHandle>(std::move(tcpTransport));
        logDiagnostic("runtime.tcp.started port=" + std::to_string(tcpPort));

        relaydesk::net::DiscoveryServiceConfig serviceConfig;
        serviceConfig.SetAdvertisedTcpPort(tcpPort);
        serviceConfig.SetAppVersion(relaydesk::core::kAppVersion);
        relaydesk::net::DiscoveryService discoveryService(
            appPaths,
            identity,
            serviceConfig);
        relaydesk::net::DiscoveryWorkerEvents workerEvents;
        workerEvents.SetPeerStoredCallback(
            [this](relaydesk::storage::PeerProfile profile,
                   std::string announcementType) {
                enqueuePeerProfile(
                    std::move(profile),
                    announcementType
                        != relaydesk::net::kDiscoveryAnnouncementTypeOffline);
            });
        auto discoveryWorker = std::make_unique<relaydesk::net::DiscoveryWorker>(
            std::move(discoveryService),
            makeDiscoveryWorkerConfig(),
            std::move(workerEvents));
        discoveryWorker->start();
        discoveryStarted_ = true;
        discoveryUdpPort_ = discoveryWorker->GetLocalUdpPort();
        logDiagnostic("runtime.discovery.started udp_port="
                      + std::to_string(discoveryUdpPort_));
        discoveryWorker_ =
            std::make_unique<DiscoveryWorkerHandle>(std::move(discoveryWorker));
#endif

        refreshPeers();
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
}

void RelayDeskRuntime::refreshPeers()
{
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        const std::vector<relaydesk::storage::PeerProfile> profiles =
            relaydesk::storage::loadPeerProfiles(appPaths);
        std::vector<PeerListItem> nextPeers;
        nextPeers.reserve(profiles.size());
        for (const auto& profile : profiles) {
            PeerListItem item = makePeerListItem(profile, false);
            item.SetLastConversationAt(
                loadPeerLastConversationAtOrEmpty(appPaths, item.GetDeviceId()));
            nextPeers.push_back(std::move(item));
        }
        peers_ = std::move(nextPeers);
        sortPeers();
        logDiagnostic("runtime.peer.refresh_from_storage count="
                      + std::to_string(peers_.size()));
        syncSelectedPeer();
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
}

void RelayDeskRuntime::setSelectedPeerDeviceId(std::string deviceId)
{
    if (selectedPeerDeviceId_ == deviceId) {
        return;
    }

    selectedPeerDeviceId_ = std::move(deviceId);
    loadSelectedPeerMessages();
}

void RelayDeskRuntime::loadSelectedPeerMessages()
{
    if (selectedPeerDeviceId_.empty() || !storageAvailable_) {
        selectedPeerMessages_.clear();
        return;
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        const auto result =
            relaydesk::storage::loadChatHistory(appPaths, selectedPeerDeviceId_);
        selectedPeerMessages_ = result.GetRecords();
        if (result.GetSkippedLineCount() > 0) {
            logDiagnostic("runtime.chat.history_skipped count="
                          + std::to_string(result.GetSkippedLineCount()));
        }
    } catch (const std::exception& error) {
        selectedPeerMessages_.clear();
        setStartupError(error.what());
    }
}

void RelayDeskRuntime::enqueueIncomingChatMessage(
    relaydesk::storage::ChatMessageRecord record)
{
    const std::string peerDeviceId =
        peerDeviceIdForRecord(record, localUser_.GetDeviceId());
    {
        std::lock_guard lock(pendingChatMutex_);
        pendingChatMessages_.push_back(std::move(record));
    }

    logDiagnostic("runtime.chat.enqueue_incoming peer_device_id=" + peerDeviceId);
    requestUiRefresh();
}

#if defined(RELAYDESK_HAS_BOOST_ASIO)
void RelayDeskRuntime::handleIncomingPeerFrame(relaydesk::net::PeerFrame frame)
{
    switch (frame.GetType()) {
    case relaydesk::net::PeerFrameType::ChatMessage:
        enqueueIncomingChatMessage(makeIncomingRecordForLocalDevice(
            relaydesk::net::parseChatMessageFrame(frame)));
        return;
    case relaydesk::net::PeerFrameType::AppUpdateRequest: {
        const relaydesk::net::AppUpdateRequestMessage request =
            relaydesk::net::parseAppUpdateRequestFrame(frame);
        if (request.GetRequestedAppVersion() > relaydesk::core::kAppVersion) {
            logDiagnostic("runtime.update.request_ignored request_id="
                          + request.GetRequestId()
                          + " reason=requested_version_unavailable");
            return;
        }
        if (request.GetCurrentAppVersion() >= relaydesk::core::kAppVersion) {
            logDiagnostic("runtime.update.request_ignored request_id="
                          + request.GetRequestId()
                          + " reason=requester_not_outdated");
            return;
        }

        const std::optional<PeerListItem> peer =
            findPeerByDeviceId(request.GetRequesterDeviceId());
        if (!peer.has_value()) {
            logDiagnostic("runtime.update.request_ignored request_id="
                          + request.GetRequestId()
                          + " reason=peer_not_found");
            return;
        }

        sendAppUpdatePackageToPeer(peer.value(), request);
        return;
    }
    case relaydesk::net::PeerFrameType::AppUpdateChunk: {
        const relaydesk::net::AppUpdateChunkMessage chunk =
            relaydesk::net::parseAppUpdateChunkFrame(frame);
        PendingIncomingAppUpdate update;
        try {
            {
                std::lock_guard lock(pendingAppUpdateMutex_);
                const auto existing =
                    pendingIncomingAppUpdates_.find(chunk.GetRequestId());
                if (existing == pendingIncomingAppUpdates_.end()) {
                    throw std::runtime_error(
                        "Incoming app update chunk has no request.");
                }
                if (existing->second.GetReceivedSize() != chunk.GetOffset()) {
                    throw std::runtime_error(
                        "Incoming app update chunk offset is not sequential.");
                }
                if (existing->second.GetExpectedSize() != 0
                    && existing->second.GetExpectedSize() != chunk.GetFileSize()) {
                    throw std::runtime_error(
                        "Incoming app update chunk file size changed.");
                }
                update = existing->second;
                update.SetExpectedSize(chunk.GetFileSize());
            }

            std::ofstream output(
                update.GetTempFilePath(),
                std::ios::binary | std::ios::app);
            if (!output) {
                throw std::runtime_error("Failed to write incoming app update chunk.");
            }
            const auto& body = frame.GetBody();
            output.write(reinterpret_cast<const char*>(body.data()),
                         static_cast<std::streamsize>(body.size()));
            if (!output) {
                throw std::runtime_error("Failed to append incoming app update chunk.");
            }

            const std::uintmax_t receivedSize =
                update.GetReceivedSize() + body.size();
            if (receivedSize > chunk.GetFileSize()) {
                throw std::runtime_error(
                    "Incoming app update chunk exceeds expected size.");
            }
            {
                std::lock_guard lock(pendingAppUpdateMutex_);
                const auto existing =
                    pendingIncomingAppUpdates_.find(chunk.GetRequestId());
                if (existing != pendingIncomingAppUpdates_.end()) {
                    existing->second.SetExpectedSize(chunk.GetFileSize());
                    existing->second.SetReceivedSize(receivedSize);
                    const auto startedAt = existing->second.GetStartedAt();
                    const double elapsedSeconds =
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - startedAt)
                            .count();
                    const double bytesPerSecond =
                        elapsedSeconds > 0.0
                            ? static_cast<double>(receivedSize) / elapsedSeconds
                            : 0.0;
                    if (appUpdatePrompt_.has_value()
                        && appUpdatePrompt_->GetSourceDeviceId()
                            == existing->second.GetSourceDeviceId()
                        && appUpdatePrompt_->GetAppVersion()
                            == existing->second.GetAppVersion()) {
                        appUpdatePrompt_->SetState(
                            AppUpdatePromptState::Downloading);
                        appUpdatePrompt_->SetFileName(
                            existing->second.GetFileName());
                        appUpdatePrompt_->SetExpectedSize(chunk.GetFileSize());
                        appUpdatePrompt_->SetReceivedSize(receivedSize);
                        appUpdatePrompt_->SetBytesPerSecond(bytesPerSecond);
                    }
                }
            }
            requestUiRefresh();
            logDiagnostic("runtime.update.chunk_received request_id="
                          + chunk.GetRequestId()
                          + " received_size=" + std::to_string(receivedSize));
        } catch (const std::exception& error) {
            if (!update.GetSourceDeviceId().empty()) {
                markAppUpdateFailed(update.GetSourceDeviceId(),
                                    update.GetAppVersion(),
                                    error.what());
            }
            throw;
        }
        return;
    }
    case relaydesk::net::PeerFrameType::AppUpdateComplete: {
        const relaydesk::net::AppUpdateCompleteMessage complete =
            relaydesk::net::parseAppUpdateCompleteFrame(frame);
        PendingIncomingAppUpdate update;
        {
            std::lock_guard lock(pendingAppUpdateMutex_);
            const auto existing =
                pendingIncomingAppUpdates_.find(complete.GetRequestId());
            if (existing == pendingIncomingAppUpdates_.end()) {
                throw std::runtime_error("Incoming app update complete has no request.");
            }
            if (existing->second.GetReceivedSize() != complete.GetFileSize()) {
                throw std::runtime_error("Incoming app update size does not match request.");
            }
            update = existing->second;
            update.SetAppVersion(complete.GetAppVersion());
            update.SetFileName(complete.GetFileName());
            update.SetExpectedSize(complete.GetFileSize());
            pendingIncomingAppUpdates_.erase(existing);
            requestedAppUpdateVersions_.erase(update.GetSourceDeviceId());
        }

        try {
            if (update.GetReceivedSize() != complete.GetFileSize()) {
                throw std::runtime_error("Incoming app update payload is incomplete.");
            }

            completeDownloadedAppUpdate(update);
        } catch (const std::exception& error) {
            markAppUpdateFailed(update.GetSourceDeviceId(),
                                update.GetAppVersion(),
                                error.what());
            throw;
        }
        return;
    }
    case relaydesk::net::PeerFrameType::TransferOffer: {
        const relaydesk::net::TransferOfferMessage offer =
            relaydesk::net::parseTransferOfferFrame(frame);
        const auto appPaths = relaydesk::storage::createAppPaths();
        std::optional<PendingIncomingTransfer> existingTransfer;
        {
            std::lock_guard lock(pendingTransferMutex_);
            const auto existing =
                pendingIncomingTransfers_.find(offer.GetTransferId());
            if (existing != pendingIncomingTransfers_.end()) {
                existingTransfer = existing->second;
            }
        }
        const std::filesystem::path tempFilePath =
            makeIncomingTempFilePath(
                appPaths,
                offer.GetTransferId(),
                offer.GetFileName());
        std::filesystem::create_directories(tempFilePath.parent_path());
        {
            std::ofstream output(tempFilePath, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("Failed to create incoming transfer file.");
            }
        }

        PendingIncomingTransfer transfer;
        transfer.SetSenderDeviceId(offer.GetSenderDeviceId());
        transfer.SetMessageId(offer.GetMessageId());
        transfer.SetPartId(offer.GetPartId());
        transfer.SetTransferId(offer.GetTransferId());
        transfer.SetFileName(offer.GetFileName());
        transfer.SetExpectedSize(offer.GetFileSize());
        transfer.SetReceivedSize(0);
        transfer.SetTempFilePath(tempFilePath);
        if (existingTransfer.has_value()) {
            transfer.SetFinalFilePath(existingTransfer->GetFinalFilePath());
            transfer.SetFolderTransfer(existingTransfer->GetFolderTransfer());
        }
        transfer.SetImageTransfer(offer.GetImageTransfer());
        if (offer.GetFolderTransfer()) {
            transfer.SetFolderTransfer(true);
        }
        {
            std::lock_guard lock(pendingTransferMutex_);
            pendingIncomingTransfers_[offer.GetTransferId()] = std::move(transfer);
        }

        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(offer.GetSenderDeviceId());
        update.SetMessageId(offer.GetMessageId());
        update.SetPartId(offer.GetPartId());
        update.SetTransferId(offer.GetTransferId());
        update.SetTransferState(relaydesk::storage::TransferState::Transferring);
        enqueueTransferStateUpdate(std::move(update));
        return;
    }
    case relaydesk::net::PeerFrameType::TransferAccept: {
        const relaydesk::net::TransferAcceptMessage accept =
            relaydesk::net::parseTransferAcceptFrame(frame);
        PendingOutgoingTransferRequest request;
        request.SetReceiverDeviceId(accept.GetReceiverDeviceId());
        request.SetMessageId(accept.GetMessageId());
        request.SetPartId(accept.GetPartId());
        request.SetTransferId(accept.GetTransferId());
        enqueueOutgoingTransferRequest(std::move(request));
        return;
    }
    case relaydesk::net::PeerFrameType::TransferReject: {
        const relaydesk::net::TransferRejectMessage reject =
            relaydesk::net::parseTransferRejectFrame(frame);
        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(reject.GetReceiverDeviceId());
        update.SetMessageId(reject.GetMessageId());
        update.SetPartId(reject.GetPartId());
        update.SetTransferId(reject.GetTransferId());
        update.SetTransferState(relaydesk::storage::TransferState::Rejected);
        enqueueTransferStateUpdate(std::move(update));
        return;
    }
    case relaydesk::net::PeerFrameType::TransferCancel: {
        const relaydesk::net::TransferCancelMessage cancel =
            relaydesk::net::parseTransferCancelFrame(frame);
        {
            std::lock_guard lock(pendingTransferMutex_);
            pendingIncomingTransfers_.erase(cancel.GetTransferId());
        }
        (void)::core::async::cancel(
            "relaydesk.transfer.send." + cancel.GetTransferId());

        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(cancel.GetCancellerDeviceId());
        update.SetMessageId(cancel.GetMessageId());
        update.SetPartId(cancel.GetPartId());
        update.SetTransferId(cancel.GetTransferId());
        update.SetTransferState(relaydesk::storage::TransferState::Cancelled);
        enqueueTransferStateUpdate(std::move(update));
        return;
    }
    case relaydesk::net::PeerFrameType::TransferChunk: {
        const relaydesk::net::TransferChunkMessage chunk =
            relaydesk::net::parseTransferChunkFrame(frame);
        PendingIncomingTransfer transfer;
        {
            std::lock_guard lock(pendingTransferMutex_);
            const auto existing =
                pendingIncomingTransfers_.find(chunk.GetTransferId());
            if (existing == pendingIncomingTransfers_.end()) {
                throw std::runtime_error("Incoming transfer chunk has no offer.");
            }
            if (existing->second.GetReceivedSize() != chunk.GetOffset()) {
                throw std::runtime_error(
                    "Incoming transfer chunk offset is not sequential.");
            }
            transfer = existing->second;
        }

        std::ofstream output(
            transfer.GetTempFilePath(),
            std::ios::binary | std::ios::app);
        if (!output) {
            throw std::runtime_error("Failed to write incoming transfer chunk.");
        }
        const auto& body = frame.GetBody();
        output.write(reinterpret_cast<const char*>(body.data()),
                     static_cast<std::streamsize>(body.size()));
        if (!output) {
            throw std::runtime_error("Failed to append incoming transfer chunk.");
        }

        const std::uintmax_t receivedSize =
            transfer.GetReceivedSize() + body.size();
        {
            std::lock_guard lock(pendingTransferMutex_);
            const auto existing =
                pendingIncomingTransfers_.find(chunk.GetTransferId());
            if (existing != pendingIncomingTransfers_.end()) {
                existing->second.SetReceivedSize(receivedSize);
            }
        }
        enqueueTransferProgressUpdate(
            makeTransferProgressUpdate(transfer.GetMessageId(),
                                       transfer.GetPartId(),
                                       transfer.GetTransferId(),
                                       receivedSize));
        return;
    }
    case relaydesk::net::PeerFrameType::TransferComplete: {
        const relaydesk::net::TransferCompleteMessage complete =
            relaydesk::net::parseTransferCompleteFrame(frame);
        PendingIncomingTransfer transfer;
        {
            std::lock_guard lock(pendingTransferMutex_);
            const auto existing =
                pendingIncomingTransfers_.find(complete.GetTransferId());
            if (existing == pendingIncomingTransfers_.end()) {
                throw std::runtime_error("Incoming transfer complete has no offer.");
            }
            transfer = existing->second;
        }

        if (transfer.GetReceivedSize() != transfer.GetExpectedSize()
            || complete.GetFileSize() != transfer.GetExpectedSize()) {
            throw std::runtime_error("Incoming transfer size does not match offer.");
        }

        const auto appPaths = relaydesk::storage::createAppPaths();
        std::filesystem::path finalFilePath;
        std::optional<std::string> sha256;
        if (transfer.GetFolderTransfer()) {
            finalFilePath = transfer.GetFinalFilePath().empty()
                ? makeIncomingFinalFilePath(appPaths, transfer)
                : transfer.GetFinalFilePath();
            extractFolderTransferPackage(transfer.GetTempFilePath(), finalFilePath);
            std::error_code error;
            std::filesystem::remove(transfer.GetTempFilePath(), error);
        } else if (transfer.GetImageTransfer()) {
            try {
                const std::optional<StoredImageAttachment> storedImage =
                    storePreviewableImageAttachment(appPaths,
                                                    transfer.GetTempFilePath(),
                                                    transfer.GetFileName(),
                                                    true);
                if (storedImage.has_value()) {
                    finalFilePath = storedImage->GetImagePath();
                    sha256 = storedImage->GetSha256();
                }
            } catch (const std::exception&) {
            }
        }

        if (finalFilePath.empty()) {
            finalFilePath = transfer.GetFinalFilePath().empty()
                ? makeIncomingFinalFilePath(appPaths, transfer)
                : transfer.GetFinalFilePath();
            std::filesystem::create_directories(finalFilePath.parent_path());
            std::error_code error;
            if (std::filesystem::exists(finalFilePath, error) && !error) {
                std::filesystem::remove(finalFilePath, error);
                if (error) {
                    throw std::runtime_error(
                        "Failed to overwrite incoming transfer file.");
                }
            }
            error.clear();
            std::filesystem::rename(transfer.GetTempFilePath(), finalFilePath, error);
            if (error) {
                throw std::runtime_error("Failed to finalize incoming transfer file.");
            }
        }

        PendingTransferUpdate update;
        update.SetPeerDeviceId(transfer.GetSenderDeviceId());
        update.SetMessageId(transfer.GetMessageId());
        update.SetPartId(transfer.GetPartId());
        update.SetTransferId(transfer.GetTransferId());
        update.SetFileName(transfer.GetFileName());
        update.SetFileSize(transfer.GetExpectedSize());
        update.SetLocalPath(makeWorkRelativePath(appPaths, finalFilePath));
        if (sha256.has_value()) {
            update.SetSha256(sha256.value());
        }
        update.SetTransferState(relaydesk::storage::TransferState::Completed);
        enqueueTransferUpdate(std::move(update));

        {
            std::lock_guard lock(pendingTransferMutex_);
            pendingIncomingTransfers_.erase(complete.GetTransferId());
        }
        return;
    }
    default:
        throw std::runtime_error("Unsupported peer frame type.");
    }
}
#endif

void RelayDeskRuntime::enqueueTransferUpdate(PendingTransferUpdate update)
{
    {
        std::lock_guard lock(pendingTransferUpdateMutex_);
        pendingTransferUpdates_.push_back(std::move(update));
    }
    requestUiRefresh();
}

void RelayDeskRuntime::drainPendingChatMessages()
{
    std::vector<relaydesk::storage::ChatMessageRecord> pendingMessages;
    {
        std::lock_guard lock(pendingChatMutex_);
        pendingMessages.swap(pendingChatMessages_);
    }

    if (pendingMessages.empty()) {
        return;
    }

    for (const auto& message : pendingMessages) {
        try {
            appendSelectedPeerMessage(
                peerDeviceIdForRecord(message, localUser_.GetDeviceId()),
                message);
        } catch (const std::exception& error) {
            setStartupError(error.what());
        }
    }
}

void RelayDeskRuntime::drainPendingTransferUpdates()
{
    std::vector<PendingTransferUpdate> updates;
    {
        std::lock_guard lock(pendingTransferUpdateMutex_);
        updates.swap(pendingTransferUpdates_);
    }

    if (updates.empty()) {
        return;
    }

    for (const auto& update : updates) {
        try {
            if (!updateChatMessageTransferPart(update)) {
                std::lock_guard lock(pendingTransferUpdateMutex_);
                pendingTransferUpdates_.push_back(update);
            }
        } catch (const std::exception& error) {
            setStartupError(error.what());
        }
    }
}

void RelayDeskRuntime::enqueueOutgoingTransferRequest(
    PendingOutgoingTransferRequest request)
{
    {
        std::lock_guard lock(pendingOutgoingTransferRequestMutex_);
        pendingOutgoingTransferRequests_.push_back(std::move(request));
    }
    requestUiRefresh();
}

void RelayDeskRuntime::drainPendingOutgoingTransferRequests()
{
    std::vector<PendingOutgoingTransferRequest> requests;
    {
        std::lock_guard lock(pendingOutgoingTransferRequestMutex_);
        requests.swap(pendingOutgoingTransferRequests_);
    }

    for (const auto& request : requests) {
        try {
            sendOutgoingTransferRequest(request);
        } catch (const std::exception& error) {
            setStartupError(error.what());
        }
    }
}

void RelayDeskRuntime::sendOutgoingTransferRequest(
    const PendingOutgoingTransferRequest& request)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    const auto peer = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&request](const PeerListItem& item) {
            return item.GetDeviceId() == request.GetReceiverDeviceId();
        });
    if (peer == peers_.end() || peer->GetAddress().empty()) {
        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(request.GetReceiverDeviceId());
        update.SetMessageId(request.GetMessageId());
        update.SetPartId(request.GetPartId());
        update.SetTransferId(request.GetTransferId());
        update.SetTransferState(relaydesk::storage::TransferState::Failed);
        enqueueTransferStateUpdate(std::move(update));
        return;
    }

    std::optional<relaydesk::storage::ChatMessageRecord> record;
    if (request.GetReceiverDeviceId() == selectedPeerDeviceId_) {
        const auto selectedMessage = std::find_if(
            selectedPeerMessages_.begin(),
            selectedPeerMessages_.end(),
            [&request](const relaydesk::storage::ChatMessageRecord& candidate) {
                return candidate.GetMessageId() == request.GetMessageId();
            });
        if (selectedMessage != selectedPeerMessages_.end()) {
            record = *selectedMessage;
        }
    }
    if (!record.has_value()) {
        const auto appPaths = relaydesk::storage::createAppPaths();
        const auto history =
            relaydesk::storage::loadChatHistory(appPaths,
                                                request.GetReceiverDeviceId());
        for (const auto& candidate : history.GetRecords()) {
            if (candidate.GetMessageId() == request.GetMessageId()) {
                record = candidate;
                break;
            }
        }
    }

    if (!record.has_value()
        || record->GetDirection()
            != relaydesk::storage::MessageDirection::Outgoing) {
        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(request.GetReceiverDeviceId());
        update.SetMessageId(request.GetMessageId());
        update.SetPartId(request.GetPartId());
        update.SetTransferId(request.GetTransferId());
        update.SetTransferState(relaydesk::storage::TransferState::Failed);
        enqueueTransferStateUpdate(std::move(update));
        return;
    }
    if (!hasTransferPartState(record.value(),
                              request.GetMessageId(),
                              request.GetPartId(),
                              request.GetTransferId(),
                              relaydesk::storage::TransferState::Offered)) {
        return;
    }

    relaydesk::storage::ChatMessageRecord transferringRecord = record.value();
    if (!applyTransferPartStateUpdate(
            transferringRecord,
            request.GetMessageId(),
            request.GetPartId(),
            request.GetTransferId(),
            relaydesk::storage::TransferState::Transferring)) {
        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(request.GetReceiverDeviceId());
        update.SetMessageId(request.GetMessageId());
        update.SetPartId(request.GetPartId());
        update.SetTransferId(request.GetTransferId());
        update.SetTransferState(relaydesk::storage::TransferState::Failed);
        enqueueTransferStateUpdate(std::move(update));
        return;
    }

    try {
        persistChatMessageRecord(request.GetReceiverDeviceId(),
                                 transferringRecord,
                                 true);
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
    updateSelectedPeerMessageRecord(transferringRecord);
    requestUiRefresh();

    const bool accepted = ::core::async::runOnce(
        "relaydesk.transfer.send." + request.GetTransferId(),
        [this, peer = *peer, transferringRecord, request](
            const ::core::async::CancelToken& token) {
            try {
                if (!tcpPeerTransport_) {
                    return ::core::async::failure(
                        "TCP peer transport is not available.");
                }
                if (token.canceled()) {
                    return ::core::async::success();
                }
                const auto appPaths = relaydesk::storage::createAppPaths();
                sendRequestedFileTransferFrames(*tcpPeerTransport_,
                                                peer,
                                                transferringRecord,
                                                request.GetPartId(),
                                                request.GetTransferId(),
                                                appPaths,
                                                [this](
                                                    const std::string& messageId,
                                                    const std::string& partId,
                                                    const std::string& transferId,
                                                    std::uintmax_t transferredSize) {
                                                    enqueueTransferProgressUpdate(
                                                        makeTransferProgressUpdate(
                                                            messageId,
                                                            partId,
                                                            transferId,
                                                            transferredSize));
                                                },
                                                token);
                return ::core::async::success();
            } catch (const std::exception& error) {
                return ::core::async::failure(error.what());
            }
        },
        [this, request](const ::core::async::Result<void>& result) {
            if (!result.ok) {
                logDiagnostic("runtime.transfer.send_failed message="
                              + result.error);
            }
            PendingTransferStateUpdate update;
            update.SetPeerDeviceId(request.GetReceiverDeviceId());
            update.SetMessageId(request.GetMessageId());
            update.SetPartId(request.GetPartId());
            update.SetTransferId(request.GetTransferId());
            update.SetTransferState(result.ok
                                        ? relaydesk::storage::TransferState::Completed
                                        : relaydesk::storage::TransferState::Failed);
            enqueueTransferStateUpdate(std::move(update));
        });
    if (!accepted) {
        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(request.GetReceiverDeviceId());
        update.SetMessageId(request.GetMessageId());
        update.SetPartId(request.GetPartId());
        update.SetTransferId(request.GetTransferId());
        update.SetTransferState(relaydesk::storage::TransferState::Failed);
        enqueueTransferStateUpdate(std::move(update));
    }
#else
    (void)request;
#endif
}

void RelayDeskRuntime::enqueueTransferStateUpdate(PendingTransferStateUpdate update)
{
    {
        std::lock_guard lock(pendingTransferStateUpdateMutex_);
        pendingTransferStateUpdates_.push_back(std::move(update));
    }
    requestUiRefresh();
}

void RelayDeskRuntime::drainPendingTransferStateUpdates()
{
    std::vector<PendingTransferStateUpdate> updates;
    {
        std::lock_guard lock(pendingTransferStateUpdateMutex_);
        updates.swap(pendingTransferStateUpdates_);
    }

    for (const auto& update : updates) {
        try {
            if (!updateChatMessageTransferState(update)) {
                std::lock_guard lock(pendingTransferStateUpdateMutex_);
                pendingTransferStateUpdates_.push_back(update);
            }
        } catch (const std::exception& error) {
            setStartupError(error.what());
        }
    }
}

void RelayDeskRuntime::enqueueTransferProgressUpdate(
    PendingTransferProgressUpdate update)
{
    {
        std::lock_guard lock(pendingTransferProgressUpdateMutex_);
        pendingTransferProgressUpdates_.push_back(std::move(update));
    }
    requestUiRefresh();
}

void RelayDeskRuntime::drainPendingTransferProgressUpdates()
{
    std::vector<PendingTransferProgressUpdate> updates;
    {
        std::lock_guard lock(pendingTransferProgressUpdateMutex_);
        updates.swap(pendingTransferProgressUpdates_);
    }

    bool updated = false;
    for (const auto& update : updates) {
        if (!updateChatMessageTransferProgress(update)) {
            std::lock_guard lock(pendingTransferProgressUpdateMutex_);
            pendingTransferProgressUpdates_.push_back(update);
        } else {
            updated = true;
        }
    }
    if (updated) {
        requestUiRefresh();
    }
}

void RelayDeskRuntime::appendSelectedPeerMessage(
    const std::string& peerDeviceId,
    const relaydesk::storage::ChatMessageRecord& record)
{
    const auto appPaths = relaydesk::storage::createAppPaths();
    relaydesk::storage::appendChatMessage(appPaths, peerDeviceId, record);
    if (peerDeviceId == selectedPeerDeviceId_) {
        selectedPeerMessages_.push_back(record);
    }
    updatePeerLastConversationAt(peerDeviceId, record.GetCreatedAt());
}

void RelayDeskRuntime::persistChatMessageRecord(
    const std::string& peerDeviceId,
    const relaydesk::storage::ChatMessageRecord& record,
    bool replaceExistingRecord)
{
    const auto appPaths = relaydesk::storage::createAppPaths();
    if (replaceExistingRecord
        && relaydesk::storage::replaceChatMessage(appPaths, peerDeviceId, record)) {
        updatePeerLastConversationAt(peerDeviceId, record.GetCreatedAt());
        return;
    }

    relaydesk::storage::appendChatMessage(appPaths, peerDeviceId, record);
    updatePeerLastConversationAt(peerDeviceId, record.GetCreatedAt());
}

bool RelayDeskRuntime::updateChatMessageTransferPart(
    const PendingTransferUpdate& update)
{
    bool updatedPendingMessage = false;
    {
        std::lock_guard lock(pendingChatMutex_);
        for (auto& message : pendingChatMessages_) {
            if (applyTransferPartUpdate(message,
                                        update.GetMessageId(),
                                        update.GetPartId(),
                                        update.GetTransferId(),
                                        update.GetFileName(),
                                        update.GetFileSize(),
                                        update.GetLocalPath(),
                                        update.GetSha256(),
                                        update.GetTransferState())) {
                updatedPendingMessage = true;
                break;
            }
        }
    }

    std::optional<relaydesk::storage::ChatMessageRecord> recordToPersist;
    if (!updatedPendingMessage) {
        for (auto& message : selectedPeerMessages_) {
            if (applyTransferPartUpdate(message,
                                        update.GetMessageId(),
                                        update.GetPartId(),
                                        update.GetTransferId(),
                                        update.GetFileName(),
                                        update.GetFileSize(),
                                        update.GetLocalPath(),
                                        update.GetSha256(),
                                        update.GetTransferState())) {
                recordToPersist = message;
                break;
            }
        }
    }

    if (!updatedPendingMessage && !recordToPersist.has_value()) {
        const auto appPaths = relaydesk::storage::createAppPaths();
        const auto history =
            relaydesk::storage::loadChatHistory(appPaths, update.GetPeerDeviceId());
        for (auto message : history.GetRecords()) {
            if (applyTransferPartUpdate(message,
                                        update.GetMessageId(),
                                        update.GetPartId(),
                                        update.GetTransferId(),
                                        update.GetFileName(),
                                        update.GetFileSize(),
                                        update.GetLocalPath(),
                                        update.GetSha256(),
                                        update.GetTransferState())) {
                recordToPersist = std::move(message);
                break;
            }
        }
    }

    if (recordToPersist.has_value()) {
        const auto appPaths = relaydesk::storage::createAppPaths();
        (void)relaydesk::storage::replaceChatMessage(appPaths,
                                                     update.GetPeerDeviceId(),
                                                     recordToPersist.value());
    }

    const bool applied = updatedPendingMessage || recordToPersist.has_value();
    if (applied) {
        requestUiRefresh();
    }
    return applied;
}

bool RelayDeskRuntime::updateChatMessageTransferState(
    const PendingTransferStateUpdate& update)
{
    const bool markDeliveryFailed =
        update.GetTransferState() == relaydesk::storage::TransferState::Failed;
    const bool markDeliverySent =
        update.GetTransferState() == relaydesk::storage::TransferState::Rejected
        || update.GetTransferState()
            == relaydesk::storage::TransferState::Cancelled;
    bool updatedPendingMessage = false;
    {
        std::lock_guard lock(pendingChatMutex_);
        for (auto& message : pendingChatMessages_) {
            if (applyTransferPartStateUpdate(message,
                                             update.GetMessageId(),
                                             update.GetPartId(),
                                             update.GetTransferId(),
                                             update.GetTransferState())) {
                updatedPendingMessage = true;
                break;
            }
        }
    }

    std::optional<relaydesk::storage::ChatMessageRecord> recordToPersist;
    if (!updatedPendingMessage) {
        for (auto& message : selectedPeerMessages_) {
            if (applyTransferPartStateUpdate(message,
                                             update.GetMessageId(),
                                             update.GetPartId(),
                                             update.GetTransferId(),
                                             update.GetTransferState())) {
                if (message.GetDirection()
                    == relaydesk::storage::MessageDirection::Outgoing) {
                    if (markDeliveryFailed) {
                        message.SetDeliveryState(
                            relaydesk::storage::DeliveryState::Failed);
                    } else if (markDeliverySent) {
                        message.SetDeliveryState(
                            relaydesk::storage::DeliveryState::Sent);
                    }
                }
                recordToPersist = message;
                break;
            }
        }
    }

    if (!updatedPendingMessage && !recordToPersist.has_value()) {
        const auto appPaths = relaydesk::storage::createAppPaths();
        const auto history =
            relaydesk::storage::loadChatHistory(appPaths, update.GetPeerDeviceId());
        for (auto message : history.GetRecords()) {
            if (applyTransferPartStateUpdate(message,
                                             update.GetMessageId(),
                                             update.GetPartId(),
                                             update.GetTransferId(),
                                             update.GetTransferState())) {
                if (message.GetDirection()
                    == relaydesk::storage::MessageDirection::Outgoing) {
                    if (markDeliveryFailed) {
                        message.SetDeliveryState(
                            relaydesk::storage::DeliveryState::Failed);
                    } else if (markDeliverySent) {
                        message.SetDeliveryState(
                            relaydesk::storage::DeliveryState::Sent);
                    }
                }
                recordToPersist = std::move(message);
                break;
            }
        }
    }

    if (recordToPersist.has_value()) {
        const auto appPaths = relaydesk::storage::createAppPaths();
        (void)relaydesk::storage::replaceChatMessage(appPaths,
                                                     update.GetPeerDeviceId(),
                                                     recordToPersist.value());
    }

    const bool applied = updatedPendingMessage || recordToPersist.has_value();
    if (applied) {
        requestUiRefresh();
    }
    return applied;
}

bool RelayDeskRuntime::updateChatMessageTransferProgress(
    const PendingTransferProgressUpdate& update)
{
    bool updatedPendingMessage = false;
    {
        std::lock_guard lock(pendingChatMutex_);
        for (auto& message : pendingChatMessages_) {
            if (applyTransferPartProgressUpdate(message,
                                                update.GetMessageId(),
                                                update.GetPartId(),
                                                update.GetTransferId(),
                                                update.GetTransferredSize())) {
                updatedPendingMessage = true;
                break;
            }
        }
    }

    bool updatedSelectedMessage = false;
    for (auto& message : selectedPeerMessages_) {
        if (applyTransferPartProgressUpdate(message,
                                            update.GetMessageId(),
                                            update.GetPartId(),
                                            update.GetTransferId(),
                                            update.GetTransferredSize())) {
            updatedSelectedMessage = true;
            break;
        }
    }

    return updatedPendingMessage || updatedSelectedMessage;
}

void RelayDeskRuntime::updateSelectedPeerMessageRecord(
    const relaydesk::storage::ChatMessageRecord& record)
{
    const std::string& messageId = record.GetMessageId();
    const auto message = std::find_if(
        selectedPeerMessages_.begin(),
        selectedPeerMessages_.end(),
        [&messageId](const relaydesk::storage::ChatMessageRecord& record) {
            return record.GetMessageId() == messageId;
        });
    if (message == selectedPeerMessages_.end()) {
        return;
    }

    *message = record;
}

void RelayDeskRuntime::enqueuePeerProfile(
    relaydesk::storage::PeerProfile profile,
    bool online)
{
    const std::string deviceId = profile.GetDeviceId();
    const std::string address = choosePeerAddress(profile);
    const std::string lastSeenAt = profile.GetLastSeenAt();
    std::size_t pendingCount = 0;
    {
        std::lock_guard lock(pendingPeerMutex_);
        pendingPeerProfiles_.emplace_back(std::move(profile), online);
        pendingCount = pendingPeerProfiles_.size();
    }

    logDiagnostic("runtime.peer.enqueue device_id=" + deviceId
                  + " address=" + address
                  + " online=" + std::to_string(online)
                  + " last_seen=" + lastSeenAt
                  + " pending_count=" + std::to_string(pendingCount));
    requestUiRefresh();
}

void RelayDeskRuntime::drainPendingPeerProfiles()
{
    std::vector<PendingPeerProfile> pendingProfiles;
    {
        std::lock_guard lock(pendingPeerMutex_);
        pendingProfiles.swap(pendingPeerProfiles_);
    }

    if (pendingProfiles.empty()) {
        return;
    }

    logDiagnostic("runtime.peer.drain count="
                  + std::to_string(pendingProfiles.size()));
    const auto now = std::chrono::steady_clock::now();
    for (const auto& pendingProfile : pendingProfiles) {
        applyPeerProfile(pendingProfile.GetProfile(),
                         pendingProfile.GetOnline(),
                         now);
    }
    syncSelectedPeer();
}

void RelayDeskRuntime::applyPeerProfile(
    const relaydesk::storage::PeerProfile& profile,
    bool online,
    std::chrono::steady_clock::time_point now)
{
    const auto peerCountBeforeCleanup = peers_.size();
    peers_.erase(
        std::remove_if(
            peers_.begin(),
            peers_.end(),
            [&profile](const PeerListItem& peer) {
                return isStalePeerForProfile(peer, profile);
            }),
        peers_.end());
    const auto stalePeerCount = peerCountBeforeCleanup - peers_.size();

    const auto existing = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&profile](const PeerListItem& peer) {
            return peer.GetDeviceId() == profile.GetDeviceId();
        });
    if (existing != peers_.end()
        && isStaleOnlineProfile(*existing, profile, online)) {
        logDiagnostic("runtime.peer.apply action=ignored_stale_online device_id="
                      + profile.GetDeviceId()
                      + " incoming_last_seen=" + profile.GetLastSeenAt()
                      + " current_last_seen=" + existing->GetLastSeenAt());
        return;
    }

    std::string lastConversationAt;
    if (existing != peers_.end()) {
        lastConversationAt = existing->GetLastConversationAt();
    } else {
        const auto appPaths = relaydesk::storage::createAppPaths();
        lastConversationAt =
            loadPeerLastConversationAtOrEmpty(appPaths, profile.GetDeviceId());
    }

    PeerListItem item = makePeerListItem(profile, online);
    item.SetLastConversationAt(std::move(lastConversationAt));
    if (online) {
        item.SetLastOnlineSignalAt(now);
    }
    if (existing == peers_.end()) {
        peers_.push_back(item);
        sortPeers();
        logDiagnostic("runtime.peer.apply action=added device_id="
                      + profile.GetDeviceId()
                      + " address=" + item.GetAddress()
                      + " online=" + std::to_string(online)
                      + " app_version="
                      + std::to_string(item.GetAppVersion())
                      + " last_seen=" + item.GetLastSeenAt()
                      + " stale_removed="
                      + std::to_string(stalePeerCount));
        maybeOfferAppUpdateFromPeer(item);
        return;
    }

    const bool wasOnline = existing->GetOnline();
    const std::string previousLastSeenAt = existing->GetLastSeenAt();
    *existing = item;
    sortPeers();
    logDiagnostic("runtime.peer.apply action=updated device_id="
                  + profile.GetDeviceId()
                  + " address=" + item.GetAddress()
                  + " online=" + std::to_string(online)
                  + " app_version=" + std::to_string(item.GetAppVersion())
                  + " was_online=" + std::to_string(wasOnline)
                  + " incoming_last_seen=" + item.GetLastSeenAt()
                  + " previous_last_seen=" + previousLastSeenAt
                  + " stale_removed=" + std::to_string(stalePeerCount));
    maybeOfferAppUpdateFromPeer(item);
}

void RelayDeskRuntime::refreshPeerOnlineStates()
{
    const auto now = std::chrono::steady_clock::now();
    bool sortNeeded = false;
    for (auto& peer : peers_) {
        const bool wasOnline = peer.GetOnline();
        const bool online = isPeerOnline(peer.GetLastOnlineSignalAt(), now);
        peer.SetOnline(online);
        if (wasOnline != online) {
            sortNeeded = true;
            logDiagnostic("runtime.peer.online_changed device_id="
                          + peer.GetDeviceId()
                          + " online=" + std::to_string(online));
        }
    }

    if (sortNeeded) {
        sortPeers();
    }
}

std::string RelayDeskRuntime::loadPeerLastConversationAtOrEmpty(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& peerDeviceId) const
{
    try {
        return loadPeerLastConversationAt(appPaths, peerDeviceId);
    } catch (const std::exception& error) {
        logDiagnostic("runtime.peer.history_load_failed device_id="
                      + peerDeviceId
                      + " message=" + error.what());
    }

    return {};
}

void RelayDeskRuntime::sortPeers()
{
    std::stable_sort(
        peers_.begin(),
        peers_.end(),
        [](const PeerListItem& left, const PeerListItem& right) {
            return isPeerListItemBefore(left, right);
        });
}

void RelayDeskRuntime::updatePeerLastConversationAt(
    const std::string& peerDeviceId,
    const std::string& lastConversationAt)
{
    if (peerDeviceId.empty() || lastConversationAt.empty()) {
        return;
    }

    const auto peer = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&peerDeviceId](const PeerListItem& item) {
            return item.GetDeviceId() == peerDeviceId;
        });
    if (peer == peers_.end()) {
        return;
    }

    if (peer->GetLastConversationAt() >= lastConversationAt) {
        return;
    }

    peer->SetLastConversationAt(lastConversationAt);
    sortPeers();
}

void RelayDeskRuntime::syncSelectedPeer()
{
    if (peers_.empty()) {
        setSelectedPeerDeviceId({});
        return;
    }

    const auto selected = std::find_if(
        peers_.begin(),
        peers_.end(),
        [this](const PeerListItem& peer) {
            return peer.GetDeviceId() == selectedPeerDeviceId_;
        });
    if (selected == peers_.end()) {
        setSelectedPeerDeviceId(peers_.front().GetDeviceId());
    }
}

void RelayDeskRuntime::setStartupError(std::string errorMessage)
{
    logDiagnostic("runtime.error message=" + errorMessage);
    startupErrorMessage_ = std::move(errorMessage);
}

void RelayDeskRuntime::requestUiRefresh()
{
    bool expected = false;
    if (!uiRefreshPending_.compare_exchange_strong(expected, true)) {
        return;
    }

    const bool accepted = ::core::async::restart(
        "relaydesk.discovery.refresh",
        [] {
            return ::core::async::success();
        },
        [this](const ::core::async::Result<void>&) {
            uiRefreshPending_.store(false);
        });
    if (!accepted) {
        uiRefreshPending_.store(false);
    }
}

void RelayDeskRuntime::requestPeerStatusRefresh()
{
    bool expected = false;
    if (!peerStatusRefreshPending_.compare_exchange_strong(expected, true)) {
        return;
    }

    const bool accepted = ::core::async::restart(
        "relaydesk.peer.status.refresh",
        [](const ::core::async::CancelToken& token) {
            const auto waitUntil =
                std::chrono::steady_clock::now() + kPeerStatusRefreshInterval;
            while (!token.canceled()
                   && std::chrono::steady_clock::now() < waitUntil) {
                std::this_thread::sleep_for(50ms);
            }
            return ::core::async::success();
        },
        [this](const ::core::async::Result<void>&) {
            peerStatusRefreshPending_.store(false);
        });
    if (!accepted) {
        peerStatusRefreshPending_.store(false);
    }
}

void RelayDeskRuntime::logDiagnostic(const std::string& message) const noexcept
{
    (void)message;
    if constexpr (!discoveryTraceEnabled()) {
        return;
    }

    if (diagnosticLogFilePath_.empty()) {
        return;
    }

    relaydesk::core::appendDiagnosticLogLine(diagnosticLogFilePath_, message);
}

RelayDeskRuntime& getRelayDeskRuntime()
{
    static RelayDeskRuntime runtime;
    return runtime;
}

}
