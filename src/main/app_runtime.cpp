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
#endif

namespace relaydesk::runtime {
namespace {

using namespace std::chrono_literals;

constexpr const char* kDiscoveryLogFileName = "discovery.log";
constexpr std::size_t kSelectedPeerMessagePageSize = 30;
constexpr auto kPeerOnlineTimeout = 10s;
constexpr auto kPeerStatusRefreshInterval = 1s;
#if defined(RELAYDESK_HAS_BOOST_ASIO)
constexpr std::uintmax_t kTransferChunkSize = 1024u * 1024u;
constexpr const char* kAppUpdateTempDirectoryName = "updates";
constexpr const char* kAppUpdateLogFileName = "apply-update.log";
constexpr const char* kAppUpdateHelperFileName = "RelayDeskUpdateHelper.exe";
constexpr const wchar_t* kAppUpdateApplyArgument = L"--relaydesk-apply-update";
constexpr const wchar_t* kAppUpdateTargetArgument = L"--target";
constexpr const wchar_t* kAppUpdatePayloadArgument = L"--payload";
constexpr const wchar_t* kAppUpdatePidArgument = L"--pid";
constexpr const wchar_t* kAppUpdateStartDirectoryArgument = L"--start-directory";
constexpr const wchar_t* kAppUpdateLogArgument = L"--log";
constexpr const wchar_t* kAppUpdateRestartArgument = L"--restart";
constexpr std::size_t kMaxFolderPackageRelativePathBytes = 32u * 1024u;
using TransferProgressCallback = std::function<void(const std::string&,
                                                    const std::string&,
                                                    const std::string&,
                                                    std::uintmax_t)>;
using TransferPreparedCallback = std::function<void(const std::string&,
                                                    const std::string&,
                                                    const std::string&,
                                                    const std::string&,
                                                    std::uintmax_t,
                                                    const std::string&)>;
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
    if (profile.GetAppVersion() > peer.GetAppVersion()) {
        return false;
    }

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
    item.SetUnreadMessageCount(profile.GetUnreadMessageCount());
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

std::string loadPeerLastConversationAt(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& peerDeviceId)
{
    return relaydesk::storage::loadLatestChatMessageCreatedAt(appPaths,
                                                              peerDeviceId);
}

#if defined(RELAYDESK_HAS_BOOST_ASIO)
relaydesk::net::DiscoveryWorkerConfig makeDiscoveryWorkerConfig(
    const RelayDeskRuntimeOptions& runtimeOptions)
{
    relaydesk::net::DiscoveryWorkerConfig config;
    config.SetBroadcastInterval(2s);
    config.SetStartupBroadcastInterval(500ms);
    config.SetStartupBroadcastCount(8);
    config.SetPollTimeout(100ms);
    config.SetBroadcastEnabled(runtimeOptions.GetDiscoveryBroadcastEnabled());
    config.SetAnnounceOnStart(runtimeOptions.GetDiscoveryAnnounceOnStart());
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

bool stringStartsWith(const std::string& text, const std::string& prefix)
{
    return text.size() >= prefix.size()
        && text.compare(0, prefix.size(), prefix) == 0;
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

bool isAcceptableIncomingTransferState(relaydesk::storage::TransferState state)
{
    return state == relaydesk::storage::TransferState::Offered
        || state == relaydesk::storage::TransferState::Interrupted;
}

bool isTerminalTransferState(relaydesk::storage::TransferState state)
{
    return state == relaydesk::storage::TransferState::Completed
        || state == relaydesk::storage::TransferState::Interrupted
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
    const bool failedStateCanBeReplaced =
        currentState == relaydesk::storage::TransferState::Failed
        && (nextState == relaydesk::storage::TransferState::Rejected
            || nextState == relaydesk::storage::TransferState::Cancelled
            || nextState == relaydesk::storage::TransferState::Transferring
            || nextState == relaydesk::storage::TransferState::Completed);
    const bool interruptedStateCanBeReplaced =
        currentState == relaydesk::storage::TransferState::Interrupted
        && (nextState == relaydesk::storage::TransferState::Transferring
            || nextState == relaydesk::storage::TransferState::Completed
            || nextState == relaydesk::storage::TransferState::Failed
            || nextState == relaydesk::storage::TransferState::Cancelled
            || nextState == relaydesk::storage::TransferState::Rejected);
    return currentState != nextState
        && isTerminalTransferState(currentState)
        && !failedStateCanBeReplaced
        && !interruptedStateCanBeReplaced;
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
    case relaydesk::storage::TransferState::Interrupted:
    case relaydesk::storage::TransferState::Failed:
    case relaydesk::storage::TransferState::Cancelled:
    case relaydesk::storage::TransferState::Rejected:
        return;
    }
}

void setTransferProgressForStateUpdate(
    relaydesk::storage::ChatMessagePart& part,
    relaydesk::storage::TransferState transferState,
    std::optional<relaydesk::storage::TransferState> previousState)
{
    if (previousState.has_value()
        && previousState.value() == relaydesk::storage::TransferState::Failed
        && transferState == relaydesk::storage::TransferState::Transferring) {
        setTransferProgress(part, 0);
        return;
    }

    setTransferProgressForState(part, transferState);
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

bool recoverInterruptedTransferParts(
    relaydesk::storage::ChatMessageRecord& record)
{
    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    bool recovered = false;
    for (auto& part : parts) {
        if (!isFileTransferPart(part)
            || !part.GetTransferState().has_value()
            || part.GetTransferState().value()
                != relaydesk::storage::TransferState::Transferring) {
            continue;
        }

        part.SetTransferState(relaydesk::storage::TransferState::Interrupted);
        setTransferProgressForState(part,
                                    relaydesk::storage::TransferState::Interrupted);
        recovered = true;
    }

    if (recovered) {
        record.SetParts(std::move(parts));
    }
    return recovered;
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

        const std::optional<relaydesk::storage::TransferState> previousState =
            part.GetTransferState();
        part.SetTransferId(transferId);
        part.SetTransferState(transferState);
        part.SetFileName(fileName);
        part.SetFileSize(fileSize);
        part.SetLocalPath(localPath);
        if (sha256.has_value()) {
            part.SetSha256(sha256.value());
        }
        setTransferProgressForStateUpdate(part, transferState, previousState);
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

        const std::optional<relaydesk::storage::TransferState> previousState =
            part.GetTransferState();
        part.SetTransferState(transferState);
        setTransferProgressForStateUpdate(part, transferState, previousState);
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

bool hasSendableTransferPartState(
    const relaydesk::storage::ChatMessageRecord& record,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId)
{
    if (record.GetMessageId() != messageId) {
        return false;
    }

    for (const auto& part : record.GetParts()) {
        const bool partMatches = part.GetPartId() == partId
            || (part.GetTransferId().has_value()
                && part.GetTransferId().value() == transferId);
        if (!partMatches
            || !isFileTransferPart(part)
            || !part.GetTransferState().has_value()) {
            continue;
        }

        const relaydesk::storage::TransferState state =
            part.GetTransferState().value();
        return state == relaydesk::storage::TransferState::Offered
            || state == relaydesk::storage::TransferState::Transferring
            || state == relaydesk::storage::TransferState::Interrupted
            || state == relaydesk::storage::TransferState::Failed;
    }

    return false;
}

relaydesk::storage::TransferState transferSendFailureState(
    const std::string& errorMessage)
{
    if (errorMessage == "transfer_cancelled"
        || errorMessage == "TCP peer transport is not available."
        || stringStartsWith(errorMessage, "Transfer source")
        || stringStartsWith(errorMessage, "Failed to open transfer source")
        || stringStartsWith(errorMessage, "Failed to seek transfer source")
        || stringStartsWith(errorMessage, "Accepted transfer part")) {
        return relaydesk::storage::TransferState::Failed;
    }

    return relaydesk::storage::TransferState::Interrupted;
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

std::filesystem::path makeIncomingTransferPayloadPath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& transferId,
    const std::string& fileName,
    const std::filesystem::path& finalPath,
    bool folderTransfer,
    bool imageTransfer)
{
    if (imageTransfer || finalPath.empty()) {
        return makeIncomingTempFilePath(appPaths, transferId, fileName);
    }

    return finalPath;
}

bool pathsReferToSameLocation(const std::filesystem::path& left,
                              const std::filesystem::path& right)
{
    if (left.empty() || right.empty()) {
        return false;
    }

    std::error_code error;
    const std::filesystem::path normalizedLeft =
        std::filesystem::absolute(left, error).lexically_normal();
    if (error) {
        error.clear();
        return left.lexically_normal() == right.lexically_normal();
    }

    error.clear();
    const std::filesystem::path normalizedRight =
        std::filesystem::absolute(right, error).lexically_normal();
    if (error) {
        error.clear();
        return left.lexically_normal() == right.lexically_normal();
    }

    return normalizedLeft == normalizedRight;
}

bool pathIsUnderDirectory(const std::filesystem::path& path,
                          const std::filesystem::path& directory)
{
    std::error_code error;
    const std::filesystem::path normalizedPath =
        std::filesystem::absolute(path, error).lexically_normal();
    if (error) {
        return false;
    }

    error.clear();
    const std::filesystem::path normalizedDirectory =
        std::filesystem::absolute(directory, error).lexically_normal();
    if (error) {
        return false;
    }

    auto pathIterator = normalizedPath.begin();
    for (auto directoryIterator = normalizedDirectory.begin();
         directoryIterator != normalizedDirectory.end();
         ++directoryIterator) {
        if (pathIterator == normalizedPath.end()
            || *pathIterator != *directoryIterator) {
            return false;
        }
        ++pathIterator;
    }

    return true;
}

bool isLegacyIncomingTempPath(const relaydesk::storage::AppPaths& appPaths,
                              const std::filesystem::path& path)
{
    return pathIsUnderDirectory(path, appPaths.GetTempTransfersDirectory());
}

void createIncomingPayloadParentDirectories(const std::filesystem::path& payloadPath)
{
    if (payloadPath.parent_path().empty()) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(payloadPath.parent_path(), error);
    if (error) {
        throw std::runtime_error("Failed to create incoming transfer directory.");
    }
}

void createIncomingPayloadFile(const std::filesystem::path& payloadPath)
{
    createIncomingPayloadParentDirectories(payloadPath);

    std::ofstream output(payloadPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create incoming transfer file.");
    }
}

void prepareIncomingFolderTransferRoot(const std::filesystem::path& targetRoot)
{
    if (targetRoot.empty()) {
        throw std::runtime_error("Incoming folder transfer target is empty.");
    }

    createIncomingPayloadParentDirectories(targetRoot);

    std::error_code error;
    if (std::filesystem::exists(targetRoot, error)) {
        error.clear();
        if (!std::filesystem::is_directory(targetRoot, error)) {
            error.clear();
            std::filesystem::remove(targetRoot, error);
            if (error) {
                throw std::runtime_error(
                    "Failed to replace incoming folder transfer target.");
            }
        }
    }
    if (error) {
        throw std::runtime_error("Failed to inspect incoming folder transfer target.");
    }

    error.clear();
    std::filesystem::create_directories(targetRoot, error);
    if (error) {
        throw std::runtime_error("Failed to create incoming folder transfer target.");
    }
}

void prepareIncomingFolderTransferRootForResume(
    const std::filesystem::path& targetRoot,
    bool resumeTransfer)
{
    if (resumeTransfer) {
        std::error_code error;
        if (std::filesystem::is_directory(targetRoot, error)) {
            return;
        }
    }
    prepareIncomingFolderTransferRoot(targetRoot);
}

void replaceIncomingTransferPayload(const std::filesystem::path& sourcePath,
                                   const std::filesystem::path& targetPath)
{
    if (pathsReferToSameLocation(sourcePath, targetPath)) {
        return;
    }

    createIncomingPayloadParentDirectories(targetPath);

    std::error_code error;
    std::filesystem::remove(targetPath, error);
    if (error) {
        throw std::runtime_error("Failed to replace incoming transfer target.");
    }

    error.clear();
    std::filesystem::rename(sourcePath, targetPath, error);
    if (!error) {
        return;
    }

    error.clear();
    std::filesystem::copy_file(sourcePath,
                               targetPath,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error) {
        throw std::runtime_error("Failed to move incoming transfer payload.");
    }

    error.clear();
    std::filesystem::remove(sourcePath, error);
    if (error) {
        throw std::runtime_error("Failed to clean up incoming transfer payload.");
    }
}

std::uintmax_t existingIncomingPayloadSize(
    const std::filesystem::path& payloadPath,
    std::uintmax_t expectedSize)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(payloadPath, error) || error) {
        return 0;
    }

    error.clear();
    const std::uintmax_t fileSize =
        std::filesystem::file_size(payloadPath, error);
    if (error || fileSize > expectedSize) {
        error.clear();
        std::filesystem::remove(payloadPath, error);
        return 0;
    }
    return fileSize;
}

std::uintmax_t migrateLegacyIncomingPayload(
    const relaydesk::storage::AppPaths& appPaths,
    const std::filesystem::path& payloadPath,
    const std::string& transferId,
    const std::string& fileName,
    std::uintmax_t expectedSize)
{
    const std::uintmax_t payloadSize =
        existingIncomingPayloadSize(payloadPath, expectedSize);
    if (payloadSize > 0) {
        return payloadSize;
    }

    const std::filesystem::path legacyPath =
        makeIncomingTempFilePath(appPaths, transferId, fileName);
    const std::uintmax_t legacySize =
        existingIncomingPayloadSize(legacyPath, expectedSize);
    if (legacySize == 0) {
        return 0;
    }

    replaceIncomingTransferPayload(legacyPath, payloadPath);
    return existingIncomingPayloadSize(payloadPath, expectedSize);
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
    const std::string& fileName)
{
    return makeAvailableSiblingPath(
        appPaths.GetInboxDirectory()
        / filesystemPathFromUtf8String(sanitizeFileName(fileName)));
}

std::filesystem::path makeIncomingFinalFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const PendingIncomingTransfer& transfer)
{
    return makeIncomingFinalFilePath(appPaths, transfer.GetFileName());
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

std::filesystem::path makeAppUpdateTempFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& requestId)
{
    return makeAppUpdateDirectory(appPaths, requestId)
        / filesystemPathFromUtf8String(appUpdatePackageFileName(appPaths));
}

std::filesystem::path makeAppUpdateLogFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& requestId)
{
    return makeAppUpdateDirectory(appPaths, requestId)
        / filesystemPathFromUtf8String(kAppUpdateLogFileName);
}

std::filesystem::path makeAppUpdateHelperFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& requestId)
{
    return makeAppUpdateDirectory(appPaths, requestId)
        / filesystemPathFromUtf8String(kAppUpdateHelperFileName);
}

std::uint32_t currentProcessId()
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return 0;
#endif
}

#if defined(_WIN32)
std::wstring quoteWindowsCommandLineArgument(const std::wstring& value)
{
    std::wstring result;
    result.reserve(value.size() + 2);
    result.push_back(L'"');
    std::size_t slashCount = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashCount;
            continue;
        }
        if (character == L'"') {
            result.append(slashCount * 2 + 1, L'\\');
            result.push_back(character);
            slashCount = 0;
            continue;
        }
        result.append(slashCount, L'\\');
        slashCount = 0;
        result.push_back(character);
    }
    result.append(slashCount * 2, L'\\');
    result.push_back(L'"');
    return result;
}
#endif

std::filesystem::path absoluteNormalizedPath(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
    if (error || absolutePath.empty()) {
        absolutePath = path;
    }
    return absolutePath.lexically_normal();
}

std::wstring makeWindowsCommandLine(
    const std::filesystem::path& executablePath,
    const std::vector<std::wstring>& arguments)
{
    std::wstring commandLine =
        quoteWindowsCommandLineArgument(executablePath.wstring());
    for (const std::wstring& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsCommandLineArgument(argument);
    }
    return commandLine;
}

bool launchWindowsProcess(const std::filesystem::path& executablePath,
                          const std::vector<std::wstring>& arguments,
                          const std::filesystem::path& workingDirectory)
{
#if defined(_WIN32)
    std::wstring commandLine = makeWindowsCommandLine(executablePath, arguments);
    std::wstring directory = workingDirectory.wstring();
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION processInfo{};
    const BOOL started = CreateProcessW(nullptr,
                                        commandLine.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        directory.empty() ? nullptr : directory.c_str(),
                                        &startupInfo,
                                        &processInfo);
    if (!started) {
        return false;
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
#else
    (void)executablePath;
    (void)arguments;
    (void)workingDirectory;
    return false;
#endif
}

bool waitForProcessExit(std::uint32_t processId,
                        std::chrono::milliseconds timeout)
{
#if defined(_WIN32)
    if (processId == 0 || processId == currentProcessId()) {
        return true;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        return true;
    }
    const DWORD result =
        WaitForSingleObject(process, static_cast<DWORD>(timeout.count()));
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
#else
    (void)processId;
    (void)timeout;
    return true;
#endif
}

void appendAppUpdateLog(const std::filesystem::path& logPath,
                        const std::string& message)
{
    std::filesystem::create_directories(logPath.parent_path());
    std::ofstream output(logPath, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("Failed to open app update log.");
    }
    output << '[' << relaydesk::core::currentUtcTimestamp() << "] "
           << message << '\n';
}

bool copyFileWithRetry(const std::filesystem::path& sourcePath,
                       const std::filesystem::path& targetPath,
                       const std::filesystem::path& logPath)
{
    for (int attempt = 1; attempt <= 60; ++attempt) {
        appendAppUpdateLog(logPath,
                           "copy attempt " + std::to_string(attempt));
        std::error_code error;
        std::filesystem::copy_file(
            sourcePath,
            targetPath,
            std::filesystem::copy_options::overwrite_existing,
            error);
        if (!error) {
            appendAppUpdateLog(logPath, "copy succeeded");
            return true;
        }
        appendAppUpdateLog(logPath, "copy failed: " + error.message());
        std::this_thread::sleep_for(1s);
    }
    appendAppUpdateLog(logPath, "copy failed after 60 attempts");
    return false;
}

bool forceProcessExitForUpdate(std::uint32_t processId,
                               const std::filesystem::path& logPath)
{
#if defined(_WIN32)
    if (processId == 0 || processId == currentProcessId()) {
        return true;
    }

    HANDLE process =
        OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        appendAppUpdateLog(logPath, "target process already exited");
        return true;
    }

    appendAppUpdateLog(logPath, "waiting for target process exit");
    DWORD waitResult = WaitForSingleObject(process, 15000);
    if (waitResult == WAIT_OBJECT_0) {
        CloseHandle(process);
        return true;
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE) {
        appendAppUpdateLog(logPath, "terminating target process");
        (void)TerminateProcess(process, 0);
    }
    waitResult = WaitForSingleObject(process, 60000);
    CloseHandle(process);
    return waitResult == WAIT_OBJECT_0;
#else
    (void)processId;
    (void)logPath;
    return true;
#endif
}

bool clearReadOnlyAttribute(const std::filesystem::path& targetPath)
{
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(targetPath.wstring().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) == 0) {
        return true;
    }
    return SetFileAttributesW(
               targetPath.wstring().c_str(),
               attributes & ~FILE_ATTRIBUTE_READONLY)
        != FALSE;
#else
    (void)targetPath;
    return true;
#endif
}

std::vector<std::wstring> makeAppUpdateApplyArguments(
    const AppUpdateApplyOptions& options)
{
    return {kAppUpdateApplyArgument,
            kAppUpdateTargetArgument,
            options.GetTargetPath().wstring(),
            kAppUpdatePayloadArgument,
            options.GetPayloadPath().wstring(),
            kAppUpdatePidArgument,
            std::to_wstring(options.GetTargetProcessId()),
            kAppUpdateStartDirectoryArgument,
            options.GetStartDirectory().wstring(),
            kAppUpdateLogArgument,
            options.GetLogPath().wstring(),
            kAppUpdateRestartArgument,
            options.GetRestartAfterApply() ? L"1" : L"0"};
}

bool parseUnsignedLong(const std::wstring& text, unsigned long& value)
{
    try {
        std::size_t parsedLength = 0;
        const unsigned long parsed = std::stoul(text, &parsedLength, 10);
        if (parsedLength != text.size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
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

std::uintmax_t folderPackageEntriesTotalFileSize(
    const std::vector<FolderPackageEntry>& entries)
{
    std::uintmax_t totalSize = 0;
    for (const FolderPackageEntry& entry : entries) {
        if (entry.type == FolderPackageEntryType::File) {
            totalSize += entry.fileSize;
        }
    }
    return totalSize;
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

std::filesystem::path resolveIncomingFolderEntryPath(
    const std::filesystem::path& targetRoot,
    const std::string& relativePathText)
{
    if (relativePathText.empty()
        || relativePathText.size() > kMaxFolderPackageRelativePathBytes) {
        throw std::runtime_error("Folder transfer path is invalid.");
    }

    const std::filesystem::path relativePath =
        filesystemPathFromUtf8String(relativePathText).lexically_normal();
    if (!isSafeFolderPackageRelativePath(relativePath)) {
        throw std::runtime_error("Folder transfer path is unsafe.");
    }

    const std::filesystem::path targetPath =
        (targetRoot / relativePath).lexically_normal();
    if (!pathIsUnderDirectory(targetPath, targetRoot)) {
        throw std::runtime_error("Folder transfer path escapes target directory.");
    }
    return targetPath;
}

std::uintmax_t existingIncomingFolderPayloadSize(
    const std::filesystem::path& targetRoot,
    std::uintmax_t expectedSize)
{
    if (!std::filesystem::is_directory(targetRoot)) {
        return 0;
    }

    const std::vector<FolderPackageEntry> receivedEntries =
        collectFolderPackageEntries(targetRoot);
    std::uintmax_t receivedSize = 0;
    for (const FolderPackageEntry& entry : receivedEntries) {
        if (entry.type == FolderPackageEntryType::File) {
            receivedSize += entry.fileSize;
        }
        if (receivedSize >= expectedSize) {
            return expectedSize;
        }
    }
    return std::min(receivedSize, expectedSize);
}

void writeIncomingFolderTransferChunk(
    const PendingIncomingTransfer& transfer,
    const relaydesk::net::TransferChunkMessage& chunk,
    const std::vector<std::uint8_t>& body)
{
    if (!chunk.GetFolderRelativePath().has_value()) {
        throw std::runtime_error("Incoming folder transfer chunk has no path.");
    }

    const std::filesystem::path targetPath =
        resolveIncomingFolderEntryPath(transfer.GetTempFilePath(),
                                       chunk.GetFolderRelativePath().value());
    if (chunk.GetFolderDirectory()) {
        if (!body.empty()) {
            throw std::runtime_error(
                "Incoming folder transfer directory chunk has payload.");
        }
        std::error_code error;
        std::filesystem::create_directories(targetPath, error);
        if (error) {
            throw std::runtime_error(
                "Failed to create incoming folder transfer directory.");
        }
        return;
    }

    createIncomingPayloadParentDirectories(targetPath);
    if (chunk.GetFolderFileOffset() > 0) {
        std::error_code error;
        const std::uintmax_t existingSize =
            std::filesystem::file_size(targetPath, error);
        if (error || existingSize != chunk.GetFolderFileOffset()) {
            throw std::runtime_error(
                "Incoming folder transfer file offset is not sequential.");
        }
    }

    const std::ios::openmode mode =
        chunk.GetFolderFileOffset() == 0 ? std::ios::trunc : std::ios::app;
    std::ofstream output(targetPath, std::ios::binary | mode);
    if (!output) {
        throw std::runtime_error("Failed to write incoming folder transfer file.");
    }
    output.write(reinterpret_cast<const char*>(body.data()),
                 static_cast<std::streamsize>(body.size()));
    if (!output) {
        throw std::runtime_error("Failed to append incoming folder transfer file.");
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
    std::uintmax_t fileSize,
    bool resumeRequest = false)
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
    message.SetResumeRequest(resumeRequest);
    if (part.GetSha256().has_value()) {
        message.SetSha256(part.GetSha256().value());
    }
    return message;
}

relaydesk::net::PeerFrame makeTransferResumeRequestFrame(
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::ChatMessagePart& part,
    const relaydesk::storage::AppPaths& appPaths)
{
    if (!part.GetTransferId().has_value() || !part.GetLocalPath().has_value()) {
        throw std::runtime_error("Transfer part is missing local file metadata.");
    }

    const std::filesystem::path localPath =
        resolveLocalPath(appPaths, part.GetLocalPath().value());
    std::uintmax_t fileSize = 0;
    if (part.GetType() == relaydesk::storage::MessagePartType::Folder) {
        if (!std::filesystem::is_directory(localPath)) {
            throw std::runtime_error("Transfer source folder does not exist.");
        }
        fileSize = folderPackageEntriesTotalFileSize(
            collectFolderPackageEntries(localPath));
    } else if (!std::filesystem::is_regular_file(localPath)) {
        throw std::runtime_error("Transfer source file does not exist.");
    } else {
        fileSize = std::filesystem::file_size(localPath);
    }

    const std::string fileName = chooseTransferFileName(part, localPath);
    return
        relaydesk::net::makeTransferOfferFrame(
            makeTransferOfferMessage(record, part, fileName, fileSize, true));
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

relaydesk::net::TransferChunkMessage makeFolderTransferChunkMessage(
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::ChatMessagePart& part,
    const FolderPackageEntry& entry,
    std::uintmax_t transferOffset,
    std::uintmax_t fileOffset)
{
    relaydesk::net::TransferChunkMessage message =
        makeTransferChunkMessage(record, part, transferOffset);
    message.SetFolderRelativePath(entry.relativePath);
    message.SetFolderFileOffset(fileOffset);
    message.SetFolderDirectory(entry.type == FolderPackageEntryType::Directory);
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
    bool overwriteExisting,
    std::uintmax_t resumeOffset = 0)
{
    relaydesk::net::TransferAcceptMessage message;
    message.SetMessageId(record.GetMessageId());
    message.SetPartId(part.GetPartId());
    message.SetTransferId(part.GetTransferId().value());
    message.SetReceiverDeviceId(receiverDeviceId);
    message.SetSaveStrategy(overwriteExisting
                                ? relaydesk::net::TransferSaveStrategy::Overwrite
                                : relaydesk::net::TransferSaveStrategy::Unique);
    message.SetResumeOffset(resumeOffset);
    return message;
}

relaydesk::net::PeerFrame makePendingTransferAcceptFrame(
    const PendingIncomingTransfer& transfer,
    const std::string& receiverDeviceId,
    std::uintmax_t resumeOffset)
{
    relaydesk::net::TransferAcceptMessage message;
    message.SetMessageId(transfer.GetMessageId());
    message.SetPartId(transfer.GetPartId());
    message.SetTransferId(transfer.GetTransferId());
    message.SetReceiverDeviceId(receiverDeviceId);
    message.SetSaveStrategy(relaydesk::net::TransferSaveStrategy::Overwrite);
    message.SetResumeOffset(resumeOffset);
    return relaydesk::net::makeTransferAcceptFrame(message);
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
    const std::string& cancellerDeviceId,
    std::string reason = "user_cancelled")
{
    relaydesk::net::TransferCancelMessage message;
    message.SetMessageId(record.GetMessageId());
    message.SetPartId(part.GetPartId());
    message.SetTransferId(part.GetTransferId().value());
    message.SetCancellerDeviceId(cancellerDeviceId);
    message.SetReason(std::move(reason));
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

std::filesystem::path incomingTransferLocalPath(
    const PendingIncomingTransfer& transfer)
{
    return transfer.GetFinalFilePath().empty()
        ? transfer.GetTempFilePath()
        : transfer.GetFinalFilePath();
}

PendingTransferUpdate makeCancelledIncomingTransferUpdate(
    const relaydesk::storage::AppPaths& appPaths,
    const PendingIncomingTransfer& transfer)
{
    PendingTransferUpdate update;
    update.SetPeerDeviceId(transfer.GetSenderDeviceId());
    update.SetMessageId(transfer.GetMessageId());
    update.SetPartId(transfer.GetPartId());
    update.SetTransferId(transfer.GetTransferId());
    update.SetFileName(transfer.GetFileName());
    update.SetFileSize(transfer.GetExpectedSize());
    update.SetLocalPath(makeWorkRelativePath(appPaths,
                                             incomingTransferLocalPath(transfer)));
    update.SetTransferState(relaydesk::storage::TransferState::Cancelled);
    return update;
}

PendingTransferUpdate makeFailedIncomingTransferUpdate(
    const relaydesk::storage::AppPaths& appPaths,
    const PendingIncomingTransfer& transfer)
{
    PendingTransferUpdate update;
    update.SetPeerDeviceId(transfer.GetSenderDeviceId());
    update.SetMessageId(transfer.GetMessageId());
    update.SetPartId(transfer.GetPartId());
    update.SetTransferId(transfer.GetTransferId());
    update.SetFileName(transfer.GetFileName());
    update.SetFileSize(transfer.GetExpectedSize());
    update.SetLocalPath(makeWorkRelativePath(appPaths,
                                             incomingTransferLocalPath(transfer)));
    update.SetTransferState(relaydesk::storage::TransferState::Failed);
    return update;
}

PendingTransferUpdate makeInterruptedIncomingTransferUpdate(
    const relaydesk::storage::AppPaths& appPaths,
    const PendingIncomingTransfer& transfer)
{
    PendingTransferUpdate update;
    update.SetPeerDeviceId(transfer.GetSenderDeviceId());
    update.SetMessageId(transfer.GetMessageId());
    update.SetPartId(transfer.GetPartId());
    update.SetTransferId(transfer.GetTransferId());
    update.SetFileName(transfer.GetFileName());
    update.SetFileSize(transfer.GetExpectedSize());
    update.SetLocalPath(makeWorkRelativePath(appPaths,
                                             incomingTransferLocalPath(transfer)));
    update.SetTransferState(relaydesk::storage::TransferState::Interrupted);
    return update;
}

PendingTransferUpdate makePreparedOutgoingTransferUpdate(
    const std::string& peerDeviceId,
    const std::string& messageId,
    const std::string& partId,
    const std::string& transferId,
    const std::string& fileName,
    std::uintmax_t fileSize,
    const std::string& localPath)
{
    PendingTransferUpdate update;
    update.SetPeerDeviceId(peerDeviceId);
    update.SetMessageId(messageId);
    update.SetPartId(partId);
    update.SetTransferId(transferId);
    update.SetFileName(fileName);
    update.SetFileSize(fileSize);
    update.SetLocalPath(localPath);
    update.SetTransferState(relaydesk::storage::TransferState::Transferring);
    return update;
}
#endif

} // namespace

std::optional<AppUpdateApplyOptions> parseAppUpdateApplyOptions(
    const std::vector<std::wstring>& arguments)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    if (arguments.empty() || arguments.front() != kAppUpdateApplyArgument) {
        return std::nullopt;
    }

    AppUpdateApplyOptions options;
    bool hasTarget = false;
    bool hasPayload = false;
    bool hasProcessId = false;
    bool hasStartDirectory = false;
    bool hasLog = false;
    bool hasRestart = false;
    for (std::size_t index = 1; index + 1 < arguments.size(); index += 2) {
        const std::wstring& key = arguments[index];
        const std::wstring& value = arguments[index + 1];
        if (key == kAppUpdateTargetArgument) {
            options.SetTargetPath(absoluteNormalizedPath(value));
            hasTarget = true;
        } else if (key == kAppUpdatePayloadArgument) {
            options.SetPayloadPath(absoluteNormalizedPath(value));
            hasPayload = true;
        } else if (key == kAppUpdatePidArgument) {
            unsigned long processId = 0;
            if (!parseUnsignedLong(value, processId)) {
                return std::nullopt;
            }
            options.SetTargetProcessId(processId);
            hasProcessId = true;
        } else if (key == kAppUpdateStartDirectoryArgument) {
            options.SetStartDirectory(absoluteNormalizedPath(value));
            hasStartDirectory = true;
        } else if (key == kAppUpdateLogArgument) {
            options.SetLogPath(absoluteNormalizedPath(value));
            hasLog = true;
        } else if (key == kAppUpdateRestartArgument) {
            options.SetRestartAfterApply(value == L"1" || value == L"true"
                                         || value == L"TRUE");
            hasRestart = true;
        } else {
            return std::nullopt;
        }
    }

    if ((arguments.size() % 2) == 0 || !hasTarget || !hasPayload
        || !hasProcessId || !hasStartDirectory || !hasLog || !hasRestart
        || options.GetTargetPath().empty() || options.GetPayloadPath().empty()
        || options.GetStartDirectory().empty() || options.GetLogPath().empty()) {
        return std::nullopt;
    }
    return options;
#else
    (void)arguments;
    return std::nullopt;
#endif
}

int runAppUpdateApplyMode(const AppUpdateApplyOptions& options) noexcept
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    try {
        appendAppUpdateLog(options.GetLogPath(),
                           "RelayDesk update helper started");
        appendAppUpdateLog(
            options.GetLogPath(),
            "target=" + filesystemPathToGenericUtf8String(
                            options.GetTargetPath()));
        appendAppUpdateLog(
            options.GetLogPath(),
            "payload=" + filesystemPathToGenericUtf8String(
                             options.GetPayloadPath()));
        appendAppUpdateLog(
            options.GetLogPath(),
            "startdir=" + filesystemPathToGenericUtf8String(
                              options.GetStartDirectory()));
        appendAppUpdateLog(options.GetLogPath(),
                           "pid="
                               + std::to_string(
                                   options.GetTargetProcessId()));

        if (!std::filesystem::is_regular_file(options.GetPayloadPath())) {
            appendAppUpdateLog(options.GetLogPath(), "payload file missing");
            return 2;
        }

        const bool processExited =
            options.GetRestartAfterApply()
                ? forceProcessExitForUpdate(
                      static_cast<std::uint32_t>(
                          options.GetTargetProcessId()),
                      options.GetLogPath())
                : waitForProcessExit(
                      static_cast<std::uint32_t>(
                          options.GetTargetProcessId()),
                      60000ms);
        if (!processExited) {
            appendAppUpdateLog(options.GetLogPath(),
                               "target process did not exit");
            return 3;
        }

        if (!clearReadOnlyAttribute(options.GetTargetPath())) {
            appendAppUpdateLog(options.GetLogPath(),
                               "failed to clear read-only attribute");
        }

        if (!copyFileWithRetry(options.GetPayloadPath(),
                               options.GetTargetPath(),
                               options.GetLogPath())) {
            return 4;
        }

        if (options.GetRestartAfterApply()) {
            if (!launchWindowsProcess(options.GetTargetPath(),
                                      {},
                                      options.GetStartDirectory())) {
                appendAppUpdateLog(options.GetLogPath(), "restart failed");
                return 5;
            }
            appendAppUpdateLog(options.GetLogPath(), "restart requested");
        } else {
            appendAppUpdateLog(options.GetLogPath(), "restart skipped");
        }
        appendAppUpdateLog(options.GetLogPath(), "update helper finished");
        return 0;
    } catch (const std::exception& error) {
        try {
            appendAppUpdateLog(options.GetLogPath(),
                               std::string("update helper failed: ")
                                   + error.what());
        } catch (const std::exception&) {
        }
        return 1;
    }
#else
    (void)options;
    return 1;
#endif
}

class DiscoveryWorkerHandle {
public:
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    explicit DiscoveryWorkerHandle(
        std::unique_ptr<relaydesk::net::DiscoveryWorker> worker)
        : worker_(std::move(worker))
    {
    }

    void updateLocalIdentity(relaydesk::storage::LocalIdentity localIdentity)
    {
        worker_->updateLocalIdentity(std::move(localIdentity));
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

    void sendFramesTo(
        const std::string& address,
        std::uint16_t port,
        const relaydesk::net::TcpPeerFrameProducer& frameProducer,
        const relaydesk::net::TcpPeerFrameSentCallback& frameSentCallback = {})
    {
        transport_->sendFramesTo(address,
                                 port,
                                 frameProducer,
                                 frameSentCallback);
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
    const TransferPreparedCallback& onPrepared,
    const TransferProgressCallback& onProgress,
    const ::core::async::CancelToken& cancelToken,
    std::uintmax_t resumeOffset = 0)
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
    std::filesystem::path payloadPath = localPath;
    std::vector<FolderPackageEntry> folderEntries;
    std::uintmax_t fileSize = 0;
    if (folderTransfer) {
        if (!std::filesystem::is_directory(localPath)) {
            throw std::runtime_error("Transfer source folder does not exist.");
        }
        folderEntries = collectFolderPackageEntries(localPath);
        fileSize = folderPackageEntriesTotalFileSize(folderEntries);
    } else if (!std::filesystem::is_regular_file(localPath)) {
        throw std::runtime_error("Transfer source file does not exist.");
    } else {
        fileSize = std::filesystem::file_size(payloadPath);
    }

    const std::string fileName = chooseTransferFileName(part, localPath);
    if (onPrepared) {
        onPrepared(record.GetMessageId(),
                   part.GetPartId(),
                   part.GetTransferId().value(),
                   fileName,
                   fileSize,
                   makeWorkRelativePath(appPaths, localPath));
    }
    if (resumeOffset > fileSize) {
        resumeOffset = 0;
    }
    std::ifstream input;
    if (!folderTransfer) {
        input.open(payloadPath, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Failed to open transfer source file.");
        }
        if (resumeOffset > 0) {
            input.seekg(static_cast<std::streamoff>(resumeOffset), std::ios::beg);
            if (!input) {
                throw std::runtime_error("Failed to seek transfer source file.");
            }
        }
    }
    if (cancelToken.canceled()) {
        return;
    }

    std::vector<std::uint8_t> buffer(
        static_cast<std::size_t>(kTransferChunkSize));
    std::uintmax_t offset = resumeOffset;
    std::size_t folderEntryIndex = 0;
    std::uintmax_t folderEntryStartOffset = 0;
    std::uintmax_t folderFileOffset = 0;
    if (folderTransfer && resumeOffset > 0) {
        while (folderEntryIndex < folderEntries.size()) {
            const FolderPackageEntry& entry = folderEntries[folderEntryIndex];
            const std::uintmax_t entrySize =
                entry.type == FolderPackageEntryType::File ? entry.fileSize : 0;
            if (folderEntryStartOffset + entrySize > resumeOffset) {
                folderFileOffset = resumeOffset - folderEntryStartOffset;
                break;
            }
            folderEntryStartOffset += entrySize;
            ++folderEntryIndex;
        }
    }
    std::optional<std::uintmax_t> sentChunkOffset;
    bool offerFrameSent = false;
    bool completeFrameSent = false;
    transport.sendFramesTo(
        peer.GetAddress(),
        peer.GetTcpPort(),
        [&]() -> std::optional<relaydesk::net::PeerFrame> {
            if (cancelToken.canceled()) {
                return std::nullopt;
            }
            if (!offerFrameSent) {
                offerFrameSent = true;
                return relaydesk::net::makeTransferOfferFrame(
                    makeTransferOfferMessage(record, part, fileName, fileSize));
            }
            if (folderTransfer) {
                while (folderEntryIndex < folderEntries.size()) {
                    const FolderPackageEntry& entry = folderEntries[folderEntryIndex];
                    if (entry.type == FolderPackageEntryType::Directory) {
                        ++folderEntryIndex;
                        return relaydesk::net::makeTransferChunkFrame(
                            makeFolderTransferChunkMessage(record,
                                                           part,
                                                           entry,
                                                           offset,
                                                           0),
                            {});
                    }

                    if (entry.fileSize == 0) {
                        ++folderEntryIndex;
                        return relaydesk::net::makeTransferChunkFrame(
                            makeFolderTransferChunkMessage(record,
                                                           part,
                                                           entry,
                                                           offset,
                                                           0),
                            {});
                    }

                    if (!input.is_open()) {
                        input.open(entry.sourcePath, std::ios::binary);
                        if (!input) {
                            throw std::runtime_error(
                                "Failed to open folder transfer file.");
                        }
                        if (folderFileOffset > 0) {
                            input.seekg(static_cast<std::streamoff>(folderFileOffset),
                                        std::ios::beg);
                            if (!input) {
                                throw std::runtime_error(
                                    "Failed to seek folder transfer file.");
                            }
                        }
                    }

                    input.read(reinterpret_cast<char*>(buffer.data()),
                               static_cast<std::streamsize>(buffer.size()));
                    const std::streamsize readSize = input.gcount();
                    if (readSize > 0) {
                        std::vector<std::uint8_t> chunk(
                            buffer.begin(),
                            buffer.begin() + readSize);
                        const std::uintmax_t chunkOffset = offset;
                        const std::uintmax_t chunkFileOffset = folderFileOffset;
                        offset += static_cast<std::uintmax_t>(readSize);
                        folderFileOffset += static_cast<std::uintmax_t>(readSize);
                        sentChunkOffset = offset;
                        return relaydesk::net::makeTransferChunkFrame(
                            makeFolderTransferChunkMessage(record,
                                                           part,
                                                           entry,
                                                           chunkOffset,
                                                           chunkFileOffset),
                            std::move(chunk));
                    }

                    if (folderFileOffset != entry.fileSize) {
                        throw std::runtime_error(
                            "Folder transfer source file changed while sending.");
                    }
                    input.close();
                    input.clear();
                    folderEntryStartOffset += entry.fileSize;
                    folderFileOffset = 0;
                    ++folderEntryIndex;
                }
            } else if (input) {
                input.read(reinterpret_cast<char*>(buffer.data()),
                           static_cast<std::streamsize>(buffer.size()));
                const std::streamsize readSize = input.gcount();
                if (readSize > 0) {
                    std::vector<std::uint8_t> chunk(
                        buffer.begin(),
                        buffer.begin() + readSize);
                    const std::uintmax_t chunkOffset = offset;
                    offset += static_cast<std::uintmax_t>(readSize);
                    sentChunkOffset = offset;
                    return relaydesk::net::makeTransferChunkFrame(
                        makeTransferChunkMessage(record, part, chunkOffset),
                        std::move(chunk));
                }
            }

            if (offset != fileSize) {
                throw std::runtime_error(
                    "Transfer source file changed while sending.");
            }
            if (completeFrameSent) {
                return std::nullopt;
            }
            completeFrameSent = true;
            return relaydesk::net::makeTransferCompleteFrame(
                makeTransferCompleteMessage(record, part, fileSize));
        },
        [&]() {
            if (!sentChunkOffset.has_value()) {
                return;
            }
            onProgress(record.GetMessageId(),
                       part.GetPartId(),
                       part.GetTransferId().value(),
                       sentChunkOffset.value());
            sentChunkOffset.reset();
        });

    if (offset != fileSize) {
        if (cancelToken.canceled()) {
            return;
        }
        throw std::runtime_error("Transfer source file changed while sending.");
    }
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
                                   {},
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
    const TransferPreparedCallback& onPrepared,
    const TransferProgressCallback& onProgress,
    const ::core::async::CancelToken& cancelToken,
    std::uintmax_t resumeOffset = 0)
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
                                   onPrepared,
                                   onProgress,
                                   cancelToken,
                                   resumeOffset);
            return;
        }
    }

    throw std::runtime_error("Accepted transfer part was not found.");
}

void sendRequestedFileTransferResumeRequest(
    TcpPeerTransportHandle& transport,
    const PeerListItem& peer,
    const relaydesk::storage::ChatMessageRecord& record,
    const std::string& partId,
    const std::string& transferId,
    const relaydesk::storage::AppPaths& appPaths)
{
    for (const auto& part : record.GetParts()) {
        const bool partMatches = part.GetPartId() == partId
            || (part.GetTransferId().has_value()
                && part.GetTransferId().value() == transferId);
        if (partMatches && isManualTransferInvitePart(part)) {
            transport.sendFrameTo(
                peer.GetAddress(),
                peer.GetTcpPort(),
                makeTransferResumeRequestFrame(record, part, appPaths));
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
    bool completeFrameSent = false;
    transport.sendFramesTo(
        peer.GetAddress(),
        peer.GetTcpPort(),
        [&]() -> std::optional<relaydesk::net::PeerFrame> {
            if (cancelToken.canceled()) {
                return std::nullopt;
            }
            if (input) {
                input.read(reinterpret_cast<char*>(buffer.data()),
                           static_cast<std::streamsize>(buffer.size()));
                const std::streamsize readSize = input.gcount();
                if (readSize > 0) {
                    std::vector<std::uint8_t> chunk(
                        buffer.begin(),
                        buffer.begin() + readSize);
                    const std::uintmax_t chunkOffset = offset;
                    offset += static_cast<std::uintmax_t>(readSize);
                    return relaydesk::net::makeAppUpdateChunkFrame(
                        makeAppUpdateChunkMessage(request.GetRequestId(),
                                                  chunkOffset,
                                                  fileSize),
                        std::move(chunk));
                }
            }

            if (offset != fileSize) {
                throw std::runtime_error(
                    "App update source executable changed while sending.");
            }
            if (completeFrameSent) {
                return std::nullopt;
            }
            completeFrameSent = true;
            return relaydesk::net::makeAppUpdateCompleteFrame(
                makeAppUpdateCompleteMessage(request.GetRequestId(),
                                             relaydesk::core::kAppVersion,
                                             fileName,
                                             fileSize));
        });

    if (offset != fileSize) {
        if (cancelToken.canceled()) {
            return;
        }
        throw std::runtime_error(
            "App update source executable changed while sending.");
    }
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

void PeerListItem::SetUnreadMessageCount(int unreadMessageCount)
{
    unreadMessageCount_ = unreadMessageCount;
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

PendingChatMessage::PendingChatMessage(
    relaydesk::storage::ChatMessageRecord record,
    bool persisted,
    bool unreadCounted)
    : record_(std::move(record)),
      persisted_(persisted),
      unreadCounted_(unreadCounted)
{
}

RelayDeskRuntimeOptions::RelayDeskRuntimeOptions()
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    tcpListenPort_ = relaydesk::net::kDefaultAdvertisedTcpPort;
    discoveryUdpPort_ = relaydesk::net::kDefaultDiscoveryUdpPort;
#endif
}

RelayDeskRuntime::RelayDeskRuntime()
    : RelayDeskRuntime(RelayDeskRuntimeOptions{})
{
}

RelayDeskRuntime::RelayDeskRuntime(RelayDeskRuntimeOptions options)
    : runtimeOptions_(std::move(options))
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

std::uint64_t RelayDeskRuntime::ConsumePendingUserNotificationCount()
{
    return pendingUserNotificationCount_.exchange(0);
}

void RelayDeskRuntime::SetUserNotificationHandler(std::function<void()> handler)
{
    std::lock_guard lock(userNotificationMutex_);
    userNotificationHandler_ = std::move(handler);
}

void RelayDeskRuntime::updateLocalDisplayName(std::string displayName)
{
    if (!storageAvailable_) {
        throw std::runtime_error("Local storage is not available.");
    }
    if (isBlankText(displayName)) {
        throw std::invalid_argument("Local display name cannot be blank.");
    }

    const auto appPaths = relaydesk::storage::createAppPaths();
    const relaydesk::storage::LocalIdentity identity =
        relaydesk::storage::updateLocalDisplayName(appPaths, displayName);
    localUser_.SetDisplayName(identity.GetDisplayName());
    localUser_.SetHostName(identity.GetHostName());
    localUser_.SetDeviceId(identity.GetDeviceId());
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    if (discoveryWorker_) {
        discoveryWorker_->updateLocalIdentity(identity);
    }
#endif
    logDiagnostic("runtime.identity.display_name_updated device_id="
                  + identity.GetDeviceId()
                  + " display_name=" + identity.GetDisplayName());
    requestUiRefresh();
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
    (void)peer;
    std::optional<PeerListItem> promptedPeer;
    {
        std::lock_guard lock(pendingAppUpdateMutex_);
        if (appUpdatePrompt_.has_value()
            && appUpdatePrompt_->GetState() == AppUpdatePromptState::Downloading) {
            return;
        }

        auto isCandidate = [this](const PeerListItem& candidate) {
            if ((runtimeOptions_.GetNetworkEnabled() && !tcpPeerTransport_)
                || !candidate.GetOnline()
                || candidate.GetDeviceId().empty()
                || candidate.GetDeviceId() == localUser_.GetDeviceId()
                || candidate.GetAppVersion() <= relaydesk::core::kAppVersion
                || candidate.GetAddress().empty()
                || candidate.GetAddress() == "unknown"
                || candidate.GetTcpPort() == 0) {
                return false;
            }

            const auto dismissedVersion =
                dismissedAppUpdateVersions_.find(candidate.GetDeviceId());
            if (dismissedVersion != dismissedAppUpdateVersions_.end()
                && dismissedVersion->second >= candidate.GetAppVersion()) {
                return false;
            }

            const auto requestedVersion =
                requestedAppUpdateVersions_.find(candidate.GetDeviceId());
            if (requestedVersion != requestedAppUpdateVersions_.end()
                && requestedVersion->second >= candidate.GetAppVersion()) {
                return false;
            }

            return !scheduledAppUpdate_.has_value()
                || scheduledAppUpdate_->GetAppVersion() < candidate.GetAppVersion();
        };

        std::optional<PeerListItem> bestPeer;
        for (const PeerListItem& candidate : peers_) {
            if (!isCandidate(candidate)) {
                continue;
            }
            if (!bestPeer.has_value()
                || candidate.GetAppVersion() > bestPeer->GetAppVersion()) {
                bestPeer = candidate;
            }
        }
        if (!bestPeer.has_value()) {
            return;
        }

        if (appUpdatePrompt_.has_value()
            && appUpdatePrompt_->GetSourceDeviceId() == bestPeer->GetDeviceId()
            && appUpdatePrompt_->GetAppVersion() == bestPeer->GetAppVersion()) {
            return;
        }
        if (appUpdatePrompt_.has_value()
            && appUpdatePrompt_->GetState() != AppUpdatePromptState::Failed
            && appUpdatePrompt_->GetAppVersion() == bestPeer->GetAppVersion()) {
            return;
        }

        AppUpdatePrompt prompt;
        prompt.SetSourceDeviceId(bestPeer->GetDeviceId());
        prompt.SetSourceDisplayName(bestPeer->GetDisplayName().empty()
                                        ? bestPeer->GetHostName()
                                        : bestPeer->GetDisplayName());
        prompt.SetFileName("relaydesk.exe");
        prompt.SetAppVersion(bestPeer->GetAppVersion());
        prompt.SetState(AppUpdatePromptState::Available);
        appUpdatePrompt_ = std::move(prompt);
        promptedPeer = bestPeer;
    }

    if (!promptedPeer.has_value()) {
        return;
    }

    logDiagnostic("runtime.update.available device_id=" + promptedPeer->GetDeviceId()
                  + " peer_app_version="
                  + std::to_string(promptedPeer->GetAppVersion())
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

AppUpdateApplyOptions RelayDeskRuntime::prepareDownloadedAppUpdate(
    const PendingIncomingAppUpdate& update,
    bool restartAfterApply)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    if (update.GetAppVersion() <= relaydesk::core::kAppVersion) {
        logDiagnostic("runtime.update.apply_ignored request_id="
                      + update.GetRequestId()
                      + " app_version="
                      + std::to_string(update.GetAppVersion()));
        return {};
    }
    if (update.GetExpectedSize() == 0
        || !std::filesystem::is_regular_file(update.GetTempFilePath())
        || std::filesystem::file_size(update.GetTempFilePath())
            != update.GetExpectedSize()) {
        throw std::runtime_error("Downloaded app update file is incomplete.");
    }

    const auto appPaths = relaydesk::storage::createAppPaths();
    const std::filesystem::path helperPath =
        makeAppUpdateHelperFilePath(appPaths, update.GetRequestId());
    std::filesystem::create_directories(helperPath.parent_path());
    std::error_code copyError;
    std::filesystem::copy_file(appPaths.GetExecutablePath(),
                               helperPath,
                               std::filesystem::copy_options::overwrite_existing,
                               copyError);
    if (copyError || !std::filesystem::is_regular_file(helperPath)) {
        throw std::runtime_error("Failed to prepare app update helper.");
    }

    AppUpdateApplyOptions options;
    options.SetTargetPath(absoluteNormalizedPath(appPaths.GetExecutablePath()));
    options.SetPayloadPath(absoluteNormalizedPath(update.GetTempFilePath()));
    options.SetHelperPath(absoluteNormalizedPath(helperPath));
    options.SetStartDirectory(absoluteNormalizedPath(appPaths.GetWorkDirectory()));
    options.SetLogPath(absoluteNormalizedPath(
        makeAppUpdateLogFilePath(appPaths, update.GetRequestId())));
    options.SetTargetProcessId(currentProcessId());
    options.SetRestartAfterApply(restartAfterApply);
    return options;
#else
    (void)update;
    (void)restartAfterApply;
    return AppUpdateApplyOptions();
#endif
}

bool RelayDeskRuntime::launchAppUpdateHelper(
    const AppUpdateApplyOptions& options)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    if (appUpdateHelperLauncherForTest_) {
        return appUpdateHelperLauncherForTest_(options);
    }
    return launchWindowsProcess(options.GetHelperPath(),
                                makeAppUpdateApplyArguments(options),
                                options.GetStartDirectory());
#else
    (void)options;
    return false;
#endif
}

void RelayDeskRuntime::applyDownloadedAppUpdate(
    const PendingIncomingAppUpdate& update,
    bool restartAfterApply)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    const AppUpdateApplyOptions options =
        prepareDownloadedAppUpdate(update, restartAfterApply);
    if (options.GetHelperPath().empty()) {
        return;
    }
    if (!launchAppUpdateHelper(options)) {
        throw std::runtime_error("Failed to launch app update helper.");
    }

    logDiagnostic("runtime.update.apply_started request_id="
                  + update.GetRequestId()
                  + " app_version="
                  + std::to_string(update.GetAppVersion())
                  + " restart_after_apply="
                  + std::to_string(restartAfterApply));
    if (restartAfterApply) {
        appUpdateExitRequested_.store(true, std::memory_order_relaxed);
        requestUiRefresh();
    }
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
    clearPeerUnreadMessageCount(selected->GetDeviceId());
    requestUiRefresh();
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
    const auto message = std::find_if(
        selectedPeerMessages_.begin(),
        selectedPeerMessages_.end(),
        [&messageId](const relaydesk::storage::ChatMessageRecord& record) {
            return record.GetMessageId() == messageId;
        });
    if (message == selectedPeerMessages_.end()) {
        return;
    }

    const auto part = std::find_if(
        message->GetParts().begin(),
        message->GetParts().end(),
        [&partId](const relaydesk::storage::ChatMessagePart& candidate) {
            return candidate.GetPartId() == partId;
        });
    if (part == message->GetParts().end() || !part->GetFileName().has_value()) {
        return;
    }

    const auto appPaths = relaydesk::storage::createAppPaths();
    std::filesystem::path desiredPath =
        makeIncomingDesiredFilePath(appPaths, part->GetFileName().value());
    const bool resumeInterrupted =
        part->GetTransferState().has_value()
        && part->GetTransferState().value()
            == relaydesk::storage::TransferState::Interrupted;
    if (resumeInterrupted
        && part->GetLocalPath().has_value()
        && !part->GetLocalPath().value().empty()) {
        const std::filesystem::path previousPath =
            resolveLocalPath(appPaths, part->GetLocalPath().value());
        if (!isLegacyIncomingTempPath(appPaths, previousPath)) {
            desiredPath = previousPath;
        }
    }
    acceptSelectedPeerFileTransferToPath(
        messageId,
        partId,
        (overwriteExisting || resumeInterrupted)
            ? desiredPath
            : makeAvailableSiblingPath(desiredPath),
        overwriteExisting || resumeInterrupted);
#else
    (void)messageId;
    (void)partId;
    (void)overwriteExisting;
#endif
}

void RelayDeskRuntime::acceptSelectedPeerFileTransferAs(
    const std::string& messageId,
    const std::string& partId,
    std::filesystem::path finalPath)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    if (finalPath.empty()) {
        return;
    }

    acceptSelectedPeerFileTransferToPath(messageId,
                                         partId,
                                         std::move(finalPath),
                                         true);
#else
    (void)messageId;
    (void)partId;
    (void)finalPath;
#endif
}

void RelayDeskRuntime::acceptSelectedPeerFileTransferToPath(
    const std::string& messageId,
    const std::string& partId,
    std::filesystem::path finalPath,
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
        || !isAcceptableIncomingTransferState(part->GetTransferState().value())
        || !part->GetFileName().has_value()) {
        return;
    }

    const auto appPaths = relaydesk::storage::createAppPaths();
    const std::filesystem::path payloadPath = makeIncomingTransferPayloadPath(
        appPaths,
        part->GetTransferId().value(),
        part->GetFileName().value(),
        finalPath,
        folderTransfer,
        false);
    const bool resumeInterrupted =
        part->GetTransferState().value()
        == relaydesk::storage::TransferState::Interrupted;
    const std::uintmax_t expectedSize = part->GetFileSize().value_or(0);
    std::uintmax_t resumeOffset = 0;
    if (resumeInterrupted) {
        if (folderTransfer) {
            resumeOffset =
                existingIncomingFolderPayloadSize(payloadPath, expectedSize);
        } else {
            resumeOffset = existingIncomingPayloadSize(payloadPath, expectedSize);
            if (resumeOffset == 0) {
                resumeOffset = migrateLegacyIncomingPayload(
                    appPaths,
                    payloadPath,
                    part->GetTransferId().value(),
                    part->GetFileName().value(),
                    expectedSize);
            }
        }
    }
    if (folderTransfer) {
        prepareIncomingFolderTransferRootForResume(payloadPath, resumeInterrupted);
    } else if (!resumeInterrupted) {
        createIncomingPayloadFile(payloadPath);
    }

    PendingIncomingTransfer transfer;
    transfer.SetSenderDeviceId(selectedPeer->GetDeviceId());
    transfer.SetMessageId(record.GetMessageId());
    transfer.SetPartId(part->GetPartId());
    transfer.SetTransferId(part->GetTransferId().value());
    transfer.SetFileName(part->GetFileName().value());
    transfer.SetExpectedSize(expectedSize);
    transfer.SetReceivedSize(resumeOffset);
    transfer.SetTempFilePath(payloadPath);
    transfer.SetFinalFilePath(finalPath);
    transfer.SetImageTransfer(false);
    transfer.SetFolderTransfer(folderTransfer);
    {
        std::lock_guard lock(pendingTransferMutex_);
        pendingIncomingTransfers_[transfer.GetTransferId()] = transfer;
    }

    part->SetTransferState(relaydesk::storage::TransferState::Transferring);
    part->SetTransferredSize(resumeOffset);
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
                                      overwriteExisting,
                                      resumeOffset));
    const std::string acceptTaskKey =
        "relaydesk.transfer.accept." + transfer.GetTransferId();
    const bool accepted = ::core::async::restart(
        acceptTaskKey,
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
         transferId = transfer.GetTransferId(),
         recoverable = resumeInterrupted](
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
            update.SetTransferState(
                recoverable
                    ? relaydesk::storage::TransferState::Interrupted
                    : relaydesk::storage::TransferState::Failed);
            enqueueTransferStateUpdate(std::move(update));
        });
    if (!accepted) {
        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(selectedPeer->GetDeviceId());
        update.SetMessageId(messageId);
        update.SetPartId(partId);
        update.SetTransferId(transfer.GetTransferId());
        update.SetTransferState(resumeInterrupted
                                    ? relaydesk::storage::TransferState::Interrupted
                                    : relaydesk::storage::TransferState::Failed);
        enqueueTransferStateUpdate(std::move(update));
    }
#else
    (void)messageId;
    (void)partId;
    (void)finalPath;
    (void)overwriteExisting;
#endif
}

void RelayDeskRuntime::sendSelectedPeerFileTransfer(const std::string& messageId,
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
            != relaydesk::storage::MessageDirection::Outgoing) {
        return;
    }

    const auto part = std::find_if(
        message->GetParts().begin(),
        message->GetParts().end(),
        [&partId](const relaydesk::storage::ChatMessagePart& candidate) {
            return candidate.GetPartId() == partId;
        });
    if (part == message->GetParts().end()
        || !isManualTransferInvitePart(*part)
        || !part->GetTransferId().has_value()
        || !part->GetTransferState().has_value()) {
        return;
    }

    const relaydesk::storage::TransferState transferState =
        part->GetTransferState().value();
    if (transferState != relaydesk::storage::TransferState::Offered
        && transferState != relaydesk::storage::TransferState::Interrupted
        && transferState != relaydesk::storage::TransferState::Failed) {
        return;
    }

    PendingOutgoingTransferRequest request;
    request.SetReceiverDeviceId(selectedPeer->GetDeviceId());
    request.SetMessageId(messageId);
    request.SetPartId(partId);
    request.SetTransferId(part->GetTransferId().value());
    if (transferState == relaydesk::storage::TransferState::Interrupted) {
        request.SetResumeRequestOnly(true);
    }
    enqueueOutgoingTransferRequest(std::move(request));
#else
    (void)messageId;
    (void)partId;
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
        || !isManualTransferInvitePart(*part)
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
        || !isManualTransferInvitePart(*part)
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
    std::optional<std::filesystem::path> cancelledLocalPath;
    if (record.GetDirection() == relaydesk::storage::MessageDirection::Incoming) {
        std::lock_guard lock(pendingTransferMutex_);
        const auto transfer = pendingIncomingTransfers_.find(transferId);
        if (transfer != pendingIncomingTransfers_.end()) {
            cancelledLocalPath = incomingTransferLocalPath(transfer->second);
        }
    }

    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferCancelFrame(
            makeTransferCancelMessage(record,
                                      *part,
                                      localUser_.GetDeviceId()));

    if (cancelledLocalPath.has_value()) {
        const auto appPaths = relaydesk::storage::createAppPaths();
        part->SetLocalPath(makeWorkRelativePath(appPaths,
                                                cancelledLocalPath.value()));
    }
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
        if (runtimeOptions_.GetNetworkEnabled()) {
            auto tcpTransport =
                std::make_unique<relaydesk::net::BoostAsioTcpPeerTransport>(
                    runtimeOptions_.GetTcpListenPort());
            tcpTransport->SetFrameCallback(
                [this](relaydesk::net::PeerFrame frame,
                       std::string,
                       std::uint16_t) {
                    try {
                        handleIncomingPeerFrame(frame);
                    } catch (const std::exception& error) {
                        failIncomingTransferFromFrame(frame);
                        logDiagnostic(
                            std::string("runtime.tcp.frame_error message=")
                            + error.what());
                    }
                });
            tcpTransport->SetErrorCallback(
                [this](std::string message) {
                    if (message
                            == "Peer TCP transfer stream closed before completion.") {
                        interruptPendingIncomingTransfers();
                    }
                    logDiagnostic("runtime.tcp.error message=" + message);
                });
            tcpTransport->start();
            const std::uint16_t tcpPort = tcpTransport->GetLocalPort();
            tcpPeerTransport_ =
                std::make_unique<TcpPeerTransportHandle>(std::move(tcpTransport));
            logDiagnostic("runtime.tcp.started port=" + std::to_string(tcpPort));

            relaydesk::net::DiscoveryServiceConfig serviceConfig;
            serviceConfig.SetDiscoveryUdpPort(
                runtimeOptions_.GetDiscoveryUdpPort());
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
            auto discoveryWorker =
                std::make_unique<relaydesk::net::DiscoveryWorker>(
                    std::move(discoveryService),
                    makeDiscoveryWorkerConfig(runtimeOptions_),
                    std::move(workerEvents));
            discoveryWorker->start();
            discoveryStarted_ = true;
            discoveryUdpPort_ = discoveryWorker->GetLocalUdpPort();
            logDiagnostic("runtime.discovery.started udp_port="
                          + std::to_string(discoveryUdpPort_));
            discoveryWorker_ =
                std::make_unique<DiscoveryWorkerHandle>(std::move(discoveryWorker));
        }
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
        selectedPeerHasMoreMessages_ = false;
        return;
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        std::lock_guard lock(chatHistoryStorageMutex_);
        const auto result =
            relaydesk::storage::loadRecentChatHistory(
                appPaths,
                selectedPeerDeviceId_,
                kSelectedPeerMessagePageSize);
        selectedPeerMessages_ = result.GetRecords();
        selectedPeerHasMoreMessages_ = result.GetHasMoreRecords();
        bool recovered = false;
        for (auto& record : selectedPeerMessages_) {
            if (recoverInterruptedTransferParts(record)) {
                (void)relaydesk::storage::replaceChatMessage(
                    appPaths,
                    selectedPeerDeviceId_,
                    record);
                recovered = true;
            }
        }
        if (recovered) {
            logDiagnostic("runtime.transfer.recovered_selected device_id="
                          + selectedPeerDeviceId_);
        }
        if (result.GetSkippedLineCount() > 0) {
            logDiagnostic("runtime.chat.history_skipped count="
                          + std::to_string(result.GetSkippedLineCount()));
        }
    } catch (const std::exception& error) {
        selectedPeerMessages_.clear();
        selectedPeerHasMoreMessages_ = false;
        setStartupError(error.what());
    }
}

void RelayDeskRuntime::loadMoreSelectedPeerMessages()
{
    if (selectedPeerDeviceId_.empty()
        || !storageAvailable_
        || !selectedPeerHasMoreMessages_
        || selectedPeerMessages_.empty()) {
        return;
    }

    try {
        const std::string beforeMessageId =
            selectedPeerMessages_.front().GetMessageId();
        const auto appPaths = relaydesk::storage::createAppPaths();
        std::lock_guard lock(chatHistoryStorageMutex_);
        const auto result =
            relaydesk::storage::loadChatHistoryBefore(
                appPaths,
                selectedPeerDeviceId_,
                beforeMessageId,
                kSelectedPeerMessagePageSize);
        if (result.GetRecords().empty()) {
            selectedPeerHasMoreMessages_ = false;
            return;
        }

        std::vector<relaydesk::storage::ChatMessageRecord> olderMessages =
            result.GetRecords();
        bool recovered = false;
        for (auto& record : olderMessages) {
            if (recoverInterruptedTransferParts(record)) {
                (void)relaydesk::storage::replaceChatMessage(
                    appPaths,
                    selectedPeerDeviceId_,
                    record);
                recovered = true;
            }
        }

        std::vector<relaydesk::storage::ChatMessageRecord> mergedMessages;
        mergedMessages.reserve(olderMessages.size() + selectedPeerMessages_.size());
        for (auto& record : olderMessages) {
            mergedMessages.push_back(std::move(record));
        }
        for (auto& record : selectedPeerMessages_) {
            mergedMessages.push_back(std::move(record));
        }
        selectedPeerMessages_ = std::move(mergedMessages);
        selectedPeerHasMoreMessages_ = result.GetHasMoreRecords();

        if (recovered) {
            logDiagnostic("runtime.transfer.recovered_selected_more device_id="
                          + selectedPeerDeviceId_);
        }
        if (result.GetSkippedLineCount() > 0) {
            logDiagnostic("runtime.chat.history_more_skipped count="
                          + std::to_string(result.GetSkippedLineCount()));
        }
        requestUiRefresh();
    } catch (const std::exception& error) {
        setStartupError(error.what());
    }
}

bool RelayDeskRuntime::loadSelectedPeerMessagesAround(const std::string& messageId)
{
    if (selectedPeerDeviceId_.empty()
        || !storageAvailable_
        || messageId.empty()) {
        return false;
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        std::lock_guard lock(chatHistoryStorageMutex_);
        const auto history =
            relaydesk::storage::loadChatHistory(appPaths, selectedPeerDeviceId_);
        const auto& records = history.GetRecords();
        const auto target = std::find_if(
            records.begin(),
            records.end(),
            [&messageId](const relaydesk::storage::ChatMessageRecord& record) {
                return record.GetMessageId() == messageId;
            });
        if (target == records.end()) {
            return false;
        }

        const std::size_t targetIndex =
            static_cast<std::size_t>(target - records.begin());
        const std::size_t halfPage = kSelectedPeerMessagePageSize / 2u;
        const std::size_t firstIndex =
            targetIndex > halfPage ? targetIndex - halfPage : 0u;
        const std::size_t lastIndex =
            std::min(records.size(), firstIndex + kSelectedPeerMessagePageSize);
        selectedPeerMessages_.assign(records.begin() + firstIndex,
                                     records.begin() + lastIndex);
        selectedPeerHasMoreMessages_ = firstIndex > 0u;
        requestUiRefresh();
        return true;
    } catch (const std::exception& error) {
        setStartupError(error.what());
        return false;
    }
}

void RelayDeskRuntime::enqueueIncomingChatMessage(
    relaydesk::storage::ChatMessageRecord record)
{
    const std::string peerDeviceId =
        peerDeviceIdForRecord(record, localUser_.GetDeviceId());
    bool persisted = false;
    bool unreadCounted = false;
    if (storageAvailable_) {
        try {
            const auto appPaths = relaydesk::storage::createAppPaths();
            {
                std::lock_guard lock(chatHistoryStorageMutex_);
                relaydesk::storage::appendChatMessage(appPaths,
                                                      peerDeviceId,
                                                      record);
                persisted = true;
            }
            if (peerDeviceId != selectedPeerDeviceId_) {
                unreadCounted =
                    incrementPersistedPeerUnreadMessageCount(peerDeviceId);
            }
        } catch (const std::exception& error) {
            logDiagnostic("runtime.chat.persist_incoming_failed peer_device_id="
                          + peerDeviceId
                          + " message=" + error.what());
        }
    }

    {
        std::lock_guard lock(pendingChatMutex_);
        pendingChatMessages_.emplace_back(std::move(record),
                                          persisted,
                                          unreadCounted);
    }

    logDiagnostic("runtime.chat.enqueue_incoming peer_device_id=" + peerDeviceId);
    pendingUserNotificationCount_.fetch_add(1);
    notifyUserNotification();
    requestUiRefresh();
}

#if defined(RELAYDESK_HAS_BOOST_ASIO)
void RelayDeskRuntime::failIncomingTransferFromFrame(
    const relaydesk::net::PeerFrame& frame)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    std::optional<std::string> transferId;
    std::optional<PendingTransferStateUpdate> offerFailureUpdate;
    try {
        switch (frame.GetType()) {
        case relaydesk::net::PeerFrameType::TransferOffer: {
            const relaydesk::net::TransferOfferMessage offer =
                relaydesk::net::parseTransferOfferFrame(frame);
            PendingTransferStateUpdate update;
            update.SetPeerDeviceId(offer.GetSenderDeviceId());
            update.SetMessageId(offer.GetMessageId());
            update.SetPartId(offer.GetPartId());
            update.SetTransferId(offer.GetTransferId());
            update.SetTransferState(relaydesk::storage::TransferState::Failed);
            offerFailureUpdate = std::move(update);
            transferId = offer.GetTransferId();
            break;
        }
        case relaydesk::net::PeerFrameType::TransferChunk: {
            const relaydesk::net::TransferChunkMessage chunk =
                relaydesk::net::parseTransferChunkFrame(frame);
            transferId = chunk.GetTransferId();
            break;
        }
        case relaydesk::net::PeerFrameType::TransferComplete: {
            const relaydesk::net::TransferCompleteMessage complete =
                relaydesk::net::parseTransferCompleteFrame(frame);
            transferId = complete.GetTransferId();
            break;
        }
        default:
            return;
        }
    } catch (const std::exception&) {
        return;
    }

    if (!transferId.has_value()) {
        return;
    }

    std::optional<PendingIncomingTransfer> failedTransfer;
    {
        std::lock_guard lock(pendingTransferMutex_);
        const auto existing = pendingIncomingTransfers_.find(transferId.value());
        if (existing != pendingIncomingTransfers_.end()) {
            failedTransfer = existing->second;
            pendingIncomingTransfers_.erase(existing);
        }
    }

    if (failedTransfer.has_value()) {
        const auto appPaths = relaydesk::storage::createAppPaths();
        enqueueTransferUpdate(
            makeFailedIncomingTransferUpdate(appPaths, failedTransfer.value()));
        notifyIncomingTransferFailed(failedTransfer.value());
        return;
    }

    if (offerFailureUpdate.has_value()) {
        enqueueTransferStateUpdate(std::move(offerFailureUpdate.value()));
    }
#else
    (void)frame;
#endif
}

void RelayDeskRuntime::interruptPendingIncomingTransfers()
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    std::vector<PendingTransferUpdate> updates;
    const auto appPaths = relaydesk::storage::createAppPaths();
    {
        std::lock_guard lock(pendingTransferMutex_);
        for (const auto& [transferId, transfer] : pendingIncomingTransfers_) {
            (void)transferId;
            updates.push_back(
                makeInterruptedIncomingTransferUpdate(appPaths, transfer));
        }
        pendingIncomingTransfers_.clear();
    }

    for (auto& update : updates) {
        enqueueTransferUpdate(std::move(update));
    }
#endif
}

void RelayDeskRuntime::notifyIncomingTransferFailed(
    const PendingIncomingTransfer& transfer)
{
#if defined(RELAYDESK_HAS_BOOST_ASIO)
    const std::optional<PeerListItem> peer =
        findPeerByDeviceId(transfer.GetSenderDeviceId());
    if (!peer.has_value()
        || peer->GetAddress().empty()
        || peer->GetAddress() == "unknown"
        || peer->GetTcpPort() == 0) {
        return;
    }

    relaydesk::storage::ChatMessageRecord record;
    record.SetMessageId(transfer.GetMessageId());
    relaydesk::storage::ChatMessagePart part;
    part.SetPartId(transfer.GetPartId());
    part.SetType(transfer.GetFolderTransfer()
                     ? relaydesk::storage::MessagePartType::Folder
                     : relaydesk::storage::MessagePartType::File);
    part.SetTransferId(transfer.GetTransferId());
    const relaydesk::net::PeerFrame frame =
        relaydesk::net::makeTransferCancelFrame(
            makeTransferCancelMessage(record,
                                      part,
                                      localUser_.GetDeviceId(),
                                      "transfer_failed"));
    const bool accepted = ::core::async::runOnce(
        "relaydesk.transfer.fail_notify." + transfer.GetTransferId(),
        [this, peer = peer.value(), frame] {
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
                logDiagnostic("runtime.transfer.fail_notify_failed message="
                              + result.error);
            }
        });
    if (!accepted) {
        logDiagnostic(
            "runtime.transfer.fail_notify_failed message=task_not_accepted");
    }
#else
    (void)transfer;
#endif
}

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
        std::optional<std::filesystem::path> interruptedFinalPath;
        bool interruptedFolderTransfer = false;
        bool interruptedTransferFound = false;
        if (!existingTransfer.has_value()) {
            auto findInterruptedPart =
                [&](const relaydesk::storage::ChatMessageRecord& record) {
                    if (record.GetMessageId() != offer.GetMessageId()) {
                        return;
                    }
                    for (const auto& part : record.GetParts()) {
                        const bool partMatches =
                            part.GetPartId() == offer.GetPartId()
                            || (part.GetTransferId().has_value()
                                && part.GetTransferId().value()
                                    == offer.GetTransferId());
                        if (!partMatches
                            || !part.GetTransferState().has_value()
                            || part.GetTransferState().value()
                                != relaydesk::storage::TransferState::Interrupted) {
                            continue;
                        }
                        interruptedTransferFound = true;
                        interruptedFolderTransfer =
                            part.GetType()
                            == relaydesk::storage::MessagePartType::Folder;
                        if (part.GetLocalPath().has_value()
                            && !part.GetLocalPath().value().empty()) {
                            const std::filesystem::path localPath =
                                resolveLocalPath(appPaths,
                                                 part.GetLocalPath().value());
                            if (!isLegacyIncomingTempPath(appPaths, localPath)) {
                                interruptedFinalPath = localPath;
                            }
                        }
                        return;
                    }
                };
            if (selectedPeerDeviceId_ == offer.GetSenderDeviceId()) {
                for (const auto& record : selectedPeerMessages_) {
                    findInterruptedPart(record);
                    if (interruptedTransferFound) {
                        break;
                    }
                }
            }
            if (!interruptedFinalPath.has_value() && storageAvailable_) {
                try {
                    std::optional<relaydesk::storage::ChatMessageRecord> record;
                    {
                        std::lock_guard lock(chatHistoryStorageMutex_);
                        record = relaydesk::storage::loadChatMessage(
                            appPaths,
                            offer.GetSenderDeviceId(),
                            offer.GetMessageId());
                    }
                    if (record.has_value()) {
                        if (recoverInterruptedTransferParts(record.value())) {
                            std::lock_guard lock(chatHistoryStorageMutex_);
                            (void)relaydesk::storage::replaceChatMessage(
                                appPaths,
                                offer.GetSenderDeviceId(),
                                record.value());
                        }
                        findInterruptedPart(record.value());
                    }
                } catch (const std::exception& error) {
                    logDiagnostic("runtime.transfer.resume_history_failed message="
                                  + std::string(error.what()));
                }
            }
        }

        bool folderTransfer = offer.GetFolderTransfer();
        std::filesystem::path finalFilePath;
        if (existingTransfer.has_value()) {
            finalFilePath = existingTransfer->GetFinalFilePath();
            folderTransfer =
                existingTransfer->GetFolderTransfer() || folderTransfer;
        } else if (interruptedFinalPath.has_value()) {
            finalFilePath = interruptedFinalPath.value();
            folderTransfer = interruptedFolderTransfer || folderTransfer;
        } else if (!offer.GetImageTransfer()) {
            finalFilePath = makeIncomingFinalFilePath(appPaths,
                                                      offer.GetFileName());
        }

        const std::filesystem::path payloadPath =
            makeIncomingTransferPayloadPath(appPaths,
                                            offer.GetTransferId(),
                                            offer.GetFileName(),
                                            finalFilePath,
                                            folderTransfer,
                                            offer.GetImageTransfer());

        const bool resumeInterruptedTransfer =
            existingTransfer.has_value()
            || (offer.GetResumeRequest() && interruptedTransferFound);
        std::uintmax_t receivedSize = 0;
        if (resumeInterruptedTransfer) {
            if (folderTransfer) {
                receivedSize =
                    existingIncomingFolderPayloadSize(payloadPath,
                                                      offer.GetFileSize());
            } else {
                receivedSize =
                    existingIncomingPayloadSize(payloadPath, offer.GetFileSize());
                if (receivedSize == 0) {
                    receivedSize = migrateLegacyIncomingPayload(
                        appPaths,
                        payloadPath,
                        offer.GetTransferId(),
                        offer.GetFileName(),
                        offer.GetFileSize());
                }
            }
        }
        if (folderTransfer) {
            prepareIncomingFolderTransferRootForResume(payloadPath,
                                                       resumeInterruptedTransfer);
        } else if (receivedSize == 0) {
            createIncomingPayloadFile(payloadPath);
        }

        PendingIncomingTransfer transfer;
        transfer.SetSenderDeviceId(offer.GetSenderDeviceId());
        transfer.SetMessageId(offer.GetMessageId());
        transfer.SetPartId(offer.GetPartId());
        transfer.SetTransferId(offer.GetTransferId());
        transfer.SetFileName(offer.GetFileName());
        transfer.SetExpectedSize(offer.GetFileSize());
        transfer.SetReceivedSize(receivedSize);
        transfer.SetTempFilePath(payloadPath);
        transfer.SetFinalFilePath(finalFilePath);
        transfer.SetImageTransfer(offer.GetImageTransfer());
        transfer.SetFolderTransfer(folderTransfer);
        if (offer.GetResumeRequest()) {
            {
                std::lock_guard lock(pendingTransferMutex_);
                pendingIncomingTransfers_[offer.GetTransferId()] = transfer;
            }

            PendingTransferStateUpdate stateUpdate;
            stateUpdate.SetPeerDeviceId(offer.GetSenderDeviceId());
            stateUpdate.SetMessageId(offer.GetMessageId());
            stateUpdate.SetPartId(offer.GetPartId());
            stateUpdate.SetTransferId(offer.GetTransferId());
            stateUpdate.SetTransferState(
                relaydesk::storage::TransferState::Transferring);
            enqueueTransferStateUpdate(std::move(stateUpdate));
            if (receivedSize > 0) {
                enqueueTransferProgressUpdate(
                    makeTransferProgressUpdate(offer.GetMessageId(),
                                               offer.GetPartId(),
                                               offer.GetTransferId(),
                                               receivedSize));
            }
            const relaydesk::net::PeerFrame acceptFrame =
                makePendingTransferAcceptFrame(transfer,
                                               localUser_.GetDeviceId(),
                                               receivedSize);
            const std::string acceptTaskKey =
                "relaydesk.transfer.resume_accept." + offer.GetTransferId();
            const bool accepted = ::core::async::restart(
                acceptTaskKey,
                [this, senderDeviceId = offer.GetSenderDeviceId(), acceptFrame] {
                    try {
                        const std::optional<PeerListItem> sender =
                            findPeerByDeviceId(senderDeviceId);
                        if (!sender.has_value()
                            || sender->GetAddress().empty()
                            || sender->GetAddress() == "unknown"
                            || sender->GetTcpPort() == 0) {
                            return ::core::async::failure(
                                "Resume sender peer is not reachable.");
                        }
                        if (!tcpPeerTransport_) {
                            return ::core::async::failure(
                                "TCP peer transport is not available.");
                        }
                        tcpPeerTransport_->sendFrameTo(sender->GetAddress(),
                                                       sender->GetTcpPort(),
                                                       acceptFrame);
                        return ::core::async::success();
                    } catch (const std::exception& error) {
                        return ::core::async::failure(error.what());
                    }
                },
                [this](const ::core::async::Result<void>& result) {
                    if (!result.ok) {
                        logDiagnostic("runtime.transfer.resume_accept_failed message="
                                      + result.error);
                    }
                });
            if (!accepted) {
                logDiagnostic(
                    "runtime.transfer.resume_accept_failed message=task_not_accepted");
            }
            return;
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
        if (receivedSize > 0) {
            enqueueTransferProgressUpdate(
                makeTransferProgressUpdate(offer.GetMessageId(),
                                           offer.GetPartId(),
                                           offer.GetTransferId(),
                                           receivedSize));
        }
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
        request.SetResumeOffset(accept.GetResumeOffset());
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
        std::optional<PendingIncomingTransfer> cancelledIncomingTransfer;
        {
            std::lock_guard lock(pendingTransferMutex_);
            const auto transfer =
                pendingIncomingTransfers_.find(cancel.GetTransferId());
            if (transfer != pendingIncomingTransfers_.end()) {
                cancelledIncomingTransfer = transfer->second;
                pendingIncomingTransfers_.erase(transfer);
            }
        }
        (void)::core::async::cancel(
            "relaydesk.transfer.send." + cancel.GetTransferId());

        const relaydesk::storage::TransferState cancelState =
            cancel.GetReason() == "transfer_failed"
                ? relaydesk::storage::TransferState::Failed
                : relaydesk::storage::TransferState::Cancelled;

        if (cancelledIncomingTransfer.has_value()) {
            const auto appPaths = relaydesk::storage::createAppPaths();
            if (cancelState == relaydesk::storage::TransferState::Failed) {
                enqueueTransferUpdate(
                    makeFailedIncomingTransferUpdate(
                        appPaths,
                        cancelledIncomingTransfer.value()));
            } else {
                enqueueTransferUpdate(
                    makeCancelledIncomingTransferUpdate(
                        appPaths,
                        cancelledIncomingTransfer.value()));
            }
            return;
        }

        PendingTransferStateUpdate update;
        update.SetPeerDeviceId(cancel.GetCancellerDeviceId());
        update.SetMessageId(cancel.GetMessageId());
        update.SetPartId(cancel.GetPartId());
        update.SetTransferId(cancel.GetTransferId());
        update.SetTransferState(cancelState);
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

        const auto& body = frame.GetBody();
        if (transfer.GetFolderTransfer()) {
            writeIncomingFolderTransferChunk(transfer, chunk, body);
        } else {
            std::ofstream output(
                transfer.GetTempFilePath(),
                std::ios::binary | std::ios::app);
            if (!output) {
                throw std::runtime_error("Failed to write incoming transfer chunk.");
            }
            output.write(reinterpret_cast<const char*>(body.data()),
                         static_cast<std::streamsize>(body.size()));
            if (!output) {
                throw std::runtime_error("Failed to append incoming transfer chunk.");
            }
        }

        const std::uintmax_t receivedSize =
            transfer.GetReceivedSize() + body.size();
        if (receivedSize > transfer.GetExpectedSize()) {
            throw std::runtime_error(
                "Incoming transfer chunk exceeds expected size.");
        }
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
        std::filesystem::path finalFilePath = transfer.GetFinalFilePath();
        std::optional<std::string> sha256;
        bool payloadConsumed = false;
        if (transfer.GetFolderTransfer()) {
            finalFilePath = transfer.GetTempFilePath();
            payloadConsumed = true;
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
                    payloadConsumed = true;
                }
            } catch (const std::exception&) {
            }
        }

        if (!payloadConsumed) {
            if (finalFilePath.empty()) {
                finalFilePath = makeIncomingFinalFilePath(appPaths, transfer);
            }
            replaceIncomingTransferPayload(transfer.GetTempFilePath(),
                                           finalFilePath);
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
    std::vector<PendingChatMessage> pendingMessages;
    {
        std::lock_guard lock(pendingChatMutex_);
        pendingMessages.swap(pendingChatMessages_);
    }

    if (pendingMessages.empty()) {
        return;
    }

    for (auto& pendingMessage : pendingMessages) {
        try {
            auto& message = pendingMessage.GetRecord();
            const std::string peerDeviceId =
                peerDeviceIdForRecord(message, localUser_.GetDeviceId());
            if (pendingMessage.GetPersisted()) {
                if (peerDeviceId == selectedPeerDeviceId_) {
                    selectedPeerMessages_.push_back(message);
                } else if (message.GetDirection()
                           == relaydesk::storage::MessageDirection::Incoming) {
                    if (pendingMessage.GetUnreadCounted()) {
                        incrementPeerUnreadMessageCountInMemory(peerDeviceId);
                    } else {
                        incrementPeerUnreadMessageCount(peerDeviceId);
                    }
                }
                updatePeerLastConversationAt(peerDeviceId, message.GetCreatedAt());
                continue;
            }

            appendSelectedPeerMessage(peerDeviceId, message);
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
        std::lock_guard lock(chatHistoryStorageMutex_);
        record = relaydesk::storage::loadChatMessage(appPaths,
                                                     request.GetReceiverDeviceId(),
                                                     request.GetMessageId());
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
    if (!hasSendableTransferPartState(record.value(),
                                      request.GetMessageId(),
                                      request.GetPartId(),
                                      request.GetTransferId())) {
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
    if (request.GetResumeOffset() > 0) {
        (void)applyTransferPartProgressUpdate(
            transferringRecord,
            request.GetMessageId(),
            request.GetPartId(),
            request.GetTransferId(),
            request.GetResumeOffset());
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

    const std::string taskKey = "relaydesk.transfer.send." + request.GetTransferId();
    const bool accepted = ::core::async::restart(
        taskKey,
        [this, peer = *peer, transferringRecord, request](
            const ::core::async::CancelToken& token) {
            try {
                if (!tcpPeerTransport_) {
                    return ::core::async::failure(
                        "TCP peer transport is not available.");
                }
                if (token.canceled()) {
                    return ::core::async::failure("transfer_cancelled");
                }
                const auto appPaths = relaydesk::storage::createAppPaths();
                if (request.GetResumeRequestOnly()) {
                    sendRequestedFileTransferResumeRequest(*tcpPeerTransport_,
                                                           peer,
                                                           transferringRecord,
                                                           request.GetPartId(),
                                                           request.GetTransferId(),
                                                           appPaths);
                    return ::core::async::success();
                }
                sendRequestedFileTransferFrames(*tcpPeerTransport_,
                                                peer,
                                                transferringRecord,
                                                request.GetPartId(),
                                                request.GetTransferId(),
                                                appPaths,
                                                [this,
                                                 peerDeviceId =
                                                     request.GetReceiverDeviceId()](
                                                    const std::string& messageId,
                                                    const std::string& partId,
                                                    const std::string& transferId,
                                                    const std::string& fileName,
                                                    std::uintmax_t fileSize,
                                                    const std::string& localPath) {
                                                    enqueueTransferUpdate(
                                                        makePreparedOutgoingTransferUpdate(
                                                            peerDeviceId,
                                                            messageId,
                                                            partId,
                                                            transferId,
                                                            fileName,
                                                            fileSize,
                                                            localPath));
                                                },
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
                                                token,
                                                request.GetResumeOffset());
                if (token.canceled()) {
                    return ::core::async::failure("transfer_cancelled");
                }
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
            if (result.ok && request.GetResumeRequestOnly()) {
                return;
            }
            PendingTransferStateUpdate update;
            update.SetPeerDeviceId(request.GetReceiverDeviceId());
            update.SetMessageId(request.GetMessageId());
            update.SetPartId(request.GetPartId());
            update.SetTransferId(request.GetTransferId());
            update.SetTransferState(
                result.ok
                    ? relaydesk::storage::TransferState::Completed
                    : transferSendFailureState(result.error));
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
    {
        std::lock_guard lock(chatHistoryStorageMutex_);
        relaydesk::storage::appendChatMessage(appPaths, peerDeviceId, record);
    }
    if (peerDeviceId == selectedPeerDeviceId_) {
        selectedPeerMessages_.push_back(record);
    } else if (record.GetDirection()
               == relaydesk::storage::MessageDirection::Incoming) {
        incrementPeerUnreadMessageCount(peerDeviceId);
    }
    updatePeerLastConversationAt(peerDeviceId, record.GetCreatedAt());
}

void RelayDeskRuntime::persistChatMessageRecord(
    const std::string& peerDeviceId,
    const relaydesk::storage::ChatMessageRecord& record,
    bool replaceExistingRecord)
{
    const auto appPaths = relaydesk::storage::createAppPaths();
    bool replaced = false;
    {
        std::lock_guard lock(chatHistoryStorageMutex_);
        replaced = replaceExistingRecord
            && relaydesk::storage::replaceChatMessage(appPaths,
                                                      peerDeviceId,
                                                      record);
        if (!replaced) {
            relaydesk::storage::appendChatMessage(appPaths, peerDeviceId, record);
        }
    }
    updatePeerLastConversationAt(peerDeviceId, record.GetCreatedAt());
}

bool RelayDeskRuntime::updateChatMessageTransferPart(
    const PendingTransferUpdate& update)
{
    bool updatedPendingMessage = false;
    {
        std::lock_guard lock(pendingChatMutex_);
        for (auto& pendingMessage : pendingChatMessages_) {
            auto& message = pendingMessage.GetRecord();
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
        std::optional<relaydesk::storage::ChatMessageRecord> message =
            std::nullopt;
        {
            std::lock_guard lock(chatHistoryStorageMutex_);
            message = relaydesk::storage::loadChatMessage(appPaths,
                                                          update.GetPeerDeviceId(),
                                                          update.GetMessageId());
        }
        if (message.has_value()) {
            if (applyTransferPartUpdate(message.value(),
                                        update.GetMessageId(),
                                        update.GetPartId(),
                                        update.GetTransferId(),
                                        update.GetFileName(),
                                        update.GetFileSize(),
                                        update.GetLocalPath(),
                                        update.GetSha256(),
                                        update.GetTransferState())) {
                recordToPersist = std::move(message.value());
            }
        }
    }

    if (recordToPersist.has_value()) {
        const auto appPaths = relaydesk::storage::createAppPaths();
        std::lock_guard lock(chatHistoryStorageMutex_);
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
    const bool markDeliverySent =
        update.GetTransferState() == relaydesk::storage::TransferState::Rejected
        || update.GetTransferState()
            == relaydesk::storage::TransferState::Cancelled;
    bool updatedPendingMessage = false;
    {
        std::lock_guard lock(pendingChatMutex_);
        for (auto& pendingMessage : pendingChatMessages_) {
            auto& message = pendingMessage.GetRecord();
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
                    if (markDeliverySent) {
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
        std::optional<relaydesk::storage::ChatMessageRecord> message =
            std::nullopt;
        {
            std::lock_guard lock(chatHistoryStorageMutex_);
            message = relaydesk::storage::loadChatMessage(appPaths,
                                                          update.GetPeerDeviceId(),
                                                          update.GetMessageId());
        }
        if (message.has_value()) {
            if (applyTransferPartStateUpdate(message.value(),
                                             update.GetMessageId(),
                                             update.GetPartId(),
                                             update.GetTransferId(),
                                             update.GetTransferState())) {
                if (message->GetDirection()
                    == relaydesk::storage::MessageDirection::Outgoing) {
                    if (markDeliverySent) {
                        message->SetDeliveryState(
                            relaydesk::storage::DeliveryState::Sent);
                    }
                }
                recordToPersist = std::move(message.value());
            }
        }
    }

    if (recordToPersist.has_value()) {
        const auto appPaths = relaydesk::storage::createAppPaths();
        std::lock_guard lock(chatHistoryStorageMutex_);
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
        for (auto& pendingMessage : pendingChatMessages_) {
            auto& message = pendingMessage.GetRecord();
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
        std::lock_guard lock(chatHistoryStorageMutex_);
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

void RelayDeskRuntime::incrementPeerUnreadMessageCount(
    const std::string& peerDeviceId)
{
    const auto peer = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&peerDeviceId](const PeerListItem& item) {
            return item.GetDeviceId() == peerDeviceId;
        });
    const int unreadMessageCount = peer == peers_.end()
        ? 1
        : peer->GetUnreadMessageCount() + 1;
    savePeerUnreadMessageCount(peerDeviceId, unreadMessageCount);
}

void RelayDeskRuntime::incrementPeerUnreadMessageCountInMemory(
    const std::string& peerDeviceId)
{
    const auto peer = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&peerDeviceId](const PeerListItem& item) {
            return item.GetDeviceId() == peerDeviceId;
        });
    if (peer != peers_.end()) {
        peer->SetUnreadMessageCount(peer->GetUnreadMessageCount() + 1);
    }
}

void RelayDeskRuntime::clearPeerUnreadMessageCount(
    const std::string& peerDeviceId)
{
    if (peerDeviceId.empty()) {
        return;
    }

    const auto peer = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&peerDeviceId](const PeerListItem& item) {
            return item.GetDeviceId() == peerDeviceId;
        });
    if (peer != peers_.end() && peer->GetUnreadMessageCount() == 0) {
        return;
    }

    savePeerUnreadMessageCount(peerDeviceId, 0);
}

void RelayDeskRuntime::savePeerUnreadMessageCount(
    const std::string& peerDeviceId,
    int unreadMessageCount)
{
    if (peerDeviceId.empty() || unreadMessageCount < 0 || !storageAvailable_) {
        return;
    }

    const auto peer = std::find_if(
        peers_.begin(),
        peers_.end(),
        [&peerDeviceId](const PeerListItem& item) {
            return item.GetDeviceId() == peerDeviceId;
        });
    if (peer != peers_.end()) {
        peer->SetUnreadMessageCount(unreadMessageCount);
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::PeerProfile profile =
            relaydesk::storage::loadPeerProfile(appPaths, peerDeviceId);
        if (profile.GetUnreadMessageCount() == unreadMessageCount) {
            return;
        }
        profile.SetUnreadMessageCount(unreadMessageCount);
        relaydesk::storage::savePeerProfile(appPaths, profile);
    } catch (const std::exception& error) {
        logDiagnostic("runtime.peer.unread_save_failed device_id="
                      + peerDeviceId
                      + " message=" + error.what());
    }
}

bool RelayDeskRuntime::incrementPersistedPeerUnreadMessageCount(
    const std::string& peerDeviceId)
{
    if (peerDeviceId.empty() || !storageAvailable_) {
        return false;
    }

    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::PeerProfile profile =
            relaydesk::storage::loadPeerProfile(appPaths, peerDeviceId);
        profile.SetUnreadMessageCount(profile.GetUnreadMessageCount() + 1);
        relaydesk::storage::savePeerProfile(appPaths, profile);
        return true;
    } catch (const std::exception& error) {
        logDiagnostic("runtime.peer.unread_increment_failed device_id="
                      + peerDeviceId
                      + " message=" + error.what());
    }

    return false;
}

void RelayDeskRuntime::syncSelectedPeer()
{
    if (peers_.empty() || selectedPeerDeviceId_.empty()) {
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
        setSelectedPeerDeviceId({});
    }
}

void RelayDeskRuntime::setStartupError(std::string errorMessage)
{
    logDiagnostic("runtime.error message=" + errorMessage);
    startupErrorMessage_ = std::move(errorMessage);
}

void RelayDeskRuntime::notifyUserNotification()
{
    std::function<void()> handler;
    try {
        std::lock_guard lock(userNotificationMutex_);
        handler = userNotificationHandler_;
    } catch (const std::exception& error) {
        logDiagnostic("runtime.notification.handler_copy_failed message="
                      + std::string(error.what()));
        return;
    } catch (...) {
        logDiagnostic("runtime.notification.handler_copy_failed message=unknown");
        return;
    }

    if (!handler) {
        return;
    }

    try {
        handler();
    } catch (const std::exception& error) {
        logDiagnostic("runtime.notification.handler_failed message="
                      + std::string(error.what()));
    } catch (...) {
        logDiagnostic("runtime.notification.handler_failed message=unknown");
    }
}

void RelayDeskRuntime::requestUiRefresh()
{
    if (!runtimeOptions_.GetAsyncRefreshEnabled()) {
        return;
    }

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
    if (!runtimeOptions_.GetAsyncRefreshEnabled()) {
        return;
    }

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
