#include "platform/startup_launch_detail.h"

#include <algorithm>
#include <cwctype>
#include <string_view>

namespace relaydesk::platform::detail {
namespace {

constexpr std::uint8_t kStartupApprovedEnabled = 0x02u;

std::wstring_view trimLeadingWhitespace(std::wstring_view value)
{
    const auto first = std::find_if_not(
        value.begin(),
        value.end(),
        [](wchar_t character) { return std::iswspace(character) != 0; });
    value.remove_prefix(static_cast<std::size_t>(first - value.begin()));
    return value;
}

bool hasExecutableSuffixAt(std::wstring_view value, std::size_t position)
{
    constexpr std::wstring_view kExecutableSuffix = L".exe";
    if (position + kExecutableSuffix.size() > value.size()) {
        return false;
    }

    for (std::size_t index = 0; index < kExecutableSuffix.size(); ++index) {
        if (std::towlower(value[position + index])
            != std::towlower(kExecutableSuffix[index])) {
            return false;
        }
    }

    const std::size_t end = position + kExecutableSuffix.size();
    return end == value.size() || std::iswspace(value[end]) != 0;
}

std::wstring_view startupExecutableToken(std::wstring_view command)
{
    command = trimLeadingWhitespace(command);
    if (command.empty()) {
        return {};
    }

    if (command.front() == L'\"') {
        command.remove_prefix(1u);
        const std::size_t closingQuote = command.find(L'\"');
        return command.substr(0u, closingQuote);
    }

    for (std::size_t index = 0; index < command.size(); ++index) {
        if (hasExecutableSuffixAt(command, index)) {
            return command.substr(0u, index + 4u);
        }
    }

    const std::size_t firstWhitespace = command.find_first_of(L" \t\r\n");
    return command.substr(0u, firstWhitespace);
}

std::wstring normalizedWindowsPath(const std::filesystem::path& path)
{
    std::wstring value = path.lexically_normal().wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.starts_with(L"\\\\?\\")) {
        value.erase(0u, 4u);
    }
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

bool commandsCurrentExecutable(std::wstring_view command,
                               const std::filesystem::path& executablePath)
{
    const std::wstring_view executableToken = startupExecutableToken(command);
    if (executableToken.empty()) {
        return false;
    }

    return normalizedWindowsPath(std::filesystem::path(executableToken))
        == normalizedWindowsPath(executablePath);
}

bool isDisabledApproval(
    const std::optional<std::vector<std::uint8_t>>& startupApproval)
{
    return startupApproval.has_value()
        && (startupApproval->empty()
            || startupApproval->front() != kStartupApprovedEnabled);
}

} // namespace

StartupLaunchState determineStartupLaunchState(
    const std::optional<std::wstring>& startupCommand,
    const std::optional<std::vector<std::uint8_t>>& startupApproval,
    const std::filesystem::path& executablePath)
{
    if (!startupCommand.has_value()) {
        return StartupLaunchState::NotRegistered;
    }
    if (isDisabledApproval(startupApproval)) {
        return StartupLaunchState::DisabledBySystem;
    }
    if (!commandsCurrentExecutable(*startupCommand, executablePath)) {
        return StartupLaunchState::DifferentExecutable;
    }
    return StartupLaunchState::Enabled;
}

}
