#include "main/app_runtime.h"

#include "core/diagnostic_log.h"
#include "core/platform/async.h"
#include "core/time.h"
#include "core/uuid.h"
#include "platform/computer_name.h"
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
#include <cctype>
#include <chrono>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace relaydesk::runtime {
namespace {

using namespace std::chrono_literals;

constexpr const char* kDiscoveryLogFileName = "discovery.log";
constexpr auto kPeerOnlineTimeout = 10s;
constexpr auto kPeerStatusRefreshInterval = 1s;
#if defined(RELAYDESK_HAS_BOOST_ASIO)
constexpr std::uintmax_t kTransferChunkSize = 256u * 1024u;
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
    item.SetOnline(online);
    return item;
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
        || part.GetType() == relaydesk::storage::MessagePartType::File;
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

relaydesk::storage::ChatMessageRecord recordWithTransferState(
    relaydesk::storage::ChatMessageRecord record,
    relaydesk::storage::TransferState transferState)
{
    std::vector<relaydesk::storage::ChatMessagePart> parts = record.GetParts();
    for (auto& part : parts) {
        if (isFileTransferPart(part)) {
            part.SetTransferState(transferState);
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

        part.SetTransferId(transferId);
        part.SetTransferState(transferState);
        part.SetFileName(fileName);
        part.SetFileSize(fileSize);
        part.SetLocalPath(localPath);
        updated = true;
        break;
    }

    if (updated) {
        record.SetParts(std::move(parts));
    }
    return updated;
}

#if defined(RELAYDESK_HAS_BOOST_ASIO)
std::filesystem::path resolveLocalPath(
    const relaydesk::storage::AppPaths& appPaths,
    const std::string& pathText)
{
    std::filesystem::path path(pathText);
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
        return path.string();
    }
    return relativePath.generic_string();
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
        / sanitizeFileName(transferId)
        / (sanitizeFileName(fileName) + ".part");
}

std::filesystem::path makeIncomingFinalFilePath(
    const relaydesk::storage::AppPaths& appPaths,
    const PendingIncomingTransfer& transfer)
{
    return appPaths.GetInboxDirectory()
        / sanitizeFileName(transfer.GetSenderDeviceId())
        / sanitizeFileName(transfer.GetTransferId())
        / sanitizeFileName(transfer.GetFileName());
}

std::string chooseTransferFileName(
    const relaydesk::storage::ChatMessagePart& part,
    const std::filesystem::path& localPath)
{
    const std::string pathFileName = localPath.filename().string();
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
void sendRecordTransferFrames(
    TcpPeerTransportHandle& transport,
    const PeerListItem& peer,
    const relaydesk::storage::ChatMessageRecord& record,
    const relaydesk::storage::AppPaths& appPaths)
{
    std::vector<std::uint8_t> buffer(
        static_cast<std::size_t>(kTransferChunkSize));
    for (const auto& part : record.GetParts()) {
        if (!isFileTransferPart(part)
            || !part.GetTransferId().has_value()
            || !part.GetLocalPath().has_value()) {
            continue;
        }

        const std::filesystem::path localPath =
            resolveLocalPath(appPaths, part.GetLocalPath().value());
        if (!std::filesystem::is_regular_file(localPath)) {
            throw std::runtime_error("Transfer source file does not exist.");
        }

        const std::string fileName =
            chooseTransferFileName(part, localPath);
        const std::uintmax_t fileSize = std::filesystem::file_size(localPath);
        transport.sendFrameTo(
            peer.GetAddress(),
            peer.GetTcpPort(),
            relaydesk::net::makeTransferOfferFrame(
                makeTransferOfferMessage(record, part, fileName, fileSize)));

        std::ifstream input(localPath, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Failed to open transfer source file.");
        }

        std::uintmax_t offset = 0;
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const std::streamsize readSize = input.gcount();
            if (readSize <= 0) {
                break;
            }

            std::vector<std::uint8_t> chunk(
                buffer.begin(),
                buffer.begin() + readSize);
            transport.sendFrameTo(
                peer.GetAddress(),
                peer.GetTcpPort(),
                relaydesk::net::makeTransferChunkFrame(
                    makeTransferChunkMessage(record, part, offset),
                    std::move(chunk)));
            offset += static_cast<std::uintmax_t>(readSize);
        }

        if (offset != fileSize) {
            throw std::runtime_error("Transfer source file changed while sending.");
        }

        transport.sendFrameTo(
            peer.GetAddress(),
            peer.GetTcpPort(),
            relaydesk::net::makeTransferCompleteFrame(
                makeTransferCompleteMessage(record, part, fileSize)));
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

void PeerListItem::SetTcpPort(std::uint16_t tcpPort)
{
    tcpPort_ = tcpPort;
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

RelayDeskRuntime::~RelayDeskRuntime() = default;

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

void RelayDeskRuntime::refreshPeersIfNeeded()
{
    if (!storageAvailable_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    drainPendingPeerProfiles();
    drainPendingChatMessages();
    drainPendingTransferUpdates();
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
        [this, peer, frame, record] {
            try {
                if (!tcpPeerTransport_) {
                    return ::core::async::failure(
                        "TCP peer transport is not available.");
                }
                tcpPeerTransport_->sendFrameTo(peer.GetAddress(),
                                               peer.GetTcpPort(),
                                               frame);
                const auto appPaths = relaydesk::storage::createAppPaths();
                sendRecordTransferFrames(*tcpPeerTransport_,
                                         peer,
                                         record,
                                         appPaths);
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
            record = recordWithTransferState(
                std::move(record),
                result.ok
                    ? relaydesk::storage::TransferState::Completed
                    : relaydesk::storage::TransferState::Failed);
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

void RelayDeskRuntime::initialize()
{
    try {
        const auto appPaths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(appPaths);
        diagnosticLogFilePath_ = makeDiscoveryLogFilePath(appPaths);
        logDiagnostic("runtime.initialize.begin");

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
            nextPeers.push_back(makePeerListItem(profile, false));
        }
        peers_ = std::move(nextPeers);
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
    case relaydesk::net::PeerFrameType::TransferOffer: {
        const relaydesk::net::TransferOfferMessage offer =
            relaydesk::net::parseTransferOfferFrame(frame);
        const auto appPaths = relaydesk::storage::createAppPaths();
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
        {
            std::lock_guard lock(pendingTransferMutex_);
            pendingIncomingTransfers_[offer.GetTransferId()] = std::move(transfer);
        }
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
        const std::filesystem::path finalFilePath =
            makeIncomingFinalFilePath(appPaths, transfer);
        std::filesystem::create_directories(finalFilePath.parent_path());
        std::error_code error;
        std::filesystem::remove(finalFilePath, error);
        error.clear();
        std::filesystem::rename(transfer.GetTempFilePath(), finalFilePath, error);
        if (error) {
            throw std::runtime_error("Failed to finalize incoming transfer file.");
        }

        PendingTransferUpdate update;
        update.SetPeerDeviceId(transfer.GetSenderDeviceId());
        update.SetMessageId(transfer.GetMessageId());
        update.SetPartId(transfer.GetPartId());
        update.SetTransferId(transfer.GetTransferId());
        update.SetFileName(transfer.GetFileName());
        update.SetFileSize(transfer.GetExpectedSize());
        update.SetLocalPath(makeWorkRelativePath(appPaths, finalFilePath));
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

void RelayDeskRuntime::appendSelectedPeerMessage(
    const std::string& peerDeviceId,
    const relaydesk::storage::ChatMessageRecord& record)
{
    const auto appPaths = relaydesk::storage::createAppPaths();
    relaydesk::storage::appendChatMessage(appPaths, peerDeviceId, record);
    if (peerDeviceId == selectedPeerDeviceId_) {
        selectedPeerMessages_.push_back(record);
    }
}

void RelayDeskRuntime::persistChatMessageRecord(
    const std::string& peerDeviceId,
    const relaydesk::storage::ChatMessageRecord& record,
    bool replaceExistingRecord)
{
    const auto appPaths = relaydesk::storage::createAppPaths();
    if (replaceExistingRecord
        && relaydesk::storage::replaceChatMessage(appPaths, peerDeviceId, record)) {
        return;
    }

    relaydesk::storage::appendChatMessage(appPaths, peerDeviceId, record);
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

    PeerListItem item = makePeerListItem(profile, online);
    if (online) {
        item.SetLastOnlineSignalAt(now);
    }
    if (existing == peers_.end()) {
        peers_.push_back(item);
        logDiagnostic("runtime.peer.apply action=added device_id="
                      + profile.GetDeviceId()
                      + " address=" + item.GetAddress()
                      + " online=" + std::to_string(online)
                      + " last_seen=" + item.GetLastSeenAt()
                      + " stale_removed="
                      + std::to_string(stalePeerCount));
        return;
    }

    const bool wasOnline = existing->GetOnline();
    const std::string previousLastSeenAt = existing->GetLastSeenAt();
    *existing = item;
    logDiagnostic("runtime.peer.apply action=updated device_id="
                  + profile.GetDeviceId()
                  + " address=" + item.GetAddress()
                  + " online=" + std::to_string(online)
                  + " was_online=" + std::to_string(wasOnline)
                  + " incoming_last_seen=" + item.GetLastSeenAt()
                  + " previous_last_seen=" + previousLastSeenAt
                  + " stale_removed=" + std::to_string(stalePeerCount));
}

void RelayDeskRuntime::refreshPeerOnlineStates()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& peer : peers_) {
        const bool wasOnline = peer.GetOnline();
        const bool online = isPeerOnline(peer.GetLastOnlineSignalAt(), now);
        peer.SetOnline(online);
        if (wasOnline != online) {
            logDiagnostic("runtime.peer.online_changed device_id="
                          + peer.GetDeviceId()
                          + " online=" + std::to_string(online));
        }
    }
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
