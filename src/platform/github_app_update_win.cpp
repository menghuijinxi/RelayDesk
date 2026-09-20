#include "platform/github_app_update.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

namespace relaydesk::platform {
namespace {

constexpr wchar_t kGitHubApiUserAgent[] = L"RelayDesk updater/1";
constexpr std::size_t kDownloadBufferBytes = 256u * 1024u;
constexpr std::size_t kHashBufferBytes = 64u * 1024u;
constexpr std::string_view kSha256DigestPrefix = "sha256:";

class WinHttpHandle {
public:
    WinHttpHandle() = default;

    explicit WinHttpHandle(HINTERNET handle)
        : handle_(handle)
    {
    }

    ~WinHttpHandle()
    {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept
        : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }

    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept
    {
        if (this != &other) {
            if (handle_ != nullptr) {
                WinHttpCloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HINTERNET Get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

protected:
    HINTERNET handle_ = nullptr;
};

class BCryptAlgorithmHandle {
public:
    explicit BCryptAlgorithmHandle(BCRYPT_ALG_HANDLE handle)
        : handle_(handle)
    {
    }

    ~BCryptAlgorithmHandle()
    {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    BCryptAlgorithmHandle(const BCryptAlgorithmHandle&) = delete;
    BCryptAlgorithmHandle& operator=(const BCryptAlgorithmHandle&) = delete;

    BCRYPT_ALG_HANDLE Get() const { return handle_; }

protected:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class BCryptHashHandle {
public:
    explicit BCryptHashHandle(BCRYPT_HASH_HANDLE handle)
        : handle_(handle)
    {
    }

    ~BCryptHashHandle()
    {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }

    BCryptHashHandle(const BCryptHashHandle&) = delete;
    BCryptHashHandle& operator=(const BCryptHashHandle&) = delete;

    BCRYPT_HASH_HANDLE Get() const { return handle_; }

protected:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    DWORD flags = 0;
};

struct HttpGet {
    WinHttpHandle session;
    WinHttpHandle connection;
    WinHttpHandle request;
};

bool ntSuccess(NTSTATUS status)
{
    return status >= 0;
}

void checkBCryptStatus(NTSTATUS status, const char* message)
{
    if (!ntSuccess(status)) {
        throw std::runtime_error(message);
    }
}

DWORD getBCryptDwordProperty(BCRYPT_ALG_HANDLE algorithm,
                             const wchar_t* propertyName)
{
    DWORD value = 0;
    DWORD copiedSize = 0;
    checkBCryptStatus(
        BCryptGetProperty(algorithm,
                          propertyName,
                          reinterpret_cast<PUCHAR>(&value),
                          sizeof(value),
                          &copiedSize,
                          0),
        "无法读取 SHA-256 属性。");
    return value;
}

std::string bytesToLowerHex(const std::vector<unsigned char>& bytes)
{
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2u);
    for (const unsigned char byte : bytes) {
        result.push_back(kHexDigits[(byte >> 4u) & 0x0Fu]);
        result.push_back(kHexDigits[byte & 0x0Fu]);
    }
    return result;
}

std::string sha256FileHex(const std::filesystem::path& sourcePath)
{
    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    checkBCryptStatus(
        BCryptOpenAlgorithmProvider(&rawAlgorithm,
                                    BCRYPT_SHA256_ALGORITHM,
                                    nullptr,
                                    0),
        "无法打开 SHA-256 算法。");
    BCryptAlgorithmHandle algorithm(rawAlgorithm);

    const DWORD objectLength =
        getBCryptDwordProperty(algorithm.Get(), BCRYPT_OBJECT_LENGTH);
    const DWORD hashLength =
        getBCryptDwordProperty(algorithm.Get(), BCRYPT_HASH_LENGTH);
    std::vector<unsigned char> hashObject(objectLength);

    BCRYPT_HASH_HANDLE rawHash = nullptr;
    checkBCryptStatus(
        BCryptCreateHash(algorithm.Get(),
                         &rawHash,
                         hashObject.data(),
                         static_cast<ULONG>(hashObject.size()),
                         nullptr,
                         0,
                         0),
        "无法创建 SHA-256 哈希。");
    BCryptHashHandle hash(rawHash);

    std::ifstream input(sourcePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法读取下载文件进行校验。");
    }

    std::array<char, kHashBufferBytes> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize readSize = input.gcount();
        if (readSize <= 0) {
            break;
        }

        checkBCryptStatus(
            BCryptHashData(hash.Get(),
                           reinterpret_cast<PUCHAR>(buffer.data()),
                           static_cast<ULONG>(readSize),
                           0),
            "无法计算下载文件的 SHA-256。");
    }
    if (input.bad()) {
        throw std::runtime_error("读取下载文件失败。");
    }

    std::vector<unsigned char> digest(hashLength);
    checkBCryptStatus(
        BCryptFinishHash(hash.Get(),
                         digest.data(),
                         static_cast<ULONG>(digest.size()),
                         0),
        "无法完成下载文件的 SHA-256 校验。");
    return bytesToLowerHex(digest);
}

bool isHexDigest(std::string_view value)
{
    if (value.size() != 64u) {
        return false;
    }

    return std::all_of(
        value.begin(),
        value.end(),
        [](unsigned char character) {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f')
                || (character >= 'A' && character <= 'F');
        });
}

std::optional<int> parseVersionFromTag(std::string_view tagName)
{
    if (tagName.size() < 2u
        || (tagName.front() != 'v' && tagName.front() != 'V')) {
        return std::nullopt;
    }

    int version = 0;
    const char* begin = tagName.data() + 1;
    const char* end = tagName.data() + tagName.size();
    const auto parsed = std::from_chars(begin, end, version);
    if (parsed.ec != std::errc{} || parsed.ptr != end || version < 0) {
        return std::nullopt;
    }
    return version;
}

std::wstring utf8ToWideText(std::string_view value)
{
    if (value.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0);
    if (length <= 0) {
        throw std::runtime_error("GitHub 更新地址编码无效。");
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            length)
        != length) {
        throw std::runtime_error("GitHub 更新地址转换失败。");
    }
    return result;
}

ParsedUrl parseUrl(std::string_view urlText)
{
    const std::wstring url = utf8ToWideText(urlText);
    std::vector<wchar_t> mutableUrl(url.begin(), url.end());
    mutableUrl.push_back(L'\0');

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    std::array<wchar_t, 256> hostBuffer{};
    std::array<wchar_t, 4096> pathBuffer{};
    std::array<wchar_t, 4096> extraInfoBuffer{};
    components.lpszHostName = hostBuffer.data();
    components.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
    components.lpszUrlPath = pathBuffer.data();
    components.dwUrlPathLength = static_cast<DWORD>(pathBuffer.size());
    components.lpszExtraInfo = extraInfoBuffer.data();
    components.dwExtraInfoLength =
        static_cast<DWORD>(extraInfoBuffer.size());

    if (!WinHttpCrackUrl(mutableUrl.data(),
                         static_cast<DWORD>(url.size()),
                         0,
                         &components)) {
        throw std::runtime_error("GitHub 更新地址解析失败。");
    }

    if (components.nScheme != INTERNET_SCHEME_HTTPS
        || components.dwHostNameLength == 0
        || components.dwUrlPathLength == 0) {
        throw std::runtime_error("GitHub 更新地址必须使用 HTTPS。");
    }

    ParsedUrl parsed;
    parsed.host.assign(hostBuffer.data(), components.dwHostNameLength);
    parsed.path.assign(pathBuffer.data(), components.dwUrlPathLength);
    parsed.path.append(extraInfoBuffer.data(), components.dwExtraInfoLength);
    parsed.port = components.nPort;
    parsed.flags = WINHTTP_FLAG_SECURE;
    return parsed;
}

HttpGet openGetRequest(std::string_view urlText)
{
    const ParsedUrl url = parseUrl(urlText);
    HttpGet http;
    http.session = WinHttpHandle(
        WinHttpOpen(kGitHubApiUserAgent,
                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS,
                    0));
    if (!http.session) {
        throw std::runtime_error("无法创建 GitHub 网络会话。");
    }
    WinHttpSetTimeouts(http.session.Get(), 10000, 10000, 30000, 30000);

    http.connection =
        WinHttpHandle(WinHttpConnect(http.session.Get(),
                                     url.host.c_str(),
                                     url.port,
                                     0));
    if (!http.connection) {
        throw std::runtime_error("无法连接 GitHub。");
    }

    http.request =
        WinHttpHandle(WinHttpOpenRequest(http.connection.Get(),
                                         L"GET",
                                         url.path.c_str(),
                                         nullptr,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         url.flags));
    if (!http.request) {
        throw std::runtime_error("无法创建 GitHub 更新请求。");
    }
    return http;
}

DWORD queryStatusCode(HINTERNET request)
{
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE
                                 | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode,
                             &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw std::runtime_error("无法读取 GitHub 响应状态。");
    }
    return statusCode;
}

std::optional<std::uintmax_t> queryContentLength(HINTERNET request)
{
    std::array<wchar_t, 64> buffer{};
    DWORD bufferSize = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_CONTENT_LENGTH,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             buffer.data(),
                             &bufferSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }

    const std::wstring value(buffer.data(),
                             bufferSize / sizeof(wchar_t));
    std::uintmax_t contentLength = 0;
    try {
        std::size_t consumed = 0;
        contentLength = std::stoull(value, &consumed);
        if (consumed != value.size()) {
            return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return contentLength;
}

std::string readResponseBody(HINTERNET request)
{
    std::string body;
    std::array<char, 16u * 1024u> buffer{};
    while (true) {
        DWORD readSize = 0;
        if (!WinHttpReadData(request,
                             buffer.data(),
                             static_cast<DWORD>(buffer.size()),
                             &readSize)) {
            throw std::runtime_error("读取 GitHub 响应失败。");
        }
        if (readSize == 0) {
            break;
        }
        body.append(buffer.data(), readSize);
    }
    return body;
}

std::string makeLatestReleaseUrl(std::string_view repository)
{
    if (repository.empty() || repository.find('/') == std::string_view::npos
        || repository.find(' ') != std::string_view::npos) {
        throw std::runtime_error("GitHub 仓库地址配置无效。");
    }
    return "https://api.github.com/repos/" + std::string(repository)
        + "/releases/latest";
}

void validateDownloadUrl(std::string_view url)
{
    const ParsedUrl parsed = parseUrl(url);
    if (parsed.host != L"github.com") {
        throw std::runtime_error("GitHub 更新文件地址不可信。");
    }
}

}

std::optional<GitHubAppUpdateRelease> parseGitHubAppUpdateReleaseJson(
    std::string_view jsonText,
    std::string_view assetName)
{
    try {
        const nlohmann::json document =
            nlohmann::json::parse(jsonText.begin(), jsonText.end());
        if (!document.is_object()
            || !document.contains("tag_name")
            || !document.at("tag_name").is_string()
            || !document.contains("assets")
            || !document.at("assets").is_array()) {
            return std::nullopt;
        }

        const std::string tagName = document.at("tag_name").get<std::string>();
        const std::optional<int> appVersion = parseVersionFromTag(tagName);
        if (!appVersion.has_value()) {
            return std::nullopt;
        }

        for (const nlohmann::json& asset : document.at("assets")) {
            if (!asset.is_object()
                || !asset.contains("name")
                || !asset.at("name").is_string()
                || asset.at("name").get<std::string>() != assetName
                || !asset.contains("browser_download_url")
                || !asset.at("browser_download_url").is_string()
                || !asset.contains("size")
                || !asset.at("size").is_number_unsigned()) {
                continue;
            }

            const std::string downloadUrl =
                asset.at("browser_download_url").get<std::string>();
            validateDownloadUrl(downloadUrl);

            GitHubAppUpdateRelease release;
            release.SetTagName(tagName);
            release.SetAppVersion(appVersion.value());
            release.SetAssetName(asset.at("name").get<std::string>());
            release.SetDownloadUrl(downloadUrl);
            release.SetAssetSize(asset.at("size").get<std::uintmax_t>());

            if (asset.contains("digest") && asset.at("digest").is_string()) {
                const std::string digest = asset.at("digest").get<std::string>();
                if (digest.starts_with(kSha256DigestPrefix)) {
                    const std::string sha256 =
                        digest.substr(kSha256DigestPrefix.size());
                    if (!isHexDigest(sha256)) {
                        return std::nullopt;
                    }
                    release.SetSha256(sha256);
                }
            }
            return release;
        }
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }

    return std::nullopt;
}

GitHubAppUpdateRelease fetchGitHubAppUpdateRelease(
    std::string_view repository,
    std::string_view assetName)
{
    HttpGet http = openGetRequest(makeLatestReleaseUrl(repository));
    const std::wstring headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpAddRequestHeaders(http.request.Get(),
                                  headers.c_str(),
                                  static_cast<DWORD>(-1L),
                                  WINHTTP_ADDREQ_FLAG_ADD
                                      | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        throw std::runtime_error("无法设置 GitHub 更新请求头。");
    }
    if (!WinHttpSendRequest(http.request.Get(),
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0)
        || !WinHttpReceiveResponse(http.request.Get(), nullptr)) {
        throw std::runtime_error("请求 GitHub 更新信息失败。");
    }

    const DWORD statusCode = queryStatusCode(http.request.Get());
    if (statusCode != HTTP_STATUS_OK) {
        throw std::runtime_error(
            "GitHub 更新服务返回 HTTP " + std::to_string(statusCode) + "。");
    }

    const std::string body = readResponseBody(http.request.Get());
    const std::optional<GitHubAppUpdateRelease> release =
        parseGitHubAppUpdateReleaseJson(body, assetName);
    if (!release.has_value()) {
        throw std::runtime_error(
            "GitHub 发布版本中没有有效的 RelayDesk 更新文件。");
    }
    return release.value();
}

void downloadGitHubAppUpdate(
    const GitHubAppUpdateRelease& release,
    const std::filesystem::path& destinationPath,
    const std::function<bool()>& isCanceled,
    const std::function<void(std::uintmax_t)>& onProgress)
{
    try {
        if (release.GetAssetSize() == 0 || release.GetDownloadUrl().empty()) {
            throw std::runtime_error("GitHub 更新文件信息不完整。");
        }
        validateDownloadUrl(release.GetDownloadUrl());

        std::filesystem::create_directories(destinationPath.parent_path());
        std::ofstream output(destinationPath,
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("无法创建 GitHub 更新临时文件。");
        }

        HttpGet http = openGetRequest(release.GetDownloadUrl());
        if (!WinHttpSendRequest(http.request.Get(),
                                WINHTTP_NO_ADDITIONAL_HEADERS,
                                0,
                                WINHTTP_NO_REQUEST_DATA,
                                0,
                                0,
                                0)
            || !WinHttpReceiveResponse(http.request.Get(), nullptr)) {
            throw std::runtime_error("下载 GitHub 更新文件失败。");
        }

        const DWORD statusCode = queryStatusCode(http.request.Get());
        if (statusCode != HTTP_STATUS_OK) {
            throw std::runtime_error(
                "GitHub 更新文件返回 HTTP " + std::to_string(statusCode)
                + "。");
        }

        const std::uintmax_t expectedSize =
            queryContentLength(http.request.Get())
                .value_or(release.GetAssetSize());
        std::array<char, kDownloadBufferBytes> buffer{};
        std::uintmax_t receivedSize = 0;
        while (true) {
            if (isCanceled && isCanceled()) {
                throw std::runtime_error("GitHub 更新下载已取消。");
            }

            DWORD readSize = 0;
            if (!WinHttpReadData(http.request.Get(),
                                 buffer.data(),
                                 static_cast<DWORD>(buffer.size()),
                                 &readSize)) {
                throw std::runtime_error("读取 GitHub 更新文件失败。");
            }
            if (readSize == 0) {
                break;
            }

            output.write(buffer.data(),
                         static_cast<std::streamsize>(readSize));
            if (!output) {
                throw std::runtime_error("保存 GitHub 更新文件失败。");
            }
            receivedSize += static_cast<std::uintmax_t>(readSize);
            if (onProgress) {
                onProgress(receivedSize);
            }
        }
        output.close();
        if (isCanceled && isCanceled()) {
            throw std::runtime_error("GitHub 更新下载已取消。");
        }

        if (receivedSize != release.GetAssetSize()
            || expectedSize != release.GetAssetSize()) {
            throw std::runtime_error("GitHub 更新文件大小校验失败。");
        }

        if (!release.GetSha256().empty()
            && sha256FileHex(destinationPath) != release.GetSha256()) {
            throw std::runtime_error("GitHub 更新文件 SHA-256 校验失败。");
        }
    } catch (...) {
        // 取消时 async 不会执行完成回调，下载线程必须自己清理半成品。
        std::error_code cleanupError;
        std::filesystem::remove(destinationPath, cleanupError);
        cleanupError.clear();
        std::filesystem::remove(destinationPath.parent_path(), cleanupError);
        throw;
    }
}

}
