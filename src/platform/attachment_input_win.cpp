#include "platform/attachment_input.h"

#include "core/uuid.h"
#include "platform/text_encoding.h"
#include "storage/app_paths.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
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
#include <wincodec.h>
#include <wrl/client.h>
#include <shlobj_core.h>
#include <shobjidl.h>
#include <shellapi.h>

namespace relaydesk::platform {
namespace {

constexpr UINT kAllDroppedFiles = 0xFFFFFFFFu;
constexpr WORD kBitmapFileSignature = 0x4D42u;
constexpr DWORD kBitmapAlphaBitfieldsCompression = 6u;
constexpr unsigned int kMinimumThumbnailSide = 1u;
constexpr float kJpegThumbnailQuality = 0.78f;
constexpr wchar_t kClipboardBitmapFileName[] = L"clipboard.bmp";
constexpr wchar_t kScreenshotBitmapFileName[] = L"screenshot.bmp";

std::mutex gDroppedAttachmentPathsMutex;
std::vector<std::filesystem::path> gDroppedAttachmentPaths;
HWND gDropWindow = nullptr;
WNDPROC gOriginalDropWndProc = nullptr;
bool gBackspaceWasDown = false;

struct ThumbnailEncoderFormat {
    GUID containerFormat;
    WICPixelFormatGUID pixelFormat;
    bool jpeg = false;
};

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

class ComApartment {
public:
    ComApartment()
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
          shouldUninitialize_(result_ == S_OK || result_ == S_FALSE)
    {
    }

    ~ComApartment()
    {
        if (shouldUninitialize_) {
            CoUninitialize();
        }
    }

    bool GetAvailable() const
    {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

protected:
    HRESULT result_ = E_FAIL;
    bool shouldUninitialize_ = false;
};

std::wstring lowercaseExtension(const std::filesystem::path& filePath)
{
    std::wstring extension = filePath.extension().wstring();
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](wchar_t value) {
                       return static_cast<wchar_t>(std::towlower(value));
                   });
    return extension;
}

ThumbnailEncoderFormat chooseThumbnailEncoderFormat(
    const std::filesystem::path& targetPath)
{
    const std::wstring extension = lowercaseExtension(targetPath);
    if (extension == L".jpg" || extension == L".jpeg") {
        return {GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, true};
    }
    return {GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA, false};
}

bool writeJpegThumbnailQuality(IPropertyBag2* propertyBag)
{
    if (propertyBag == nullptr) {
        return false;
    }

    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");

    VARIANT value{};
    value.vt = VT_R4;
    value.fltVal = kJpegThumbnailQuality;
    return SUCCEEDED(propertyBag->Write(1, &option, &value));
}

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

HGLOBAL createDroppedFilesHandle(const std::filesystem::path& sourcePath)
{
    if (sourcePath.empty()) {
        return nullptr;
    }

    const std::wstring pathText = sourcePath.wstring();
    if (pathText.empty()) {
        return nullptr;
    }

    const SIZE_T bytes =
        sizeof(DROPFILES)
        + (pathText.size() + 2u) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (handle == nullptr) {
        return nullptr;
    }

    void* rawMemory = GlobalLock(handle);
    if (rawMemory == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }

    auto* dropFiles = static_cast<DROPFILES*>(rawMemory);
    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->fWide = TRUE;

    auto* target = reinterpret_cast<wchar_t*>(
        static_cast<unsigned char*>(rawMemory)
        + sizeof(DROPFILES));
    std::copy(pathText.begin(), pathText.end(), target);
    target[pathText.size()] = L'\0';
    target[pathText.size() + 1u] = L'\0';
    GlobalUnlock(handle);
    return handle;
}

HGLOBAL createPreferredCopyEffectHandle()
{
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (handle == nullptr) {
        return nullptr;
    }

    void* rawMemory = GlobalLock(handle);
    if (rawMemory == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }

    *static_cast<DWORD*>(rawMemory) = DROPEFFECT_COPY;
    GlobalUnlock(handle);
    return handle;
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

void setDialogDefaultFolder(IFileDialog* dialog,
                            const std::filesystem::path& directoryPath)
{
    if (dialog == nullptr || directoryPath.empty()) {
        return;
    }

    std::error_code error;
    if (!std::filesystem::is_directory(directoryPath, error) || error) {
        return;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IShellItem> folder;
    const HRESULT result = SHCreateItemFromParsingName(
        directoryPath.c_str(),
        nullptr,
        IID_PPV_ARGS(&folder));
    if (SUCCEEDED(result)) {
        (void)dialog->SetDefaultFolder(folder.Get());
    }
}

std::optional<std::filesystem::path> shellItemFilesystemPath(IShellItem* item)
{
    if (item == nullptr) {
        return std::nullopt;
    }

    PWSTR rawPath = nullptr;
    const HRESULT result = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    if (FAILED(result) || rawPath == nullptr) {
        return std::nullopt;
    }

    const std::filesystem::path selectedPath(rawPath);
    CoTaskMemFree(rawPath);
    return selectedPath.lexically_normal();
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

bool shellExecuteSucceeded(HINSTANCE result)
{
    return reinterpret_cast<std::intptr_t>(result) > 32;
}

std::optional<std::filesystem::path> saveClipboardDibToOutbox(
    const std::wstring& fileName)
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
    const std::filesystem::path targetPath = targetDirectory / fileName;

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

HANDLE createClipboardDibFromImageFile(const std::filesystem::path& sourcePath)
{
    ComApartment apartment;
    if (!apartment.GetAvailable()) {
        return nullptr;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return nullptr;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(
        sourcePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (FAILED(result)) {
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result)) {
        return nullptr;
    }

    UINT width = 0;
    UINT height = 0;
    result = frame->GetSize(&width, &height);
    if (FAILED(result) || width == 0 || height == 0) {
        return nullptr;
    }

    const std::uint64_t stride64 = static_cast<std::uint64_t>(width) * 4u;
    const std::uint64_t pixelSize64 = stride64 * static_cast<std::uint64_t>(height);
    const std::uint64_t dibSize64 =
        sizeof(BITMAPINFOHEADER) + pixelSize64;
    if (stride64 > std::numeric_limits<UINT>::max()
        || pixelSize64 > std::numeric_limits<UINT>::max()
        || pixelSize64 > std::numeric_limits<DWORD>::max()
        || dibSize64 > std::numeric_limits<SIZE_T>::max()
        || width > static_cast<UINT>(std::numeric_limits<LONG>::max())
        || height > static_cast<UINT>(std::numeric_limits<LONG>::max())) {
        return nullptr;
    }

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (FAILED(result)) {
        return nullptr;
    }
    result = converter->Initialize(frame.Get(),
                                   GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone,
                                   nullptr,
                                   0.0,
                                   WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        return nullptr;
    }

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(pixelSize64), 0u);
    result = converter->CopyPixels(
        nullptr,
        static_cast<UINT>(stride64),
        static_cast<UINT>(pixelSize64),
        pixels.data());
    if (FAILED(result)) {
        return nullptr;
    }

    const HANDLE dibHandle = GlobalAlloc(
        GMEM_MOVEABLE,
        static_cast<SIZE_T>(dibSize64));
    if (dibHandle == nullptr) {
        return nullptr;
    }

    GlobalMemoryLock dibMemory(dibHandle);
    if (dibMemory.GetData() == nullptr) {
        GlobalFree(dibHandle);
        return nullptr;
    }

    auto* header = static_cast<BITMAPINFOHEADER*>(
        const_cast<void*>(dibMemory.GetData()));
    std::memset(header, 0, sizeof(BITMAPINFOHEADER));
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(width);
    header->biHeight = -static_cast<LONG>(height);
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixelSize64);
    std::memcpy(static_cast<std::uint8_t*>(const_cast<void*>(dibMemory.GetData()))
                    + sizeof(BITMAPINFOHEADER),
                pixels.data(),
                pixels.size());

    return dibHandle;
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
        std::optional<std::filesystem::path> imagePath =
            saveClipboardDibToOutbox(kClipboardBitmapFileName);
        if (imagePath.has_value()) {
            paths.push_back(std::move(imagePath.value()));
        }
    }
    return paths;
}

std::vector<std::filesystem::path> collectClipboardImageAttachmentPaths()
{
    std::vector<std::filesystem::path> paths;
    ClipboardScope clipboard;
    if (!clipboard.GetOpened()) {
        return paths;
    }

    if (IsClipboardFormatAvailable(CF_DIB)) {
        std::optional<std::filesystem::path> imagePath =
            saveClipboardDibToOutbox(kScreenshotBitmapFileName);
        if (imagePath.has_value()) {
            paths.push_back(std::move(imagePath.value()));
        }
    }
    return paths;
}

std::uint32_t getClipboardSequenceNumber()
{
    return static_cast<std::uint32_t>(GetClipboardSequenceNumber());
}

bool startScreenClipCapture()
{
    HINSTANCE result = ShellExecuteW(nullptr,
                                     L"open",
                                     L"ms-screenclip:",
                                     nullptr,
                                     nullptr,
                                     SW_SHOWNORMAL);
    if (shellExecuteSucceeded(result)) {
        return true;
    }

    result = ShellExecuteW(nullptr,
                           L"open",
                           L"SnippingTool.exe",
                           L"/clip",
                           nullptr,
                           SW_SHOWNORMAL);
    return shellExecuteSucceeded(result);
}

bool revealPathInFileManager(const std::filesystem::path& sourcePath)
{
    if (sourcePath.empty()) {
        return false;
    }

    std::error_code error;
    if (std::filesystem::exists(sourcePath, error) && !error) {
        const std::wstring parameters =
            L"/select,\"" + sourcePath.wstring() + L"\"";
        const HINSTANCE result = ShellExecuteW(nullptr,
                                               L"open",
                                               L"explorer.exe",
                                               parameters.c_str(),
                                               nullptr,
                                               SW_SHOWNORMAL);
        return shellExecuteSucceeded(result);
    }

    const std::filesystem::path directoryPath = sourcePath.parent_path();
    if (directoryPath.empty()) {
        return false;
    }
    const HINSTANCE result = ShellExecuteW(nullptr,
                                           L"open",
                                           directoryPath.c_str(),
                                           nullptr,
                                           nullptr,
                                           SW_SHOWNORMAL);
    return shellExecuteSucceeded(result);
}

bool copyImageFileToClipboard(const std::filesystem::path& sourcePath)
{
    if (sourcePath.empty()) {
        return false;
    }

    const HANDLE dibHandle = createClipboardDibFromImageFile(sourcePath);
    if (dibHandle == nullptr) {
        return false;
    }

    ClipboardScope clipboard;
    if (!clipboard.GetOpened()) {
        GlobalFree(dibHandle);
        return false;
    }
    if (EmptyClipboard() == FALSE) {
        GlobalFree(dibHandle);
        return false;
    }
    if (SetClipboardData(CF_DIB, dibHandle) == nullptr) {
        GlobalFree(dibHandle);
        return false;
    }
    return true;
}

bool copyAttachmentPathToClipboard(const std::filesystem::path& sourcePath)
{
    HGLOBAL dropHandle = createDroppedFilesHandle(sourcePath);
    if (dropHandle == nullptr) {
        return false;
    }
    HGLOBAL dropEffectHandle = createPreferredCopyEffectHandle();
    const UINT dropEffectFormat =
        RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (dropEffectHandle == nullptr || dropEffectFormat == 0) {
        GlobalFree(dropHandle);
        if (dropEffectHandle != nullptr) {
            GlobalFree(dropEffectHandle);
        }
        return false;
    }

    ClipboardScope clipboard;
    if (!clipboard.GetOpened()) {
        GlobalFree(dropHandle);
        GlobalFree(dropEffectHandle);
        return false;
    }
    if (EmptyClipboard() == FALSE) {
        GlobalFree(dropHandle);
        GlobalFree(dropEffectHandle);
        return false;
    }
    if (SetClipboardData(CF_HDROP, dropHandle) == nullptr) {
        GlobalFree(dropHandle);
        GlobalFree(dropEffectHandle);
        return false;
    }
    if (SetClipboardData(dropEffectFormat, dropEffectHandle) == nullptr) {
        GlobalFree(dropEffectHandle);
        (void)EmptyClipboard();
        return false;
    }
    return true;
}

std::optional<ImageSize> probeImageSize(const std::filesystem::path& sourcePath)
{
    ComApartment apartment;
    if (!apartment.GetAvailable()) {
        return std::nullopt;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return std::nullopt;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(
        sourcePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (FAILED(result)) {
        return std::nullopt;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result)) {
        return std::nullopt;
    }

    UINT width = 0;
    UINT height = 0;
    result = frame->GetSize(&width, &height);
    if (FAILED(result) || width == 0 || height == 0) {
        return std::nullopt;
    }

    return ImageSize{width, height};
}

std::optional<std::filesystem::path> createImageThumbnail(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& targetPath,
    unsigned int maxSide)
{
    if (maxSide < kMinimumThumbnailSide) {
        return std::nullopt;
    }

    ComApartment apartment;
    if (!apartment.GetAvailable()) {
        return std::nullopt;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return std::nullopt;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(
        sourcePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (FAILED(result)) {
        return std::nullopt;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result)) {
        return std::nullopt;
    }

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    result = frame->GetSize(&sourceWidth, &sourceHeight);
    if (FAILED(result) || sourceWidth == 0 || sourceHeight == 0) {
        return std::nullopt;
    }

    const ThumbnailEncoderFormat encoderFormat =
        chooseThumbnailEncoderFormat(targetPath);
    const double scale = std::min(
        1.0,
        std::min(static_cast<double>(maxSide) / static_cast<double>(sourceWidth),
                 static_cast<double>(maxSide) / static_cast<double>(sourceHeight)));
    const UINT thumbWidth = std::max(kMinimumThumbnailSide,
                                     static_cast<UINT>(std::lround(sourceWidth * scale)));
    const UINT thumbHeight = std::max(kMinimumThumbnailSide,
                                      static_cast<UINT>(std::lround(sourceHeight * scale)));

    ComPtr<IWICBitmapScaler> scaler;
    result = factory->CreateBitmapScaler(&scaler);
    if (FAILED(result)) {
        return std::nullopt;
    }
    result = scaler->Initialize(frame.Get(),
                                thumbWidth,
                                thumbHeight,
                                WICBitmapInterpolationModeFant);
    if (FAILED(result)) {
        return std::nullopt;
    }

    std::error_code error;
    std::filesystem::create_directories(targetPath.parent_path(), error);
    if (error) {
        return std::nullopt;
    }
    const std::filesystem::path temporaryPath = targetPath;
    const std::filesystem::path writingPath =
        targetPath.parent_path()
        / (targetPath.filename().wstring() + L".writing");
    std::filesystem::remove(temporaryPath, error);
    error.clear();
    std::filesystem::remove(writingPath, error);
    error.clear();

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (FAILED(result)) {
        return std::nullopt;
    }
    result = converter->Initialize(scaler.Get(),
                                   encoderFormat.pixelFormat,
                                   WICBitmapDitherTypeNone,
                                   nullptr,
                                   0.0,
                                   WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        return std::nullopt;
    }

    ComPtr<IWICStream> stream;
    result = factory->CreateStream(&stream);
    if (FAILED(result)) {
        return std::nullopt;
    }
    result = stream->InitializeFromFilename(writingPath.c_str(), GENERIC_WRITE);
    if (FAILED(result)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(encoderFormat.containerFormat,
                                    nullptr,
                                    &encoder);
    if (FAILED(result)) {
        return std::nullopt;
    }
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(result)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    ComPtr<IPropertyBag2> propertyBag;
    result = encoder->CreateNewFrame(&frameEncode, &propertyBag);
    if (FAILED(result)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }

    if (encoderFormat.jpeg
        && !writeJpegThumbnailQuality(propertyBag.Get())) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }

    result = frameEncode->Initialize(propertyBag.Get());
    if (FAILED(result)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }
    result = frameEncode->SetSize(thumbWidth, thumbHeight);
    if (FAILED(result)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }

    WICPixelFormatGUID pixelFormat = encoderFormat.pixelFormat;
    result = frameEncode->SetPixelFormat(&pixelFormat);
    if (FAILED(result)
        || !IsEqualGUID(pixelFormat, encoderFormat.pixelFormat)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }

    result = frameEncode->WriteSource(converter.Get(), nullptr);
    if (FAILED(result)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }
    result = frameEncode->Commit();
    if (FAILED(result)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }
    result = encoder->Commit();
    if (FAILED(result)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }
    result = stream->Commit(STGC_DEFAULT);
    if (FAILED(result)) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }
    frameEncode.Reset();
    propertyBag.Reset();
    encoder.Reset();
    stream.Reset();

    const std::uintmax_t writtenSize =
        std::filesystem::file_size(writingPath, error);
    if (error || writtenSize == 0u) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }

    std::filesystem::rename(writingPath, targetPath, error);
    if (error) {
        std::filesystem::remove(writingPath, error);
        return std::nullopt;
    }

    return targetPath.lexically_normal();
}

std::optional<std::filesystem::path> selectSavePathFromDialog(
    const std::filesystem::path& initialDirectory,
    const std::string& suggestedFileName)
{
    ComApartment apartment;
    if (!apartment.GetAvailable()) {
        return std::nullopt;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IFileSaveDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileSaveDialog,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        return std::nullopt;
    }

    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) {
        (void)dialog->SetOptions(options | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
    }
    setDialogDefaultFolder(dialog.Get(), initialDirectory);
    if (!suggestedFileName.empty()) {
        const std::wstring fileName = utf8ToWide(suggestedFileName);
        (void)dialog->SetFileName(fileName.c_str());
    }

    result = dialog->Show(findRelayDeskMainWindow());
    if (FAILED(result)) {
        return std::nullopt;
    }

    ComPtr<IShellItem> item;
    result = dialog->GetResult(&item);
    if (FAILED(result)) {
        return std::nullopt;
    }

    return shellItemFilesystemPath(item.Get());
}

std::optional<std::filesystem::path> selectFolderFromDialog()
{
    ComApartment apartment;
    if (!apartment.GetAvailable()) {
        return std::nullopt;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        return std::nullopt;
    }

    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) {
        (void)dialog->SetOptions(
            options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }

    result = dialog->Show(findRelayDeskMainWindow());
    if (FAILED(result)) {
        return std::nullopt;
    }

    ComPtr<IShellItem> item;
    result = dialog->GetResult(&item);
    if (FAILED(result)) {
        return std::nullopt;
    }

    return shellItemFilesystemPath(item.Get());
}

bool consumeBackspacePressed()
{
    const bool down = (GetKeyState(VK_BACK) & 0x8000) != 0;
    const bool pressed = down && !gBackspaceWasDown;
    gBackspaceWasDown = down;
    return pressed;
}

}
