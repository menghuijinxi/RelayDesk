#include "platform/computer_name.h"
#include "storage/app_paths.h"

#include <exception>
#include <iostream>

int main()
{
    try {
        const auto paths = relaydesk::storage::createAppPaths();
        relaydesk::storage::ensureAppDirectories(paths);

        std::wcout << L"RelayDesk bootstrap" << L'\n';
        std::wcout << L"Computer name: " << relaydesk::platform::getComputerName() << L'\n';
        std::wcout << L"Work directory: " << paths.GetWorkDirectory().wstring() << L'\n';
        std::wcout << L"Data directory: " << paths.GetDataDirectory().wstring() << L'\n';

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RelayDesk startup failed: " << error.what() << '\n';
        return 1;
    }
}

