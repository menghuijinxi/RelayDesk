#include "platform/attachment_input.h"

#include "core/uuid.h"
#include "storage/app_paths.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace relaydesk::platform {
namespace {

constexpr UINT kAllDroppedFiles = 0xFFFFFFFFu;
constexpr WORD kBitmapFileSignature = 0x4D42u;
constexpr DWORD kBitmapAlphaBitfieldsCompression = 6u;

std::mutex gDroppedAttachmentPathsMutex;
std::vector<std::filesystem::path> gDroppedAttachmentPaths;
HWND gDropWindow = nullptr;
WNDPROC gOriginalDropWndProc = nullptr;

class ClipboardScope {
public:
    ClipboardScope()
        : opened_(OpenClipboard(nullptr) != FALSE)
    {
    }

    ~ClipboardScope()
    {
        if (opened_) {
            CloseClipboard();
        }
    }

    bool GetOpened() const { return opened_; }

protected:
    bool opened_ = false;
};

class GlobalMemoryLock {
public:
    explicit GlobalMemoryLock(HANDLE handle)
        : handle_(handle),
          data_(GlobalLock(handle))
    {
    }

    ~GlobalMemoryLock()
    {
        if (data_ != nullptr) {
            GlobalUnlock(handle_);
        }
    }

    const void* GetData() const { return data_; }
    SIZE_T GetSize() const { return GlobalSize(handle_); }

protected:
    HANDLE handle_ = nullptr;
    void* data_ = nullptr;
};

std::vector<std::filesystem::path> extractDroppedPaths(HDROP dropHandle)
{
    std::vector<std::filesystem::path> paths;
    const UINT count = DragQueryFileW(dropHandle, kAllDroppedFiles, nullptr, 0);
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(dropHandle, index, nullptr, 0);
        if (length == 0) {
            continue;
        }

        std::wstring buffer(static_cast<std::size_t>(length) + 1u, L'\0');
        const UINT copied =
            DragQueryFileW(dropHandle, index, buffer.data(), length + 1u);
        if (copied == 0) {
            continue;
        }

        buffer.resize(copied);
        paths.emplace_back(buffer);
    }
    return paths;
}

void appendDroppedPaths(std::vector<std::filesystem::path> paths)
{
    std::lock_guard lock(gDroppedAttachmentPathsMutex);
    for (auto& path : paths) {
        gDroppedAttachmentPaths.push_back(std::move(path));
    }
}

LRESULT CALLBACK attachmentDropWndProc(HWND window,
                                       UINT message,
                                       WPARAM wParam,
                                       LPARAM lParam)
{
    if (message == WM_DROPFILES) {
        const auto dropHandle = reinterpret_cast<HDROP>(wParam);
        appendDroppedPaths(extractDroppedPaths(dropHandle));
        DragFinish(dropHandle);
        return 0;
    }

    if (gOriginalDropWndProc != nullptr) {
        return CallWindowProcW(
            gOriginalDropWndProc,
            window,
            message,
            wParam,
            lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

struct WindowSearchContext {
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK findRelayDeskWindow(HWND window, LPARAM contextAddress)
{
    auto& context =
        *reinterpret_cast<WindowSearchContext*>(contextAddress);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != context.processId
        || !IsWindowVisible(window)
        || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }

    std::array<wchar_t, 256> titleBuffer{};
    GetWindowTextW(
        window,
        titleBuffer.data(),
        static_cast<int>(titleBuffer.size()));
    const std::wstring title(titleBuffer.data());
    if (title.rfind(L"RelayDesk", 0) != 0) {
        return TRUE;
    }

    context.window = window;
    return FALSE;
}

HWND findRelayDeskMainWindow()
{
    WindowSearchContext context;
    context.processId = GetCurrentProcessId();
    EnumWindows(findRelayDeskWindow, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

std::uint32_t calculateDibPixelOffset(const BITMAPINFOHEADER& header)
{
    std::uint32_t offset = header.biSize;
    if ((header.biCompression == BI_BITFIELDS
         || header.biCompression == kBitmapAlphaBitfieldsCompression)
        && header.biSize == sizeof(BITMAPINFOHEADER)) {
        offset += 3u * static_cast<std::uint32_t>(sizeof(DWORD));
    }

    if (header.biBitCount > 0 && header.biBitCount <= 8) {
        const std::uint32_t colorCount = header.biClrUsed != 0
            ? header.biClrUsed
            : (1u << header.biBitCount);
        offset += colorCount * static_cast<std::uint32_t>(sizeof(RGBQUAD));
    }

    return offset;
}

std::optional<std::filesystem::path> saveClipboardDibToOutbox()
{
    HANDLE dibHandle = GetClipboardData(CF_DIB);
    if (dibHandle == nullptr) {
        return std::nullopt;
    }

    GlobalMemoryLock dibMemory(dibHandle);
    if (dibMemory.GetData() == nullptr
        || dibMemory.GetSize() < sizeof(BITMAPINFOHEADER)) {
        return std::nullopt;
    }

    const auto* header =
        static_cast<const BITMAPINFOHEADER*>(dibMemory.GetData());
    if (header->biSize < sizeof(BITMAPINFOHEADER)
        || dibMemory.GetSize() < header->biSize) {
        return std::nullopt;
    }

    const SIZE_T dibSize = dibMemory.GetSize();
    const std::uint64_t fileSize =
        static_cast<std::uint64_t>(sizeof(BITMAPFILEHEADER)) + dibSize;
    if (fileSize > std::numeric_limits<DWORD>::max()) {
        return std::nullopt;
    }

    const auto appPaths = relaydesk::storage::createAppPaths();
    relaydesk::storage::ensureAppDirectories(appPaths);
    const std::filesystem::path targetDirectory =
        appPaths.GetOutboxDirectory() / relaydesk::core::createUuidV4();
    std::filesystem::create_directories(targetDirectory);
    const std::filesystem::path targetPath = targetDirectory / "clipboard.bmp";

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = kBitmapFileSignature;
    fileHeader.bfSize = static_cast<DWORD>(fileSize);
    fileHeader.bfOffBits = static_cast<DWORD>(
        sizeof(BITMAPFILEHEADER) + calculateDibPixelOffset(*header));

    std::ofstream output(targetPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::nullopt;
    }

    output.write(
        reinterpret_cast<const char*>(&fileHeader),
        static_cast<std::streamsize>(sizeof(fileHeader)));
    output.write(
        static_cast<const char*>(dibMemory.GetData()),
        static_cast<std::streamsize>(dibSize));
    if (!output) {
        return std::nullopt;
    }

    return targetPath;
}

} // namespace

void initializeAttachmentDropTarget()
{
    if (gDropWindow != nullptr && IsWindow(gDropWindow)) {
        return;
    }

    gDropWindow = nullptr;
    gOriginalDropWndProc = nullptr;
    HWND window = findRelayDeskMainWindow();
    if (window == nullptr) {
        return;
    }

    SetLastError(0);
    const LONG_PTR previousWndProc = SetWindowLongPtrW(
        window,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(attachmentDropWndProc));
    if (previousWndProc == 0 && GetLastError() != 0) {
        return;
    }

    gOriginalDropWndProc = reinterpret_cast<WNDPROC>(previousWndProc);
    gDropWindow = window;
    DragAcceptFiles(window, TRUE);
}

std::vector<std::filesystem::path> consumeDroppedAttachmentPaths()
{
    std::lock_guard lock(gDroppedAttachmentPathsMutex);
    std::vector<std::filesystem::path> paths;
    paths.swap(gDroppedAttachmentPaths);
    return paths;
}

std::vector<std::filesystem::path> collectClipboardAttachmentPaths()
{
    std::vector<std::filesystem::path> paths;
    ClipboardScope clipboard;
    if (!clipboard.GetOpened()) {
        return paths;
    }

    if (IsClipboardFormatAvailable(CF_HDROP)) {
        const auto dropHandle = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (dropHandle != nullptr) {
            paths = extractDroppedPaths(dropHandle);
        }
        if (!paths.empty()) {
            return paths;
        }
    }

    if (IsClipboardFormatAvailable(CF_DIB)) {
        std::optional<std::filesystem::path> imagePath = saveClipboardDibToOutbox();
        if (imagePath.has_value()) {
            paths.push_back(std::move(imagePath.value()));
        }
    }
    return paths;
}

bool isPasteShortcutDown()
{
    const bool controlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0
        || (GetKeyState(VK_LCONTROL) & 0x8000) != 0
        || (GetKeyState(VK_RCONTROL) & 0x8000) != 0;
    return controlDown && (GetKeyState('V') & 0x8000) != 0;
}

}
