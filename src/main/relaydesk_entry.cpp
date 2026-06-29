#include "main/app_runtime.h"

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

int relaydesk_eui_main();

namespace {

std::vector<std::wstring> currentProcessArguments()
{
#if defined(_WIN32)
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
#else
    return {};
#endif
}

} // namespace

int main()
{
    try {
        const std::vector<std::wstring> arguments = currentProcessArguments();
        const std::optional<relaydesk::runtime::AppUpdateApplyOptions>
            updateOptions =
                relaydesk::runtime::parseAppUpdateApplyOptions(arguments);
        if (updateOptions.has_value()) {
            return relaydesk::runtime::runAppUpdateApplyMode(
                updateOptions.value());
        }
        if (!arguments.empty()
            && arguments.front() == L"--relaydesk-apply-update") {
            return 2;
        }
        return relaydesk_eui_main();
    } catch (const std::exception&) {
        return 1;
    }
}
