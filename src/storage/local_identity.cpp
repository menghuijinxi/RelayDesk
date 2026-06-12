#include "storage/local_identity.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <string_view>

namespace relaydesk::storage {
namespace {

constexpr int kSchemaVersion = 1;

std::string createUuidV4()
{
    std::array<unsigned char, 16> bytes{};
    std::random_device randomDevice;
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(randomDevice());
    }

    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3Fu) | 0x80u);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << static_cast<int>(bytes[index]);
    }
    return output.str();
}

std::string createUtcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm utcTime{};

#if defined(_WIN32)
    if (gmtime_s(&utcTime, &nowTime) != 0) {
        throw std::runtime_error("Failed to convert identity timestamp to UTC.");
    }
#else
    if (gmtime_r(&nowTime, &utcTime) == nullptr) {
        throw std::runtime_error("Failed to convert identity timestamp to UTC.");
    }
#endif

    std::ostringstream output;
    output << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string escapeJsonString(const std::string& value)
{
    std::ostringstream output;
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (byte < 0x20u) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(byte) << std::dec;
            } else {
                output << static_cast<char>(byte);
            }
            break;
        }
    }
    return output.str();
}

int hexDigitValue(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }

    throw std::runtime_error("Local identity string escape is unsupported.");
}

void appendUtf8CodePoint(std::string& value, unsigned int codePoint)
{
    if (codePoint <= 0x7Fu) {
        value.push_back(static_cast<char>(codePoint));
        return;
    }

    if (codePoint <= 0x7FFu) {
        value.push_back(static_cast<char>(0xC0u | ((codePoint >> 6) & 0x1Fu)));
        value.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        return;
    }

    if (codePoint <= 0xFFFFu) {
        value.push_back(static_cast<char>(0xE0u | ((codePoint >> 12) & 0x0Fu)));
        value.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        value.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        return;
    }

    if (codePoint <= 0x10FFFFu) {
        value.push_back(static_cast<char>(0xF0u | ((codePoint >> 18) & 0x07u)));
        value.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        value.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        value.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        return;
    }

    throw std::runtime_error("Local identity string escape is unsupported.");
}

std::string readTextFile(const std::filesystem::path& filePath)
{
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open local identity file for reading.");
    }

    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void skipWhitespace(std::string_view content, std::size_t& offset)
{
    while (offset < content.size()
           && std::isspace(static_cast<unsigned char>(content[offset])) != 0) {
        ++offset;
    }
}

std::size_t findValueStart(std::string_view content, std::string_view key)
{
    const std::string quotedKey = "\"" + std::string(key) + "\"";
    const std::size_t keyOffset = content.find(quotedKey);
    if (keyOffset == std::string_view::npos) {
        throw std::runtime_error("Local identity file is missing a required field.");
    }

    std::size_t colonOffset = content.find(':', keyOffset + quotedKey.size());
    if (colonOffset == std::string_view::npos) {
        throw std::runtime_error("Local identity field is missing a value separator.");
    }

    ++colonOffset;
    skipWhitespace(content, colonOffset);
    return colonOffset;
}

int readJsonInt(std::string_view content, std::string_view key)
{
    const std::size_t valueOffset = findValueStart(content, key);
    std::size_t valueEnd = valueOffset;
    while (valueEnd < content.size()
           && std::isdigit(static_cast<unsigned char>(content[valueEnd])) != 0) {
        ++valueEnd;
    }

    int value = 0;
    const auto result = std::from_chars(
        content.data() + valueOffset,
        content.data() + valueEnd,
        value);
    if (result.ec != std::errc{} || result.ptr != content.data() + valueEnd) {
        throw std::runtime_error("Local identity integer field is invalid.");
    }
    return value;
}

std::string readJsonString(std::string_view content, std::string_view key)
{
    std::size_t offset = findValueStart(content, key);
    if (offset >= content.size() || content[offset] != '"') {
        throw std::runtime_error("Local identity string field is invalid.");
    }

    ++offset;
    std::string value;
    while (offset < content.size()) {
        const char current = content[offset++];
        if (current == '"') {
            return value;
        }
        if (current != '\\') {
            value.push_back(current);
            continue;
        }

        if (offset >= content.size()) {
            throw std::runtime_error("Local identity string escape is incomplete.");
        }

        const char escaped = content[offset++];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            value.push_back(escaped);
            break;
        case 'b':
            value.push_back('\b');
            break;
        case 'f':
            value.push_back('\f');
            break;
        case 'n':
            value.push_back('\n');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case 't':
            value.push_back('\t');
            break;
        case 'u': {
            if (offset + 4 > content.size()) {
                throw std::runtime_error("Local identity string escape is incomplete.");
            }

            unsigned int codePoint = 0;
            for (int digitIndex = 0; digitIndex < 4; ++digitIndex) {
                codePoint = static_cast<unsigned int>(codePoint << 4)
                    | static_cast<unsigned int>(hexDigitValue(content[offset++]));
            }

            if (codePoint >= 0xD800u && codePoint <= 0xDBFFu) {
                if (offset + 6 > content.size() || content[offset] != '\\'
                    || content[offset + 1] != 'u') {
                    throw std::runtime_error("Local identity string escape is unsupported.");
                }

                offset += 2;
                unsigned int lowSurrogate = 0;
                for (int digitIndex = 0; digitIndex < 4; ++digitIndex) {
                    lowSurrogate = static_cast<unsigned int>(lowSurrogate << 4)
                        | static_cast<unsigned int>(hexDigitValue(content[offset++]));
                }

                if (lowSurrogate < 0xDC00u || lowSurrogate > 0xDFFFu) {
                    throw std::runtime_error("Local identity string escape is unsupported.");
                }

                const unsigned int combinedCodePoint = 0x10000u
                    + ((codePoint - 0xD800u) << 10)
                    + (lowSurrogate - 0xDC00u);
                appendUtf8CodePoint(value, combinedCodePoint);
                break;
            }

            if (codePoint >= 0xDC00u && codePoint <= 0xDFFFu) {
                throw std::runtime_error("Local identity string escape is unsupported.");
            }

            appendUtf8CodePoint(value, codePoint);
            break;
        }
        default:
            throw std::runtime_error("Local identity string escape is unsupported.");
        }
    }

    throw std::runtime_error("Local identity string field is not terminated.");
}

void validateIdentity(const LocalIdentity& identity)
{
    if (identity.GetSchemaVersion() != kSchemaVersion
        || identity.GetDeviceId().empty()
        || identity.GetInstallId().empty()
        || identity.GetCreatedAt().empty()
        || identity.GetHostName().empty()
        || identity.GetDisplayName().empty()) {
        throw std::runtime_error("Local identity file contains invalid required fields.");
    }
}

LocalIdentity createLocalIdentity(const std::string& hostName)
{
    if (hostName.empty()) {
        throw std::invalid_argument("Local identity host name cannot be empty.");
    }

    return LocalIdentity(createUuidV4(), createUuidV4(), createUtcTimestamp(), hostName,
                         hostName);
}

} // namespace

LocalIdentity::LocalIdentity(std::string deviceId,
                             std::string installId,
                             std::string createdAt,
                             std::string hostName,
                             std::string displayName)
    : deviceId_(std::move(deviceId)),
      installId_(std::move(installId)),
      createdAt_(std::move(createdAt)),
      hostName_(std::move(hostName)),
      displayName_(std::move(displayName))
{
}

LocalIdentity loadLocalIdentity(const AppPaths& appPaths)
{
    const std::string content = readTextFile(appPaths.GetIdentityFilePath());
    const int schemaVersion = readJsonInt(content, "schema_version");
    if (schemaVersion != kSchemaVersion) {
        throw std::runtime_error("Local identity schema version is unsupported.");
    }

    LocalIdentity identity(
        readJsonString(content, "device_id"),
        readJsonString(content, "install_id"),
        readJsonString(content, "created_at"),
        readJsonString(content, "host_name"),
        readJsonString(content, "display_name"));
    validateIdentity(identity);
    return identity;
}

void saveLocalIdentity(const AppPaths& appPaths, const LocalIdentity& identity)
{
    validateIdentity(identity);
    std::filesystem::create_directories(appPaths.GetDataDirectory());

    std::ofstream output(appPaths.GetIdentityFilePath(),
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open local identity file for writing.");
    }

    output << "{\n"
           << "  \"schema_version\": " << kSchemaVersion << ",\n"
           << "  \"device_id\": \"" << escapeJsonString(identity.GetDeviceId()) << "\",\n"
           << "  \"install_id\": \"" << escapeJsonString(identity.GetInstallId()) << "\",\n"
           << "  \"created_at\": \"" << escapeJsonString(identity.GetCreatedAt()) << "\",\n"
           << "  \"host_name\": \"" << escapeJsonString(identity.GetHostName()) << "\",\n"
           << "  \"display_name\": \"" << escapeJsonString(identity.GetDisplayName()) << "\"\n"
           << "}\n";

    if (!output) {
        throw std::runtime_error("Failed to write local identity file.");
    }
}

LocalIdentity loadOrCreateLocalIdentity(const AppPaths& appPaths, const std::string& hostName)
{
    if (!std::filesystem::exists(appPaths.GetIdentityFilePath())) {
        LocalIdentity identity = createLocalIdentity(hostName);
        saveLocalIdentity(appPaths, identity);
        return identity;
    }

    LocalIdentity identity = loadLocalIdentity(appPaths);
    if (identity.GetHostName() != hostName) {
        identity.SetHostName(hostName);
        saveLocalIdentity(appPaths, identity);
    }
    return identity;
}

LocalIdentity updateLocalDisplayName(const AppPaths& appPaths,
                                     const std::string& displayName)
{
    if (displayName.empty()) {
        throw std::invalid_argument("Local identity display name cannot be empty.");
    }

    LocalIdentity identity = loadLocalIdentity(appPaths);
    identity.SetDisplayName(displayName);
    saveLocalIdentity(appPaths, identity);
    return identity;
}

}
