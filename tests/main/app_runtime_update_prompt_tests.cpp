#include "core/app_version.h"
#include "main/app_runtime.h"
#include "storage/app_paths.h"
#include "storage/peer_profile.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

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

} // namespace

int main()
{
    return offersAppUpdatePromptWhenOnlinePeerVersionIncreases();
}
