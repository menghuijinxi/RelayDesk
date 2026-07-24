#include "platform/startup_launch_detail.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using relaydesk::platform::StartupLaunchState;

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int expectState(StartupLaunchState actual,
                StartupLaunchState expected,
                const char* message)
{
    if (actual != expected) {
        return fail(message);
    }
    return 0;
}

StartupLaunchState determineState(
    const std::optional<std::wstring>& startupCommand,
    const std::optional<std::vector<std::uint8_t>>& startupApproval =
        std::nullopt)
{
    return relaydesk::platform::detail::determineStartupLaunchState(
        startupCommand,
        startupApproval,
        L"D:\\Apps\\RelayDesk\\relaydesk_skiaui.exe");
}

int recognizesEnabledStartupEntry()
{
    if (const int result = expectState(
            determineState(
                L"\"D:\\Apps\\RelayDesk\\relaydesk_skiaui.exe\""),
            StartupLaunchState::Enabled,
            "Matching startup entry without approval data was not enabled.");
        result != 0) {
        return result;
    }

    return expectState(
        determineState(
            L"\"D:\\Apps\\RelayDesk\\relaydesk_skiaui.exe\"",
            std::vector<std::uint8_t>{0x02u, 0x00u, 0x00u, 0x00u}),
        StartupLaunchState::Enabled,
        "Windows-approved startup entry was not enabled.");
}

int recognizesWindowsDisabledStartupEntry()
{
    return expectState(
        determineState(
            L"\"D:\\Apps\\RelayDesk\\relaydesk_skiaui.exe\"",
            std::vector<std::uint8_t>{0x03u, 0x00u, 0x00u, 0x00u}),
        StartupLaunchState::DisabledBySystem,
        "Windows-disabled startup entry was not recognized.");
}

int recognizesMissingStartupEntry()
{
    return expectState(
        determineState(std::nullopt,
                       std::vector<std::uint8_t>{0x03u}),
        StartupLaunchState::NotRegistered,
        "Missing Run value was not recognized as unregistered.");
}

int recognizesDifferentExecutable()
{
    return expectState(
        determineState(L"\"D:\\Apps\\RelayDesk\\relaydesk.exe\""),
        StartupLaunchState::DifferentExecutable,
        "Old RelayDesk executable was not recognized as different.");
}

int acceptsEquivalentWindowsCommands()
{
    if (const int result = expectState(
            determineState(
                L"  \"d:/apps/relaydesk/RELAYDESK_SKIAUI.EXE\" --background"),
            StartupLaunchState::Enabled,
            "Equivalent quoted path with arguments was not enabled.");
        result != 0) {
        return result;
    }

    return expectState(
        relaydesk::platform::detail::determineStartupLaunchState(
            L"D:\\Program Files\\RelayDesk\\relaydesk_skiaui.exe "
            L"--background",
            std::nullopt,
            L"D:\\Program Files\\RelayDesk\\relaydesk_skiaui.exe"),
        StartupLaunchState::Enabled,
        "Unquoted executable path containing spaces was not enabled.");
}

int treatsUnknownApprovalDataAsDisabled()
{
    if (const int result = expectState(
            determineState(
                L"\"D:\\Apps\\RelayDesk\\relaydesk_skiaui.exe\"",
                std::vector<std::uint8_t>{}),
            StartupLaunchState::DisabledBySystem,
            "Empty approval data was not handled conservatively.");
        result != 0) {
        return result;
    }

    return expectState(
        determineState(
            L"\"D:\\Apps\\RelayDesk\\relaydesk_skiaui.exe\"",
            std::vector<std::uint8_t>{0x7fu}),
        StartupLaunchState::DisabledBySystem,
        "Unknown approval data was not handled conservatively.");
}

} // namespace

int main()
{
    if (const int result = recognizesEnabledStartupEntry(); result != 0) {
        return result;
    }
    if (const int result = recognizesWindowsDisabledStartupEntry();
        result != 0) {
        return result;
    }
    if (const int result = recognizesMissingStartupEntry(); result != 0) {
        return result;
    }
    if (const int result = recognizesDifferentExecutable(); result != 0) {
        return result;
    }
    if (const int result = acceptsEquivalentWindowsCommands(); result != 0) {
        return result;
    }
    if (const int result = treatsUnknownApprovalDataAsDisabled(); result != 0) {
        return result;
    }

    return 0;
}
