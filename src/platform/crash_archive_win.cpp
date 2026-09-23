#include "platform/crash_archive.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include <zlib.h>

namespace relaydesk::platform {
namespace {

constexpr std::size_t kTarBlockBytes = 512u;
constexpr std::size_t kArchiveBufferBytes = 256u * 1024u;
constexpr char kCrashDumpFileName[] = "crash.dmp";
constexpr char kUploadMarkerFileName[] = "uploaded.txt";

struct ArchiveEntry {
    std::filesystem::path path;
    std::string name;
    std::uintmax_t size = 0;
};

class GzipFile {
public:
    explicit GzipFile(const std::filesystem::path& path)
        : file_(gzopen_w(path.c_str(), "wb6"))
    {
    }

    ~GzipFile()
    {
        if (file_ != nullptr) {
            (void)gzclose(file_);
        }
    }

    GzipFile(const GzipFile&) = delete;
    GzipFile& operator=(const GzipFile&) = delete;

    gzFile Get() const { return file_; }

    bool Close()
    {
        if (file_ == nullptr) {
            return false;
        }
        gzFile file = file_;
        file_ = nullptr;
        return gzclose(file) == Z_OK;
    }

protected:
    gzFile file_ = nullptr;
};

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

bool writeGzipData(gzFile file, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size > 0) {
        const unsigned int chunkSize =
            static_cast<unsigned int>(std::min<std::size_t>(
                size,
                static_cast<std::size_t>(
                    std::numeric_limits<unsigned int>::max())));
        const int written = gzwrite(file, bytes, chunkSize);
        if (written != static_cast<int>(chunkSize)) {
            return false;
        }
        bytes += chunkSize;
        size -= chunkSize;
    }
    return true;
}

bool writeTarOctal(char* field, std::size_t width, std::uintmax_t value)
{
    std::array<char, 32> digits{};
    const auto result =
        std::to_chars(digits.data(), digits.data() + digits.size(), value, 8);
    if (result.ec != std::errc{}) {
        return false;
    }

    const std::size_t digitCount =
        static_cast<std::size_t>(result.ptr - digits.data());
    if (digitCount + 1u > width) {
        return false;
    }

    std::fill(field, field + width, '0');
    std::copy(digits.data(), result.ptr, field + width - digitCount - 1u);
    field[width - 1u] = '\0';
    return true;
}

bool writeTarChecksum(std::array<char, kTarBlockBytes>& header)
{
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    std::uintmax_t checksum = 0;
    for (const char value : header) {
        checksum += static_cast<unsigned char>(value);
    }

    std::array<char, 16> digits{};
    const auto result = std::to_chars(
        digits.data(), digits.data() + digits.size(), checksum, 8);
    if (result.ec != std::errc{}) {
        return false;
    }
    const std::size_t digitCount =
        static_cast<std::size_t>(result.ptr - digits.data());
    if (digitCount > 6u) {
        return false;
    }

    std::fill(header.begin() + 148, header.begin() + 154, '0');
    std::copy(digits.data(), result.ptr, header.begin() + 154 - digitCount);
    header[154] = '\0';
    header[155] = ' ';
    return true;
}

bool writeTarName(std::array<char, kTarBlockBytes>& header,
                  const std::string& name)
{
    if (name.empty() || name.front() == '/') {
        return false;
    }
    if (name.size() <= 100u) {
        std::copy(name.begin(), name.end(), header.begin());
        return true;
    }

    std::size_t separator = name.rfind('/');
    while (separator != std::string::npos) {
        const std::size_t prefixLength = separator;
        const std::size_t fileNameLength = name.size() - separator - 1u;
        if (prefixLength <= 155u && fileNameLength <= 100u) {
            std::copy(name.begin(),
                      name.begin() + static_cast<std::ptrdiff_t>(separator),
                      header.begin() + 345);
            std::copy(name.begin()
                          + static_cast<std::ptrdiff_t>(separator + 1u),
                      name.end(),
                      header.begin());
            return true;
        }
        if (separator == 0) {
            break;
        }
        separator = name.rfind('/', separator - 1u);
    }
    return false;
}

bool writeTarHeader(gzFile file, const ArchiveEntry& entry)
{
    std::array<char, kTarBlockBytes> header{};
    if (!writeTarName(header, entry.name)
        || !writeTarOctal(header.data() + 100, 8, 0644u)
        || !writeTarOctal(header.data() + 108, 8, 0)
        || !writeTarOctal(header.data() + 116, 8, 0)
        || !writeTarOctal(header.data() + 124, 12, entry.size)
        || !writeTarOctal(header.data() + 136, 12, 0)) {
        return false;
    }

    header[156] = '0';
    const std::string magic = "ustar";
    std::copy(magic.begin(), magic.end(), header.begin() + 257);
    header[262] = '\0';
    header[263] = '0';
    header[264] = '0';
    const std::string owner = "RelayDesk";
    std::copy(owner.begin(), owner.end(), header.begin() + 265);
    std::copy(owner.begin(), owner.end(), header.begin() + 297);
    if (!writeTarChecksum(header)) {
        return false;
    }
    return writeGzipData(file, header.data(), header.size());
}

bool writeArchiveEntry(gzFile file, const ArchiveEntry& entry)
{
    if (!writeTarHeader(file, entry)) {
        return false;
    }

    std::ifstream input(entry.path, std::ios::binary);
    if (!input) {
        return false;
    }

    std::array<char, kArchiveBufferBytes> buffer{};
    std::uintmax_t remaining = entry.size;
    while (remaining > 0) {
        const std::streamsize requested = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(remaining, buffer.size()));
        input.read(buffer.data(), requested);
        const std::streamsize readSize = input.gcount();
        if (readSize <= 0
            || !writeGzipData(
                file, buffer.data(), static_cast<std::size_t>(readSize))) {
            return false;
        }
        remaining -= static_cast<std::uintmax_t>(readSize);
    }
    if (input.bad()) {
        return false;
    }

    const std::size_t padding =
        (kTarBlockBytes - (entry.size % kTarBlockBytes)) % kTarBlockBytes;
    const std::array<char, kTarBlockBytes> zeros{};
    return padding == 0 || writeGzipData(file, zeros.data(), padding);
}

std::vector<ArchiveEntry>
collectArchiveEntries(const std::filesystem::path& reportDirectory,
                      const std::filesystem::path& archivePath,
                      bool& valid)
{
    valid = false;
    std::vector<ArchiveEntry> entries;
    std::error_code error;
    const std::filesystem::path normalizedArchivePath =
        std::filesystem::absolute(archivePath, error).lexically_normal();
    if (error) {
        return entries;
    }

    std::filesystem::recursive_directory_iterator iterator(
        reportDirectory, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry& directoryEntry = *iterator;
        const std::filesystem::file_status status =
            directoryEntry.symlink_status(error);
        if (error) {
            break;
        }
        if (status.type() == std::filesystem::file_type::regular) {
            const std::filesystem::path normalizedPath =
                std::filesystem::absolute(directoryEntry.path(), error)
                    .lexically_normal();
            if (error) {
                break;
            }
            if (normalizedPath != normalizedArchivePath
                && directoryEntry.path().filename() != kUploadMarkerFileName) {
                const std::filesystem::path relativePath =
                    std::filesystem::relative(
                        directoryEntry.path(), reportDirectory, error);
                if (error) {
                    break;
                }
                const std::uintmax_t size = directoryEntry.file_size(error);
                if (error) {
                    break;
                }
                ArchiveEntry entry;
                entry.path = directoryEntry.path();
                entry.name = pathToUtf8(relativePath);
                entry.size = size;
                entries.push_back(std::move(entry));
            }
        }
        iterator.increment(error);
    }
    if (error) {
        return {};
    }

    std::sort(entries.begin(),
              entries.end(),
              [](const ArchiveEntry& left, const ArchiveEntry& right) {
                  if (left.name == kCrashDumpFileName) {
                      return right.name != kCrashDumpFileName;
                  }
                  if (right.name == kCrashDumpFileName) {
                      return false;
                  }
                  return left.name < right.name;
              });
    valid = std::any_of(
        entries.begin(), entries.end(), [](const ArchiveEntry& entry) {
            return entry.name == kCrashDumpFileName;
        });
    return entries;
}

} // namespace

bool createCrashReportArchive(const std::filesystem::path& reportDirectory,
                              const std::filesystem::path& archivePath)
{
    bool entriesValid = false;
    const std::vector<ArchiveEntry> entries =
        collectArchiveEntries(reportDirectory, archivePath, entriesValid);
    if (!entriesValid || entries.empty()) {
        return false;
    }

    std::error_code error;
    std::filesystem::remove(archivePath, error);
    GzipFile output(archivePath);
    if (output.Get() == nullptr) {
        return false;
    }

    bool complete = true;
    for (const ArchiveEntry& entry : entries) {
        if (!writeArchiveEntry(output.Get(), entry)) {
            complete = false;
            break;
        }
    }

    const std::array<char, kTarBlockBytes * 2u> endBlocks{};
    if (complete) {
        complete =
            writeGzipData(output.Get(), endBlocks.data(), endBlocks.size());
    }
    const bool closed = output.Close();
    if (!complete || !closed) {
        std::filesystem::remove(archivePath, error);
        return false;
    }
    return true;
}

} // namespace relaydesk::platform
