#include "platform/crash_archive.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

constexpr std::size_t kTarBlockBytes = 512u;

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int expect(bool condition, const char* message)
{
    return condition ? 0 : fail(message);
}

class TestDirectory {
public:
    TestDirectory() : path_(RELAYDESK_CRASH_ARCHIVE_TEST_WORK_DIR)
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_);
    }

    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& GetPath() const { return path_; }

protected:
    std::filesystem::path path_;
};

void writeFile(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
}

std::optional<std::vector<std::uint8_t>>
decompressGzip(const std::filesystem::path& path)
{
    gzFile input = gzopen_w(path.c_str(), "rb");
    if (input == nullptr) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> content;
    std::array<std::uint8_t, 4096> buffer{};
    while (true) {
        const int readSize = gzread(
            input, buffer.data(), static_cast<unsigned int>(buffer.size()));
        if (readSize < 0) {
            (void)gzclose(input);
            return std::nullopt;
        }
        if (readSize == 0) {
            break;
        }
        content.insert(
            content.end(), buffer.begin(), buffer.begin() + readSize);
    }
    if (gzclose(input) != Z_OK) {
        return std::nullopt;
    }
    return content;
}

std::optional<std::uintmax_t> parseTarOctal(const std::uint8_t* field,
                                            std::size_t width)
{
    std::uintmax_t value = 0;
    bool foundDigit = false;
    for (std::size_t index = 0; index < width; ++index) {
        const unsigned char character = field[index];
        if (character == '\0' || character == ' ') {
            continue;
        }
        if (character < '0' || character > '7') {
            return std::nullopt;
        }
        foundDigit = true;
        value = value * 8u + static_cast<std::uintmax_t>(character - '0');
    }
    return foundDigit ? std::optional<std::uintmax_t>(value)
                      : std::optional<std::uintmax_t>(0);
}

std::string tarString(const std::uint8_t* field, std::size_t width)
{
    const auto end = std::find(field, field + width, std::uint8_t{0});
    return std::string(reinterpret_cast<const char*>(field),
                       reinterpret_cast<const char*>(end));
}

std::optional<std::map<std::string, std::vector<std::uint8_t>>>
parseTarEntries(const std::vector<std::uint8_t>& content)
{
    std::map<std::string, std::vector<std::uint8_t>> entries;
    std::size_t offset = 0;
    while (offset + kTarBlockBytes <= content.size()) {
        const std::uint8_t* header = content.data() + offset;
        if (std::all_of(header,
                        header + kTarBlockBytes,
                        [](std::uint8_t value) { return value == 0; })) {
            return entries;
        }

        std::string name = tarString(header, 100);
        const std::string prefix = tarString(header + 345, 155);
        if (!prefix.empty()) {
            name = prefix + "/" + name;
        }
        const std::optional<std::uintmax_t> size =
            parseTarOctal(header + 124, 12);
        if (name.empty() || !size.has_value()
            || size.value() > content.size()) {
            return std::nullopt;
        }

        offset += kTarBlockBytes;
        const std::size_t fileSize = static_cast<std::size_t>(size.value());
        if (fileSize > content.size() - offset) {
            return std::nullopt;
        }
        entries[name] = std::vector<std::uint8_t>(
            content.begin() + static_cast<std::ptrdiff_t>(offset),
            content.begin() + static_cast<std::ptrdiff_t>(offset + fileSize));
        offset += ((fileSize + kTarBlockBytes - 1u) / kTarBlockBytes)
                  * kTarBlockBytes;
    }
    return std::nullopt;
}

int createsCompressedCrashArchive()
{
    TestDirectory testDirectory;
    const std::filesystem::path reportDirectory =
        testDirectory.GetPath() / "crash-20260923-120000";
    const std::filesystem::path archivePath =
        testDirectory.GetPath() / "crash.tar.gz";
    const std::vector<std::uint8_t> dumpContent(512u * 1024u, 0x5au);
    const std::vector<std::uint8_t> reportContent{
        'r', 'e', 'p', 'o', 'r', 't', '\n'};
    const std::vector<std::uint8_t> logContent{'l', 'o', 'g', '\n'};
    writeFile(reportDirectory / "crash.dmp", dumpContent);
    writeFile(reportDirectory / "report.txt", reportContent);
    writeFile(reportDirectory / "logs" / "discovery.log", logContent);
    writeFile(reportDirectory / "uploaded.txt", {'i', 'g', 'n', 'o', 'r', 'e'});

    if (const int result = expect(relaydesk::platform::createCrashReportArchive(
                                      reportDirectory, archivePath),
                                  "创建崩溃压缩包失败。");
        result != 0) {
        return result;
    }
    if (const int result =
            expect(std::filesystem::file_size(archivePath) < dumpContent.size(),
                   "崩溃压缩包没有压缩重复数据。");
        result != 0) {
        return result;
    }

    std::ifstream archiveInput(archivePath, std::ios::binary);
    std::array<std::uint8_t, 3> signature{};
    archiveInput.read(reinterpret_cast<char*>(signature.data()),
                      static_cast<std::streamsize>(signature.size()));
    if (const int result = expect(
            signature == std::array<std::uint8_t, 3>{0x1fu, 0x8bu, 0x08u},
            "崩溃压缩包不是有效的 gzip 数据。");
        result != 0) {
        return result;
    }

    const auto decompressed = decompressGzip(archivePath);
    if (const int result =
            expect(decompressed.has_value(), "无法解压崩溃压缩包。");
        result != 0) {
        return result;
    }
    const auto entries = parseTarEntries(decompressed.value());
    if (const int result =
            expect(entries.has_value(), "无法解析崩溃 tar 归档。");
        result != 0) {
        return result;
    }
    if (const int result =
            expect(entries->size() == 3u && entries->contains("crash.dmp")
                       && entries->contains("report.txt")
                       && entries->contains("logs/discovery.log")
                       && entries->at("crash.dmp") == dumpContent
                       && entries->at("report.txt") == reportContent
                       && entries->at("logs/discovery.log") == logContent,
                   "崩溃压缩包内容不完整。");
        result != 0) {
        return result;
    }
    return expect(!entries->contains("uploaded.txt"),
                  "崩溃压缩包包含上传完成标记。");
}

int rejectsReportWithoutCrashDump()
{
    TestDirectory testDirectory;
    const std::filesystem::path reportDirectory =
        testDirectory.GetPath() / "crash-without-dump";
    const std::filesystem::path archivePath =
        testDirectory.GetPath() / "missing.tar.gz";
    writeFile(reportDirectory / "report.txt", {'r', 'e', 'p', 'o', 'r', 't'});

    return expect(!relaydesk::platform::createCrashReportArchive(
                      reportDirectory, archivePath)
                      && !std::filesystem::exists(archivePath),
                  "缺少 crash.dmp 时仍创建了崩溃压缩包。");
}

} // namespace

int main()
{
    if (const int result = createsCompressedCrashArchive(); result != 0) {
        return result;
    }
    return rejectsReportWithoutCrashDump();
}
