#include "net/boost_asio_udp_discovery_transport.h"
#include "net/discovery_service.h"
#include "net/discovery_worker.h"
#include "platform/text_encoding.h"
#include "storage/app_paths.h"
#include "storage/local_identity.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <windows.h>

namespace {

using namespace std::chrono_literals;

constexpr auto kChildReadyTimeout = 5s;
constexpr auto kChildExitTimeout = 10s;
constexpr auto kDiscoveryTimeout = 6s;

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

std::filesystem::path testRoot()
{
    return std::filesystem::path(RELAYDESK_DISCOVERY_PROCESS_TEST_WORK_DIR);
}

relaydesk::storage::LocalIdentity makeIdentity(const std::string& deviceId,
                                               const std::string& displayName)
{
    return relaydesk::storage::LocalIdentity(deviceId,
                                             deviceId + "-install",
                                             "2026-06-13T00:00:00Z",
                                             displayName + "-HOST",
                                             displayName);
}

relaydesk::net::DiscoveryServiceConfig makeServiceConfig(
    std::uint16_t discoveryUdpPort)
{
    relaydesk::net::DiscoveryServiceConfig config;
    config.SetDiscoveryUdpPort(discoveryUdpPort);
    config.SetAdvertisedTcpPort(39171);
    config.SetCapabilities({"text", "file"});
    return config;
}

relaydesk::net::DiscoveryWorkerConfig makeWorkerConfig()
{
    relaydesk::net::DiscoveryWorkerConfig config;
    config.SetBroadcastEnabled(true);
    config.SetAnnounceOnStart(false);
    config.SetStartupBroadcastCount(0);
    config.SetPollTimeout(20ms);
    config.SetBroadcastInterval(60s);
    return config;
}

std::uint16_t readPortFile(const std::filesystem::path& filePath)
{
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open port file.");
    }

    unsigned int port = 0;
    input >> port;
    if (port == 0 || port > 65535u) {
        throw std::runtime_error("Port file contains invalid port.");
    }
    return static_cast<std::uint16_t>(port);
}

void writePortFile(const std::filesystem::path& filePath, std::uint16_t port)
{
    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    output << port << '\n';
    if (!output) {
        throw std::runtime_error("Failed to write port file.");
    }
}

bool waitForPortFile(const std::filesystem::path& filePath,
                     std::chrono::steady_clock::time_point deadline)
{
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(filePath)) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

bool hasWorkerStoredPeer(relaydesk::net::DiscoveryWorker& worker)
{
    return worker.GetStats().GetStoredPeerCount() > 0;
}

bool senderReceivedPeerReply(const relaydesk::net::DiscoveryServicePollResult& result,
                             const std::string& peerDeviceId)
{
    return result.GetAction() == relaydesk::net::DiscoveryServicePollAction::StoredPeer
        && result.HasPeerProfile()
        && result.GetPeerProfile().value().GetDeviceId() == peerDeviceId;
}

void printDiscoveryLog(const std::filesystem::path& workDirectory,
                       const std::string& label)
{
    const auto logFilePath =
        workDirectory / "worker" / "data" / "logs" / "discovery.log";
    std::ifstream input(logFilePath, std::ios::binary);
    if (!input) {
        std::cerr << label << " discovery log not found: "
                  << logFilePath.string() << '\n';
        return;
    }

    std::cerr << "==== " << label << " discovery.log ====\n";
    std::string line;
    while (std::getline(input, line)) {
        std::cerr << line << '\n';
    }
}

int runChild(int argc, char** argv)
{
    if (argc != 8) {
        return fail("Invalid child argument count.");
    }

    const std::filesystem::path workDirectory(argv[2]);
    const std::string deviceId = argv[3];
    const std::string displayName = argv[4];
    const std::string peerDeviceId = argv[5];
    const std::filesystem::path readyFile(argv[6]);
    const std::filesystem::path peerPortFile(argv[7]);

    const auto workerPaths = relaydesk::storage::AppPaths(
        workDirectory / "worker" / "relaydesk.exe");
    const auto senderPaths = relaydesk::storage::AppPaths(
        workDirectory / "sender" / "relaydesk.exe");
    relaydesk::storage::ensureAppDirectories(workerPaths);
    relaydesk::storage::ensureAppDirectories(senderPaths);

    relaydesk::net::DiscoveryService workerService(
        workerPaths,
        makeIdentity(deviceId, displayName),
        makeServiceConfig(0));
    relaydesk::net::DiscoveryWorker worker(
        std::move(workerService),
        makeWorkerConfig());
    worker.start();
    writePortFile(readyFile, worker.GetLocalUdpPort());

    const auto readyDeadline = std::chrono::steady_clock::now()
        + kChildReadyTimeout;
    if (!waitForPortFile(peerPortFile, readyDeadline)) {
        worker.stop();
        return fail("Peer port file was not created.");
    }
    const std::uint16_t peerPort = readPortFile(peerPortFile);

    relaydesk::net::DiscoveryService senderService(
        senderPaths,
        makeIdentity(deviceId, displayName),
        makeServiceConfig(0));

    bool workerStoredPeer = false;
    bool replyReceived = false;
    auto nextHelloAt = std::chrono::steady_clock::now();
    const auto discoveryDeadline = std::chrono::steady_clock::now()
        + kDiscoveryTimeout;
    while (std::chrono::steady_clock::now() < discoveryDeadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextHelloAt) {
            senderService.sendAnnouncementTo("127.0.0.1", peerPort);
            nextHelloAt = now + 100ms;
        }

        const auto result = senderService.pollOnce(20ms);
        if (senderReceivedPeerReply(result, peerDeviceId)) {
            replyReceived = true;
        }
        if (hasWorkerStoredPeer(worker)) {
            workerStoredPeer = true;
        }
        if (workerStoredPeer && replyReceived) {
            break;
        }
    }

    worker.stop();
    senderService.close();
    if (!workerStoredPeer) {
        return fail("Worker did not receive peer hello.");
    }
    if (!replyReceived) {
        return fail("Sender did not receive peer reply.");
    }
    return 0;
}

std::string quoteArgument(const std::string& argument)
{
    std::string quoted = "\"";
    for (char value : argument) {
        if (value == '"') {
            quoted += "\\\"";
        } else {
            quoted += value;
        }
    }
    quoted += '"';
    return quoted;
}

std::string makeChildCommandLine(const std::filesystem::path& executablePath,
                                 const std::filesystem::path& workDirectory,
                                 const std::string& deviceId,
                                 const std::string& displayName,
                                 const std::string& peerDeviceId,
                                 const std::filesystem::path& readyFile,
                                 const std::filesystem::path& peerPortFile)
{
    return quoteArgument(executablePath.string())
        + " --child"
        + " " + quoteArgument(workDirectory.string())
        + " " + quoteArgument(deviceId)
        + " " + quoteArgument(displayName)
        + " " + quoteArgument(peerDeviceId)
        + " " + quoteArgument(readyFile.string())
        + " " + quoteArgument(peerPortFile.string());
}

PROCESS_INFORMATION startChildProcess(const std::string& commandLine)
{
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLineWide = relaydesk::platform::utf8ToWide(commandLine);
    const BOOL started = CreateProcessW(nullptr,
                                        commandLineWide.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startupInfo,
                                        &processInfo);
    if (started == FALSE) {
        throw std::runtime_error("Failed to start child process.");
    }

    CloseHandle(processInfo.hThread);
    return processInfo;
}

DWORD waitForChildExit(PROCESS_INFORMATION& processInfo)
{
    const DWORD waitResult = WaitForSingleObject(
        processInfo.hProcess,
        static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
            kChildExitTimeout).count()));
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(processInfo.hProcess, 2);
        CloseHandle(processInfo.hProcess);
        return 2;
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hProcess);
    return exitCode;
}

int runParent(const std::filesystem::path& executablePath)
{
    const auto root = testRoot();
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto aWorkDirectory = root / "child-a";
    const auto bWorkDirectory = root / "child-b";
    const auto aReadyFile = root / "child-a.port";
    const auto bReadyFile = root / "child-b.port";
    const auto aPeerPortFile = root / "child-a-peer.port";
    const auto bPeerPortFile = root / "child-b-peer.port";

    PROCESS_INFORMATION childA = startChildProcess(
        makeChildCommandLine(executablePath,
                             aWorkDirectory,
                             "process-a-device",
                             "Process-A",
                             "process-b-device",
                             aReadyFile,
                             aPeerPortFile));
    PROCESS_INFORMATION childB = startChildProcess(
        makeChildCommandLine(executablePath,
                             bWorkDirectory,
                             "process-b-device",
                             "Process-B",
                             "process-a-device",
                             bReadyFile,
                             bPeerPortFile));

    const auto readyDeadline = std::chrono::steady_clock::now()
        + kChildReadyTimeout;
    if (!waitForPortFile(aReadyFile, readyDeadline)
        || !waitForPortFile(bReadyFile, readyDeadline)) {
        TerminateProcess(childA.hProcess, 3);
        TerminateProcess(childB.hProcess, 3);
        CloseHandle(childA.hProcess);
        CloseHandle(childB.hProcess);
        return fail("Child discovery workers did not publish their ports.");
    }

    writePortFile(aPeerPortFile, readPortFile(bReadyFile));
    writePortFile(bPeerPortFile, readPortFile(aReadyFile));

    const DWORD childAExitCode = waitForChildExit(childA);
    const DWORD childBExitCode = waitForChildExit(childB);
    if (childAExitCode != 0 || childBExitCode != 0) {
        printDiscoveryLog(aWorkDirectory, "child-a");
        printDiscoveryLog(bWorkDirectory, "child-b");
        return fail("Discovery process smoke test failed. child-a="
                    + std::to_string(childAExitCode)
                    + " child-b=" + std::to_string(childBExitCode));
    }

    std::filesystem::remove_all(root);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--child") {
        return runChild(argc, argv);
    }

    return runParent(std::filesystem::absolute(argv[0]));
}
