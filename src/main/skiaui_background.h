#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <memory>

namespace relaydesk::runtime {
class RelayDeskRuntime;
}

namespace relaydesk::skiaui {

class SingleInstanceGuard {
public:
    SingleInstanceGuard();
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    [[nodiscard]] bool alreadyRunning() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

void activateExistingInstance();
void syncLaunchAtStartupOnAppStart();

class BackgroundController {
public:
    explicit BackgroundController(HINSTANCE instance);
    ~BackgroundController();

    BackgroundController(const BackgroundController&) = delete;
    BackgroundController& operator=(const BackgroundController&) = delete;

    void attachRuntime(relaydesk::runtime::RelayDeskRuntime& runtime);
    void detachRuntime();
    bool handleMainWindowMessage(HWND window,
                                 UINT message,
                                 WPARAM wParam,
                                 LPARAM lParam);
    void processRuntimeState();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace relaydesk::skiaui
