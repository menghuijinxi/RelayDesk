#include "storage/app_paths.h"

#include <filesystem>
#include <iostream>

namespace {

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

}

int main()
{
    const std::filesystem::path executablePath =
        LR"(C:\RelayDesk\bin\relaydesk.exe)";
    const relaydesk::storage::AppPaths appPaths(executablePath);

    if (appPaths.GetExecutablePath() != executablePath) {
        return fail("Executable path mismatch.");
    }

    if (appPaths.GetWorkDirectory() != LR"(C:\RelayDesk\bin)") {
        return fail("Work directory mismatch.");
    }

    if (appPaths.GetDataDirectory() != LR"(C:\RelayDesk\bin\data)") {
        return fail("Data directory mismatch.");
    }

    if (appPaths.GetInboxDirectory() != LR"(C:\RelayDesk\bin\data\transfers\inbox)") {
        return fail("Inbox directory mismatch.");
    }

    if (appPaths.GetOutboxDirectory() != LR"(C:\RelayDesk\bin\data\transfers\outbox)") {
        return fail("Outbox directory mismatch.");
    }

    if (appPaths.GetTempTransfersDirectory() != LR"(C:\RelayDesk\bin\data\transfers\temp)") {
        return fail("Temporary transfer directory mismatch.");
    }

    return 0;
}

