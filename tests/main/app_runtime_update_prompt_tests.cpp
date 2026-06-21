#include "core/app_version.h"
#include "main/app_runtime.h"
#include "net/peer_message.h"
#include "storage/app_paths.h"
#include "storage/history_store.h"
#include "storage/peer_profile.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

class TestableRelayDeskRuntime : public relaydesk::runtime::RelayDeskRuntime {
public:
    TestableRelayDeskRuntime()
        : RelayDeskRuntime(makeTestRuntimeOptions())
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

} // namespace

int main()
{
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
    return restartsInterruptedIncomingTransferWhenFreshOfferStartsAtZero();
}
