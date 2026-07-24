#include "main/skiaui_background.h"

#include "main/app_runtime.h"
#include "platform/startup_launch.h"
#include "storage/app_paths.h"
#include "storage/ui_preferences.h"

#include <array>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

#include <mmsystem.h>
#include <shellapi.h>

namespace relaydesk::skiaui {
namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\RelayDesk.SingleInstance";
constexpr wchar_t kTrayWindowClassName[] = L"RelayDeskTrayWindow";
constexpr wchar_t kMainWindowTitle[] = L"RelayDesk";
constexpr UINT kTrayCallbackMessage = WM_APP + 0x532;
constexpr UINT kIncomingNotificationMessage = WM_APP + 0x533;
constexpr UINT_PTR kTrayAttentionTimerId = 1;
constexpr UINT kTrayShowCommand = 1000;
constexpr UINT kTrayExitCommand = 1002;
constexpr int kApplicationIconResourceId = 1;
constexpr int kNotificationSoundResourceId = 2;

struct ExistingInstanceActivationContext {
    bool activated = false;
};

BOOL CALLBACK activateExistingTrayWindow(HWND window, LPARAM contextAddress)
{
    wchar_t className[64]{};
    const int classLength = GetClassNameW(
        window,
        className,
        static_cast<int>(std::size(className)));
    if (classLength <= 0 || std::wstring(className) != kTrayWindowClassName) {
        return TRUE;
    }

    SendMessageW(window, WM_COMMAND, kTrayShowCommand, 0);
    auto* context =
        reinterpret_cast<ExistingInstanceActivationContext*>(contextAddress);
    context->activated = true;
    return FALSE;
}

BOOL CALLBACK activateExistingMainWindow(HWND window, LPARAM contextAddress)
{
    wchar_t title[128]{};
    const int titleLength = GetWindowTextW(
        window,
        title,
        static_cast<int>(std::size(title)));
    if (titleLength <= 0 || std::wstring(title) != kMainWindowTitle) {
        return TRUE;
    }

    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    auto* context =
        reinterpret_cast<ExistingInstanceActivationContext*>(contextAddress);
    context->activated = true;
    return FALSE;
}

HICON loadApplicationIcon(HINSTANCE instance, int width, int height)
{
    return static_cast<HICON>(LoadImageW(instance,
                                         MAKEINTRESOURCEW(
                                             kApplicationIconResourceId),
                                         IMAGE_ICON,
                                         width,
                                         height,
                                         LR_DEFAULTCOLOR));
}

HICON createTransparentIcon(HINSTANCE instance)
{
    constexpr int kIconSize = 16;
    std::array<BYTE, (kIconSize * kIconSize) / 8> andMask{};
    std::array<BYTE, (kIconSize * kIconSize) / 8> xorMask{};
    andMask.fill(0xFF);
    return CreateIcon(instance,
                      kIconSize,
                      kIconSize,
                      1,
                      1,
                      andMask.data(),
                      xorMask.data());
}

bool isMainWindowActive(HWND window)
{
    if (window == nullptr) {
        return true;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground == window) {
        return true;
    }
    return foreground != nullptr && GetAncestor(foreground, GA_ROOT) == window;
}

void setTaskbarFlash(HWND window, bool enabled)
{
    if (window == nullptr) {
        return;
    }

    FLASHWINFO flashInfo{};
    flashInfo.cbSize = sizeof(flashInfo);
    flashInfo.hwnd = window;
    flashInfo.dwFlags = enabled
        ? static_cast<DWORD>(FLASHW_TRAY | FLASHW_TIMERNOFG)
        : static_cast<DWORD>(FLASHW_STOP);
    flashInfo.uCount = 0;
    flashInfo.dwTimeout = 0;
    (void)FlashWindowEx(&flashInfo);
}

class NotificationBridge {
public:
    void setTrayWindow(HWND window)
    {
        std::lock_guard lock(mutex_);
        trayWindow_ = window;
    }

    void postIncomingNotification()
    {
        std::lock_guard lock(mutex_);
        if (trayWindow_ != nullptr) {
            (void)PostMessageW(trayWindow_, kIncomingNotificationMessage, 0, 0);
        }
    }

private:
    std::mutex mutex_;
    HWND trayWindow_ = nullptr;
};

bool loadStoredLaunchAtStartupEnabled()
{
    try {
        const auto paths = relaydesk::storage::createAppPaths();
        return relaydesk::storage::loadLaunchAtStartupEnabled(paths);
    } catch (const std::exception&) {
        return true;
    }
}

void applyLaunchAtStartupEnabled(bool enabled)
{
    try {
        const auto paths = relaydesk::storage::createAppPaths();
        relaydesk::platform::setStartupLaunchEnabled(
            paths.GetExecutablePath(),
            enabled);
    } catch (const std::exception&) {
    }
}

}  // namespace

class SingleInstanceGuard::Impl {
public:
    Impl()
    {
        mutex_ = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
        alreadyRunning_ =
            mutex_ != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    }

    ~Impl()
    {
        if (mutex_ != nullptr) {
            CloseHandle(mutex_);
        }
    }

    [[nodiscard]] bool alreadyRunning() const { return alreadyRunning_; }

private:
    HANDLE mutex_ = nullptr;
    bool alreadyRunning_ = false;
};

SingleInstanceGuard::SingleInstanceGuard()
    : impl_(std::make_unique<Impl>())
{
}

SingleInstanceGuard::~SingleInstanceGuard() = default;

bool SingleInstanceGuard::alreadyRunning() const
{
    return impl_->alreadyRunning();
}

void activateExistingInstance()
{
    ExistingInstanceActivationContext context;
    EnumWindows(activateExistingTrayWindow,
                reinterpret_cast<LPARAM>(&context));
    if (!context.activated) {
        EnumWindows(activateExistingMainWindow,
                    reinterpret_cast<LPARAM>(&context));
    }
}

void syncLaunchAtStartupOnAppStart()
{
    applyLaunchAtStartupEnabled(loadStoredLaunchAtStartupEnabled());
}

class BackgroundController::Impl {
public:
    explicit Impl(HINSTANCE instance)
        : instance_(instance),
          notificationBridge_(std::make_shared<NotificationBridge>())
    {
    }

    ~Impl()
    {
        detachRuntime();
        shutdownTray();
    }

    void attachRuntime(relaydesk::runtime::RelayDeskRuntime& runtime)
    {
        if (runtime_ == &runtime) {
            return;
        }
        detachRuntime();
        runtime_ = &runtime;
        const std::shared_ptr<NotificationBridge> bridge = notificationBridge_;
        runtime_->SetUserNotificationHandler([bridge] {
            (void)PlaySoundW(MAKEINTRESOURCEW(kNotificationSoundResourceId),
                             GetModuleHandleW(nullptr),
                             SND_RESOURCE | SND_ASYNC | SND_NODEFAULT
                                 | SND_SYSTEM);
            bridge->postIncomingNotification();
        });
    }

    void detachRuntime()
    {
        if (runtime_ == nullptr) {
            return;
        }
        runtime_->SetUserNotificationHandler({});
        runtime_ = nullptr;
    }

    bool handleMainWindowMessage(HWND window,
                                 UINT message,
                                 WPARAM wParam,
                                 LPARAM lParam)
    {
        (void)lParam;
        switch (message) {
        case WM_CREATE:
            mainWindow_ = window;
            applyMainWindowIcons();
            (void)initializeTray();
            return false;
        case WM_CLOSE:
            if (trayAvailable_ && !exitRequested_) {
                ShowWindow(window, SW_HIDE);
                return true;
            }
            return false;
        case WM_KEYDOWN:
            return wParam == VK_ESCAPE;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) {
                clearAttention();
            }
            return false;
        case WM_DESTROY:
            mainWindow_ = nullptr;
            return false;
        default:
            return false;
        }
    }

    void processRuntimeState()
    {
        if (runtime_ == nullptr) {
            return;
        }

        if (runtime_->GetAppUpdateExitRequested()) {
            requestExit();
            return;
        }
        if (isMainWindowActive(mainWindow_)) {
            clearAttention();
        }
        (void)runtime_->ConsumePendingUserNotificationCount();
    }

private:
    static LRESULT CALLBACK trayWindowProc(HWND window,
                                           UINT message,
                                           WPARAM wParam,
                                           LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window,
                              GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(
                                  create->lpCreateParams));
        }

        auto* controller = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (controller == nullptr) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return controller->handleTrayWindowMessage(
            window,
            message,
            wParam,
            lParam);
    }

    LRESULT handleTrayWindowMessage(HWND window,
                                    UINT message,
                                    WPARAM wParam,
                                    LPARAM lParam)
    {
        switch (message) {
        case kTrayCallbackMessage:
            if (lParam == WM_LBUTTONUP) {
                restoreMainWindow();
                return 0;
            }
            if (lParam == WM_RBUTTONUP) {
                showTrayMenu();
                return 0;
            }
            break;
        case kIncomingNotificationMessage:
            handleIncomingNotification();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kTrayShowCommand) {
                restoreMainWindow();
                return 0;
            }
            if (LOWORD(wParam) == kTrayExitCommand) {
                requestExit();
                return 0;
            }
            break;
        case WM_TIMER:
            if (wParam == kTrayAttentionTimerId) {
                toggleTrayAttentionIcon();
                return 0;
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool initializeTray()
    {
        if (trayAvailable_) {
            return true;
        }

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance_;
        windowClass.lpfnWndProc = &Impl::trayWindowProc;
        windowClass.lpszClassName = kTrayWindowClassName;
        if (RegisterClassExW(&windowClass) == 0) {
            return false;
        }
        trayClassRegistered_ = true;

        trayWindow_ = CreateWindowExW(0,
                                      kTrayWindowClassName,
                                      L"",
                                      0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      nullptr,
                                      nullptr,
                                      instance_,
                                      this);
        if (trayWindow_ == nullptr) {
            shutdownTray();
            return false;
        }

        trayIcon_ = loadApplicationIcon(instance_,
                                        GetSystemMetrics(SM_CXSMICON),
                                        GetSystemMetrics(SM_CYSMICON));
        transparentIcon_ = createTransparentIcon(instance_);
        if (trayIcon_ == nullptr || transparentIcon_ == nullptr) {
            shutdownTray();
            return false;
        }

        trayIconData_ = {};
        trayIconData_.cbSize = sizeof(trayIconData_);
        trayIconData_.hWnd = trayWindow_;
        trayIconData_.uID = 0;
        trayIconData_.uFlags = NIF_ICON | NIF_MESSAGE;
        trayIconData_.uCallbackMessage = kTrayCallbackMessage;
        trayIconData_.hIcon = trayIcon_;
        if (!Shell_NotifyIconW(NIM_ADD, &trayIconData_)) {
            shutdownTray();
            return false;
        }

        trayIconAdded_ = true;
        trayAvailable_ = true;
        notificationBridge_->setTrayWindow(trayWindow_);
        return true;
    }

    void shutdownTray()
    {
        notificationBridge_->setTrayWindow(nullptr);
        if (trayWindow_ != nullptr) {
            KillTimer(trayWindow_, kTrayAttentionTimerId);
        }
        if (trayIconAdded_) {
            (void)Shell_NotifyIconW(NIM_DELETE, &trayIconData_);
            trayIconAdded_ = false;
        }
        if (trayWindow_ != nullptr) {
            DestroyWindow(trayWindow_);
            trayWindow_ = nullptr;
        }
        if (trayClassRegistered_) {
            UnregisterClassW(kTrayWindowClassName, instance_);
            trayClassRegistered_ = false;
        }
        if (trayIcon_ != nullptr) {
            DestroyIcon(trayIcon_);
            trayIcon_ = nullptr;
        }
        if (transparentIcon_ != nullptr) {
            DestroyIcon(transparentIcon_);
            transparentIcon_ = nullptr;
        }
        if (mainLargeIcon_ != nullptr) {
            DestroyIcon(mainLargeIcon_);
            mainLargeIcon_ = nullptr;
        }
        if (mainSmallIcon_ != nullptr) {
            DestroyIcon(mainSmallIcon_);
            mainSmallIcon_ = nullptr;
        }
        trayAvailable_ = false;
        trayAttentionEnabled_ = false;
    }

    void applyMainWindowIcons()
    {
        mainLargeIcon_ = loadApplicationIcon(instance_,
                                            GetSystemMetrics(SM_CXICON),
                                            GetSystemMetrics(SM_CYICON));
        mainSmallIcon_ = loadApplicationIcon(instance_,
                                            GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON));
        if (mainLargeIcon_ != nullptr) {
            SendMessageW(mainWindow_,
                         WM_SETICON,
                         ICON_BIG,
                         reinterpret_cast<LPARAM>(mainLargeIcon_));
        }
        if (mainSmallIcon_ != nullptr) {
            SendMessageW(mainWindow_,
                         WM_SETICON,
                         ICON_SMALL,
                         reinterpret_cast<LPARAM>(mainSmallIcon_));
            SetClassLongPtrW(mainWindow_,
                             GCLP_HICONSM,
                             reinterpret_cast<LONG_PTR>(mainSmallIcon_));
        }
    }

    void showTrayMenu()
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) {
            return;
        }

        AppendMenuW(menu, MF_STRING, kTrayShowCommand, L"显示");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出");

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(trayWindow_);
        const UINT command = TrackPopupMenu(menu,
                                            TPM_LEFTALIGN | TPM_RIGHTBUTTON
                                                | TPM_RETURNCMD
                                                | TPM_NONOTIFY,
                                            cursor.x,
                                            cursor.y,
                                            0,
                                            trayWindow_,
                                            nullptr);
        DestroyMenu(menu);
        if (command != 0) {
            SendMessageW(trayWindow_, WM_COMMAND, command, 0);
        }
    }

    void restoreMainWindow()
    {
        if (mainWindow_ == nullptr) {
            return;
        }
        ShowWindow(mainWindow_, SW_RESTORE);
        SetForegroundWindow(mainWindow_);
    }

    void requestExit()
    {
        if (mainWindow_ == nullptr || exitRequested_) {
            return;
        }
        exitRequested_ = true;
        SendMessageW(mainWindow_, WM_CLOSE, 0, 0);
    }

    void handleIncomingNotification()
    {
        if (mainWindow_ == nullptr) {
            return;
        }
        if (!IsWindowVisible(mainWindow_)) {
            ShowWindow(mainWindow_, SW_SHOWMINNOACTIVE);
        }
        if (!isMainWindowActive(mainWindow_)) {
            setTaskbarFlash(mainWindow_, true);
            setTrayAttention(true);
        }
    }

    void setTrayAttention(bool enabled)
    {
        if (!trayAvailable_ || trayAttentionEnabled_ == enabled) {
            return;
        }

        trayAttentionEnabled_ = enabled;
        transparentIconVisible_ = false;
        updateTrayIcon(trayIcon_);
        if (enabled) {
            (void)SetTimer(trayWindow_,
                           kTrayAttentionTimerId,
                           500,
                           nullptr);
        } else {
            KillTimer(trayWindow_, kTrayAttentionTimerId);
        }
    }

    void clearAttention()
    {
        setTaskbarFlash(mainWindow_, false);
        setTrayAttention(false);
    }

    void toggleTrayAttentionIcon()
    {
        if (!trayAttentionEnabled_) {
            return;
        }
        transparentIconVisible_ = !transparentIconVisible_;
        updateTrayIcon(transparentIconVisible_ ? transparentIcon_ : trayIcon_);
    }

    void updateTrayIcon(HICON icon)
    {
        if (!trayIconAdded_ || icon == nullptr) {
            return;
        }
        trayIconData_.uFlags = NIF_ICON;
        trayIconData_.hIcon = icon;
        (void)Shell_NotifyIconW(NIM_MODIFY, &trayIconData_);
    }

    HINSTANCE instance_ = nullptr;
    relaydesk::runtime::RelayDeskRuntime* runtime_ = nullptr;
    std::shared_ptr<NotificationBridge> notificationBridge_;
    HWND mainWindow_ = nullptr;
    HWND trayWindow_ = nullptr;
    HICON trayIcon_ = nullptr;
    HICON transparentIcon_ = nullptr;
    HICON mainLargeIcon_ = nullptr;
    HICON mainSmallIcon_ = nullptr;
    NOTIFYICONDATAW trayIconData_{};
    bool trayClassRegistered_ = false;
    bool trayIconAdded_ = false;
    bool trayAvailable_ = false;
    bool exitRequested_ = false;
    bool trayAttentionEnabled_ = false;
    bool transparentIconVisible_ = false;
};

BackgroundController::BackgroundController(HINSTANCE instance)
    : impl_(std::make_unique<Impl>(instance))
{
}

BackgroundController::~BackgroundController() = default;

void BackgroundController::attachRuntime(
    relaydesk::runtime::RelayDeskRuntime& runtime)
{
    impl_->attachRuntime(runtime);
}

void BackgroundController::detachRuntime()
{
    impl_->detachRuntime();
}

bool BackgroundController::handleMainWindowMessage(HWND window,
                                                   UINT message,
                                                   WPARAM wParam,
                                                   LPARAM lParam)
{
    return impl_->handleMainWindowMessage(
        window,
        message,
        wParam,
        lParam);
}

void BackgroundController::processRuntimeState()
{
    impl_->processRuntimeState();
}

}  // namespace relaydesk::skiaui
