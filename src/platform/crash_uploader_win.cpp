#include "platform/crash_uploader.h"

#include "core/app_version.h"
#include "core/time.h"
#include "platform/crash_report.h"
#include "platform/text_encoding.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace relaydesk::platform {
namespace {

constexpr wchar_t kUploadReportsArgument[] = L"--relaydesk-upload-crash-reports";
constexpr wchar_t kCrashDirectoryArgument[] = L"--crash-directory";
constexpr wchar_t kCrashReportIdArgument[] = L"--crash-report-id";
constexpr const char* kCrashDumpFileName = "crash.dmp";
// 没有环境变量或配置时使用的崩溃收集服务器。显式配置仍优先。
constexpr std::string_view kDefaultCrashUploadUrl =
    "http://39.99.153.9:10019/";
constexpr const char* kUploadMarkerFileName = "uploaded.txt";
constexpr std::size_t kUploadBufferBytes = 256u * 1024u;
constexpr std::size_t kServerMultipartMaxFiles = 8u;

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

std::optional<std::string> readEnvironmentVariable(const char* name)
{
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> readConfigString(const std::filesystem::path& configPath,
                                            const char* key)
{
    std::ifstream input(configPath, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    try {
        const nlohmann::json value = nlohmann::json::parse(input);
        if (!value.is_object() || !value.contains(key)
            || !value.at(key).is_string()) {
            return std::nullopt;
        }
        const std::string text = value.at(key).get<std::string>();
        if (text.empty()) {
            return std::nullopt;
        }
        return text;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::optional<CrashUploadOptions> resolveUploadOptions(CrashUploadOptions options)
{
    const std::filesystem::path configPath =
        options.GetCrashDirectory().parent_path() / "config.json";
    if (options.GetServerUrl().empty()) {
        if (const auto url =
                readEnvironmentVariable("RELAYDESK_CRASH_UPLOAD_URL")) {
            options.SetServerUrl(*url);
        } else if (const auto configUrl =
                       readConfigString(configPath, "crash_upload_url")) {
            options.SetServerUrl(*configUrl);
        } else {
            options.SetServerUrl(std::string(kDefaultCrashUploadUrl));
        }
    }
    if (options.GetApiKey().empty()) {
        if (const auto apiKey =
                readEnvironmentVariable("RELAYDESK_CRASH_UPLOAD_API_KEY")) {
            options.SetApiKey(*apiKey);
        } else if (const auto configApiKey =
                       readConfigString(configPath, "crash_upload_api_key")) {
            options.SetApiKey(*configApiKey);
        }
    }
    if (options.GetServerUrl().empty()) {
        return std::nullopt;
    }
    return options;
}

std::wstring appendQuery(std::wstring path,
                         std::wstring_view key,
                         std::wstring_view value)
{
    path += path.find(L'?') == std::wstring::npos ? L'?' : L'&';
    path += key;
    path += L'=';
    path += value;
    return path;
}

struct UploadFile {
    std::filesystem::path path;
    std::string serverFileName;
    std::uintmax_t size = 0;
};

std::string makeServerFileName(const std::filesystem::path& reportDirectory,
                               const std::filesystem::path& filePath)
{
    std::error_code error;
    const std::filesystem::path relativePath =
        std::filesystem::relative(filePath, reportDirectory, error);
    const std::string relativeText =
        error ? filePath.filename().generic_string()
              : relativePath.generic_string();
    if (relativeText == kCrashDumpFileName) {
        return kCrashDumpFileName;
    }

    std::string result;
    for (const char character : relativeText) {
        if (character == '/') {
            result += "__";
        } else if ((character >= 'a' && character <= 'z')
                   || (character >= 'A' && character <= 'Z')
                   || (character >= '0' && character <= '9')
                   || character == '.' || character == '-' || character == '_') {
            result.push_back(character);
        } else {
            result.push_back('_');
        }
    }
    return result;
}

std::optional<std::vector<UploadFile>> collectUploadFiles(
    const CrashReportEntry& report)
{
    std::vector<UploadFile> files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        report.GetDirectory(),
        std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error) {
        return std::nullopt;
    }

    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entryError;
        if (!entry.is_regular_file(entryError)) {
            continue;
        }
        if (entry.path().filename() == kUploadMarkerFileName) {
            continue;
        }
        const std::uintmax_t size = entry.file_size(entryError);
        if (entryError) {
            continue;
        }
        UploadFile file;
        file.path = entry.path();
        file.serverFileName =
            makeServerFileName(report.GetDirectory(), entry.path());
        file.size = size;
        files.push_back(std::move(file));
    }

    std::sort(files.begin(),
              files.end(),
              [](const UploadFile& left, const UploadFile& right) {
                  if (left.serverFileName == kCrashDumpFileName) {
                      return right.serverFileName != kCrashDumpFileName;
                  }
                  if (right.serverFileName == kCrashDumpFileName) {
                      return false;
                  }
                  return left.serverFileName < right.serverFileName;
              });
    return files;
}

std::wstring contentTypeForFile(std::string_view fileName)
{
    const std::size_t dot = fileName.find_last_of('.');
    if (dot != std::string_view::npos) {
        const std::string_view extension = fileName.substr(dot);
        if (extension == ".txt" || extension == ".log") {
            return L"text/plain; charset=utf-8";
        }
    }
    return L"application/octet-stream";
}

std::string makeMultipartField(std::string_view boundary,
                               std::string_view name,
                               std::string_view value)
{
    std::string result = "--";
    result += boundary;
    result += "\r\nContent-Disposition: form-data; name=\"";
    result += name;
    result += "\"\r\n\r\n";
    result += value;
    result += "\r\n";
    return result;
}

std::string makeMultipartFileHeader(std::string_view boundary,
                                    const UploadFile& file)
{
    std::string result = "--";
    result += boundary;
    result += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
    result += file.serverFileName;
    result += "\"\r\nContent-Type: ";
    result += wideToUtf8(contentTypeForFile(file.serverFileName));
    result += "\r\n\r\n";
    return result;
}

bool writeRequestData(HINTERNET request, std::string_view data)
{
    while (!data.empty()) {
        const DWORD chunkSize = static_cast<DWORD>(std::min<std::size_t>(
            data.size(), static_cast<std::size_t>(
                             std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WinHttpWriteData(request,
                              data.data(),
                              chunkSize,
                              &written)
            || written != chunkSize) {
            return false;
        }
        data.remove_prefix(written);
    }
    return true;
}

bool writeFileData(HINTERNET request, const UploadFile& file)
{
    std::ifstream input(file.path, std::ios::binary);
    if (!input) {
        return false;
    }

    std::array<char, kUploadBufferBytes> buffer{};
    std::uintmax_t remaining = file.size;
    while (remaining > 0) {
        const std::streamsize requested = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(remaining, buffer.size()));
        input.read(buffer.data(), requested);
        const std::streamsize readSize = input.gcount();
        if (readSize <= 0) {
            return false;
        }

        DWORD written = 0;
        if (!WinHttpWriteData(request,
                              buffer.data(),
                              static_cast<DWORD>(readSize),
                              &written)
            || written != static_cast<DWORD>(readSize)) {
            return false;
        }
        remaining -= static_cast<std::uintmax_t>(readSize);
    }
    return !input.bad();
}

std::optional<std::string> readResponseBody(HINTERNET request)
{
    std::string body;
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD readSize = 0;
        if (!WinHttpReadData(request,
                             buffer.data(),
                             static_cast<DWORD>(buffer.size()),
                             &readSize)) {
            return std::nullopt;
        }
        if (readSize == 0) {
            break;
        }
        body.append(buffer.data(), readSize);
    }
    return body;
}

bool uploadFiles(const std::vector<UploadFile>& files,
                 std::size_t firstFile,
                 std::size_t fileCount,
                 const CrashUploadOptions& options,
                 const std::string& reportId)
{
    if (fileCount == 0 || fileCount > kServerMultipartMaxFiles
        || firstFile > files.size()
        || fileCount > files.size() - firstFile) {
        return false;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    std::array<wchar_t, 256> hostBuffer{};
    std::array<wchar_t, 2048> pathBuffer{};
    std::array<wchar_t, 2048> extraInfoBuffer{};
    components.lpszHostName = hostBuffer.data();
    components.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
    components.lpszUrlPath = pathBuffer.data();
    components.dwUrlPathLength = static_cast<DWORD>(pathBuffer.size());
    components.lpszExtraInfo = extraInfoBuffer.data();
    components.dwExtraInfoLength = static_cast<DWORD>(extraInfoBuffer.size());
    std::wstring url = utf8ToWide(options.GetServerUrl());
    std::vector<wchar_t> mutableUrl(url.begin(), url.end());
    mutableUrl.push_back(L'\0');
    if (!WinHttpCrackUrl(mutableUrl.data(),
                         static_cast<DWORD>(url.size()),
                         0,
                         &components)) {
        return false;
    }

    const std::wstring host(hostBuffer.data(), components.dwHostNameLength);
    std::wstring path(pathBuffer.data(), components.dwUrlPathLength);
    path.append(extraInfoBuffer.data(), components.dwExtraInfoLength);
    // WinHttpCrackUrl 对没有显式路径的地址通常返回 "/"；这里仍应把它视为
    // 服务器根地址，并补上 MinidumpServer 的正式上传端点。
    if (path.empty() || path == L"/") {
        path = L"/api/dumps";
    }
    path = appendQuery(path, L"project", L"RelayDesk");
    path = appendQuery(path,
                       L"version",
                       utf8ToWide(std::to_string(relaydesk::core::kAppVersion)));
    path = appendQuery(path, L"label", L"relaydesk");
    path = appendQuery(path, L"note", utf8ToWide(reportId));
    path = appendQuery(path, L"submission_id", utf8ToWide(reportId));

    const DWORD flags =
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle session(WinHttpOpen(L"RelayDesk crash uploader/1",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS,
                                       0));
    if (!session) {
        return false;
    }
    WinHttpSetTimeouts(session.Get(), 10000, 10000, 30000, 30000);

    WinHttpHandle connection(
        WinHttpConnect(session.Get(), host.c_str(), components.nPort, 0));
    if (!connection) {
        return false;
    }

    WinHttpHandle request(WinHttpOpenRequest(connection.Get(),
                                             L"POST",
                                             path.c_str(),
                                             nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             flags));
    if (!request) {
        return false;
    }

    constexpr std::string_view boundary = "RelayDeskCrashBoundary7B9C1D2E";
    const std::vector<std::string> fields = {
        makeMultipartField(boundary, "project", "RelayDesk"),
        makeMultipartField(
            boundary,
            "version",
            std::to_string(relaydesk::core::kAppVersion)),
        makeMultipartField(boundary, "label", "relaydesk"),
        makeMultipartField(boundary, "note", reportId),
        makeMultipartField(boundary, "submission_id", reportId),
    };
    std::vector<std::string> fileHeaders;
    fileHeaders.reserve(fileCount);
    for (std::size_t index = firstFile;
         index < firstFile + fileCount;
         ++index) {
        fileHeaders.push_back(makeMultipartFileHeader(boundary, files[index]));
    }
    const std::string closingBoundary =
        "--" + std::string(boundary) + "--\r\n";

    std::uint64_t totalLength = closingBoundary.size();
    const auto addLength = [&totalLength](std::uintmax_t length) {
        if (length > std::numeric_limits<std::uint64_t>::max() - totalLength) {
            return false;
        }
        totalLength += static_cast<std::uint64_t>(length);
        return true;
    };
    for (const std::string& field : fields) {
        if (!addLength(field.size())) {
            return false;
        }
    }
    for (std::size_t index = 0; index < fileHeaders.size(); ++index) {
        if (!addLength(fileHeaders[index].size())
            || !addLength(files[firstFile + index].size)
            || !addLength(2u)) {
            return false;
        }
    }
    if (totalLength > std::numeric_limits<DWORD>::max()) {
        return false;
    }

    std::wstring headers = L"Content-Type: multipart/form-data; boundary="
        + utf8ToWide(std::string(boundary)) + L"\r\n";
    if (!options.GetApiKey().empty()) {
        headers += L"X-Api-Key: ";
        headers += utf8ToWide(options.GetApiKey());
        headers += L"\r\n";
    }
    if (!WinHttpAddRequestHeaders(request.Get(),
                                  headers.c_str(),
                                  static_cast<DWORD>(-1L),
                                  WINHTTP_ADDREQ_FLAG_ADD
                                      | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return false;
    }

    if (!WinHttpSendRequest(request.Get(),
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            static_cast<DWORD>(totalLength),
                            0)) {
        return false;
    }
    for (const std::string& field : fields) {
        if (!writeRequestData(request.Get(), field)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < fileHeaders.size(); ++index) {
        if (!writeRequestData(request.Get(), fileHeaders[index])
            || !writeFileData(request.Get(), files[firstFile + index])
            || !writeRequestData(request.Get(), "\r\n")) {
            return false;
        }
    }
    if (!writeRequestData(request.Get(), closingBoundary)) {
        return false;
    }
    if (!WinHttpReceiveResponse(request.Get(), nullptr)) {
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request.Get(),
                             WINHTTP_QUERY_STATUS_CODE
                                 | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode,
                                 &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    if (statusCode != 200 && statusCode != 201) {
        return false;
    }

    const std::optional<std::string> responseBody =
        readResponseBody(request.Get());
    if (!responseBody.has_value()) {
        return false;
    }
    try {
        const nlohmann::json response =
            nlohmann::json::parse(*responseBody);
        return response.value("ok", false)
            && response.value("stored", std::size_t(0)) == fileCount
            && response.value("submission_id", std::string()) == reportId;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool hasUploadConfiguration(const std::filesystem::path& crashDirectory)
{
    CrashUploadOptions options;
    options.SetCrashDirectory(crashDirectory);
    return resolveUploadOptions(std::move(options)).has_value();
}

} // namespace

std::optional<CrashUploadOptions> parseCrashUploadOptions(
    const std::vector<std::wstring>& arguments)
{
    CrashUploadOptions options;
    bool uploadRequested = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == kUploadReportsArgument) {
            uploadRequested = true;
        } else if (argument == kCrashDirectoryArgument
                   && index + 1 < arguments.size()) {
            options.SetCrashDirectory(
                std::filesystem::path(arguments[++index]));
        } else if (argument == kCrashReportIdArgument
                   && index + 1 < arguments.size()) {
            options.SetReportId(
                wideToUtf8(arguments[++index]));
        }
    }
    if (!uploadRequested || options.GetCrashDirectory().empty()) {
        return std::nullopt;
    }
    return resolveUploadOptions(std::move(options));
}

int runCrashUploadMode(const CrashUploadOptions& options) noexcept
{
    try {
        const std::vector<CrashReportEntry> reports =
            findPendingCrashReports(options.GetCrashDirectory());
        bool failed = false;
        for (const CrashReportEntry& report : reports) {
            if (!options.GetReportId().empty()
                && report.GetReportId() != options.GetReportId()) {
                continue;
            }
            const std::optional<std::vector<UploadFile>> files =
                collectUploadFiles(report);
            if (!files.has_value() || files->empty()) {
                failed = true;
                continue;
            }

            const bool hasCrashDump = std::any_of(
                files->begin(),
                files->end(),
                [](const UploadFile& file) {
                    return file.serverFileName == kCrashDumpFileName;
                });
            if (!hasCrashDump) {
                failed = true;
                continue;
            }

            bool reportFailed = false;
            for (std::size_t firstFile = 0;
                 firstFile < files->size();
                 firstFile += kServerMultipartMaxFiles) {
                const std::size_t fileCount = std::min(
                    kServerMultipartMaxFiles,
                    files->size() - firstFile);
                if (!uploadFiles(*files,
                                 firstFile,
                                 fileCount,
                                 options,
                                 report.GetReportId())) {
                    reportFailed = true;
                }
            }
            if (reportFailed) {
                failed = true;
            } else {
                markCrashReportUploaded(
                    report.GetDirectory(),
                    relaydesk::core::currentUtcTimestamp());
            }
        }
        return failed ? 1 : 0;
    } catch (...) {
        return 1;
    }
}

} // namespace relaydesk::platform
