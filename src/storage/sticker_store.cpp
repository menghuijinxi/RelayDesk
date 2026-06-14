#include "storage/sticker_store.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace relaydesk::storage {
namespace {

constexpr int kStickerPackSchemaVersion = 1;
constexpr const char* kFavoritesPackId = "favorites";
constexpr const char* kFavoritesPackName = "收藏表情";

std::string toJsonValue(StickerPackImportFormat importFormat)
{
    switch (importFormat) {
    case StickerPackImportFormat::RelayDeskManifest:
        return "relaydesk_manifest";
    case StickerPackImportFormat::OwOJson:
        return "owo_json";
    case StickerPackImportFormat::ImageFolder:
        return "image_folder";
    }

    throw std::runtime_error("Unsupported sticker pack import format.");
}

StickerPackImportFormat importFormatFromJsonValue(const std::string& value)
{
    if (value == "relaydesk_manifest") {
        return StickerPackImportFormat::RelayDeskManifest;
    }
    if (value == "owo_json") {
        return StickerPackImportFormat::OwOJson;
    }
    if (value == "image_folder") {
        return StickerPackImportFormat::ImageFolder;
    }

    throw std::runtime_error("Unsupported sticker pack import format value.");
}

std::string makeSafeId(const std::string& value, const char* fallback)
{
    std::string result;
    bool previousSeparator = false;
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0) {
            result.push_back(static_cast<char>(std::tolower(character)));
            previousSeparator = false;
            continue;
        }

        if (character == '-' || character == '_') {
            result.push_back(static_cast<char>(character));
            previousSeparator = character == '-' || character == '_';
            continue;
        }

        if (!previousSeparator && !result.empty()) {
            result.push_back('-');
            previousSeparator = true;
        }
    }

    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    if (result.empty()) {
        result = fallback;
    }
    if (result.size() > 64u) {
        result.resize(64u);
        while (!result.empty() && result.back() == '-') {
            result.pop_back();
        }
    }
    return result.empty() ? fallback : result;
}

std::string lowercaseExtension(const std::filesystem::path& filePath)
{
    std::string extension = filePath.extension().string();
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension;
}

bool isSupportedStickerImage(const std::filesystem::path& filePath)
{
    const std::string extension = lowercaseExtension(filePath);
    return extension == ".png"
        || extension == ".jpg"
        || extension == ".jpeg"
        || extension == ".gif"
        || extension == ".webp";
}

std::string pathToUtf8String(const std::filesystem::path& filePath)
{
    const auto value = filePath.u8string();
    return std::string(value.begin(), value.end());
}

std::filesystem::path getPackManifestPath(const std::filesystem::path& packDirectory)
{
    return packDirectory / "manifest.json";
}

std::filesystem::path getPackItemsDirectory(const std::filesystem::path& packDirectory)
{
    return packDirectory / "items";
}

std::string makeRelativeToWorkDirectory(const AppPaths& appPaths,
                                        const std::filesystem::path& filePath)
{
    const std::filesystem::path relativePath =
        std::filesystem::relative(filePath, appPaths.GetWorkDirectory());
    if (relativePath.empty() || relativePath.is_absolute()) {
        throw std::runtime_error("Sticker path is outside the work directory.");
    }

    const auto begin = relativePath.begin();
    if (begin != relativePath.end() && *begin == "..") {
        throw std::runtime_error("Sticker path is outside the work directory.");
    }

    return relativePath.generic_string();
}

bool isPathInsideDirectory(const std::filesystem::path& directory,
                           const std::filesystem::path& filePath)
{
    const std::filesystem::path canonicalDirectory =
        std::filesystem::weakly_canonical(directory);
    const std::filesystem::path canonicalFile = std::filesystem::weakly_canonical(filePath);
    const std::filesystem::path relativePath =
        std::filesystem::relative(canonicalFile, canonicalDirectory);
    if (relativePath.empty() || relativePath.is_absolute()) {
        return false;
    }

    const auto begin = relativePath.begin();
    return begin == relativePath.end() || *begin != "..";
}

std::filesystem::path makeUniqueDestination(const std::filesystem::path& directory,
                                            const std::string& preferredStem,
                                            const std::string& extension)
{
    std::filesystem::create_directories(directory);
    std::filesystem::path destination = directory / (preferredStem + extension);
    for (int index = 2; std::filesystem::exists(destination); ++index) {
        destination = directory / (preferredStem + "-" + std::to_string(index) + extension);
    }
    return destination;
}

nlohmann::json readJsonFile(const std::filesystem::path& filePath)
{
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open sticker JSON file for reading.");
    }

    try {
        return nlohmann::json::parse(input);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Sticker JSON file contains invalid JSON.");
    }
}

void writeJsonFile(const std::filesystem::path& filePath,
                   const nlohmann::json& value)
{
    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open sticker JSON file for writing.");
    }

    output << value.dump(4) << '\n';
    if (!output) {
        throw std::runtime_error("Failed to write sticker JSON file.");
    }
}

void requireStringField(const nlohmann::json& value, const char* fieldName)
{
    if (!value.contains(fieldName) || !value[fieldName].is_string()
        || value[fieldName].get<std::string>().empty()) {
        throw std::runtime_error("Sticker manifest is missing a required string field.");
    }
}

std::string readStringOrFallback(const nlohmann::json& value,
                                 const char* fieldName,
                                 const std::string& fallback)
{
    if (!value.contains(fieldName) || !value[fieldName].is_string()) {
        return fallback;
    }

    const std::string result = value[fieldName].get<std::string>();
    return result.empty() ? fallback : result;
}

nlohmann::json stickerItemToJson(const StickerItem& item)
{
    return nlohmann::json{
        {"id", item.GetItemId()},
        {"name", item.GetDisplayName()},
        {"path", item.GetRelativePath()},
    };
}

StickerItem stickerItemFromJson(const nlohmann::json& value)
{
    requireStringField(value, "id");
    requireStringField(value, "name");
    requireStringField(value, "path");
    return StickerItem(value["id"].get<std::string>(),
                       value["name"].get<std::string>(),
                       value["path"].get<std::string>());
}

StickerPack stickerPackFromManifest(const nlohmann::json& value)
{
    if (!value.is_object()
        || !value.contains("schema_version")
        || !value["schema_version"].is_number_integer()
        || value["schema_version"].get<int>() != kStickerPackSchemaVersion) {
        throw std::runtime_error("Sticker manifest schema version is unsupported.");
    }
    requireStringField(value, "pack_id");
    requireStringField(value, "name");
    if (!value.contains("items") || !value["items"].is_array()) {
        throw std::runtime_error("Sticker manifest items field is invalid.");
    }

    std::vector<StickerItem> items;
    for (const auto& item : value["items"]) {
        items.push_back(stickerItemFromJson(item));
    }

    const std::string importFormat = readStringOrFallback(
        value,
        "import_format",
        toJsonValue(StickerPackImportFormat::RelayDeskManifest));
    return StickerPack(value["pack_id"].get<std::string>(),
                       value["name"].get<std::string>(),
                       importFormatFromJsonValue(importFormat),
                       std::move(items));
}

nlohmann::json stickerPackToManifest(const StickerPack& pack)
{
    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : pack.GetItems()) {
        items.push_back(stickerItemToJson(item));
    }

    return nlohmann::json{
        {"schema_version", kStickerPackSchemaVersion},
        {"pack_id", pack.GetPackId()},
        {"name", pack.GetDisplayName()},
        {"import_format", toJsonValue(pack.GetImportFormat())},
        {"items", items},
    };
}

StickerPack loadStickerPackManifest(const std::filesystem::path& manifestPath)
{
    return stickerPackFromManifest(readJsonFile(manifestPath));
}

void saveStickerPackManifest(const std::filesystem::path& packDirectory,
                             const StickerPack& pack)
{
    writeJsonFile(getPackManifestPath(packDirectory), stickerPackToManifest(pack));
}

StickerItem copyStickerImage(const AppPaths& appPaths,
                             const std::filesystem::path& sourceFile,
                             const std::filesystem::path& destinationDirectory,
                             const std::string& preferredName)
{
    if (!std::filesystem::is_regular_file(sourceFile) || !isSupportedStickerImage(sourceFile)) {
        throw std::runtime_error("Sticker image file is unsupported.");
    }

    const std::string displayName = preferredName.empty()
        ? pathToUtf8String(sourceFile.stem())
        : preferredName;
    const std::string itemId = makeSafeId(displayName, "sticker");
    const std::filesystem::path destination = makeUniqueDestination(
        destinationDirectory,
        itemId,
        lowercaseExtension(sourceFile));
    std::filesystem::copy_file(sourceFile,
                               destination,
                               std::filesystem::copy_options::overwrite_existing);

    return StickerItem(pathToUtf8String(destination.stem()),
                       displayName,
                       makeRelativeToWorkDirectory(appPaths, destination));
}

std::filesystem::path resolveSourceItemPath(const std::filesystem::path& sourceDirectory,
                                            const std::string& pathValue)
{
    const std::filesystem::path sourceFile =
        (sourceDirectory / std::filesystem::path(pathValue)).lexically_normal();
    if (!std::filesystem::exists(sourceFile)
        || !isPathInsideDirectory(sourceDirectory, sourceFile)) {
        throw std::runtime_error("Sticker source path is invalid.");
    }
    return sourceFile;
}

std::vector<StickerItem> importRelayDeskManifestItems(
    const AppPaths& appPaths,
    const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& destinationItemsDirectory,
    const nlohmann::json& manifest)
{
    if (!manifest.contains("items") || !manifest["items"].is_array()) {
        throw std::runtime_error("Sticker manifest items field is invalid.");
    }

    std::vector<StickerItem> items;
    for (const auto& item : manifest["items"]) {
        requireStringField(item, "path");
        const std::string name = readStringOrFallback(item, "name", "sticker");
        const std::filesystem::path sourceFile =
            resolveSourceItemPath(sourceDirectory, item["path"].get<std::string>());
        items.push_back(copyStickerImage(appPaths,
                                         sourceFile,
                                         destinationItemsDirectory,
                                         name));
    }
    return items;
}

bool isRemoteReference(const std::string& value)
{
    return value.rfind("http://", 0) == 0
        || value.rfind("https://", 0) == 0
        || value.rfind("data:", 0) == 0;
}

std::string trimAsciiWhitespace(std::string value)
{
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::string extractImageReference(std::string value)
{
    value = trimAsciiWhitespace(std::move(value));
    const std::string marker = "src=";
    const std::size_t markerPosition = value.find(marker);
    if (markerPosition == std::string::npos) {
        return value;
    }

    std::size_t valueStart = markerPosition + marker.size();
    while (valueStart < value.size()
           && std::isspace(static_cast<unsigned char>(value[valueStart])) != 0) {
        ++valueStart;
    }
    if (valueStart >= value.size()) {
        return {};
    }

    const char quote = value[valueStart];
    if (quote == '\'' || quote == '"') {
        const std::size_t valueEnd = value.find(quote, valueStart + 1u);
        if (valueEnd == std::string::npos) {
            return {};
        }
        return value.substr(valueStart + 1u, valueEnd - valueStart - 1u);
    }

    const std::size_t valueEnd = value.find_first_of(" \t\r\n>", valueStart);
    return valueEnd == std::string::npos
        ? value.substr(valueStart)
        : value.substr(valueStart, valueEnd - valueStart);
}

std::optional<std::string> readOwOItemIcon(const nlohmann::json& item)
{
    const std::array<const char*, 4> fields{"icon", "src", "path", "url"};
    for (const char* field : fields) {
        if (item.contains(field) && item[field].is_string()) {
            const std::string value = extractImageReference(item[field].get<std::string>());
            if (!value.empty() && !isRemoteReference(value)) {
                return value;
            }
        }
    }
    return std::nullopt;
}

void importOwOContainer(const AppPaths& appPaths,
                        const std::filesystem::path& sourceDirectory,
                        const std::filesystem::path& destinationItemsDirectory,
                        const nlohmann::json& container,
                        std::vector<StickerItem>& items)
{
    if (!container.is_array()) {
        return;
    }

    for (const auto& item : container) {
        if (!item.is_object()) {
            continue;
        }
        const std::optional<std::string> icon = readOwOItemIcon(item);
        if (!icon.has_value()) {
            continue;
        }

        const std::string name = readStringOrFallback(item, "text", "sticker");
        const std::filesystem::path sourceFile =
            resolveSourceItemPath(sourceDirectory, icon.value());
        items.push_back(copyStickerImage(appPaths,
                                         sourceFile,
                                         destinationItemsDirectory,
                                         name));
    }
}

std::vector<StickerItem> importOwOItems(
    const AppPaths& appPaths,
    const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& destinationItemsDirectory,
    const nlohmann::json& value)
{
    std::vector<StickerItem> items;
    if (!value.is_object()) {
        throw std::runtime_error("OwO sticker pack root must be an object.");
    }

    for (const auto& [categoryName, categoryValue] : value.items()) {
        static_cast<void>(categoryName);
        if (categoryValue.is_array()) {
            importOwOContainer(appPaths,
                               sourceDirectory,
                               destinationItemsDirectory,
                               categoryValue,
                               items);
        } else if (categoryValue.is_object()
                   && categoryValue.contains("container")) {
            importOwOContainer(appPaths,
                               sourceDirectory,
                               destinationItemsDirectory,
                               categoryValue["container"],
                               items);
        }
    }
    return items;
}

std::vector<std::filesystem::path> collectImageFiles(
    const std::filesystem::path& sourceDirectory)
{
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceDirectory)) {
        if (entry.is_regular_file() && isSupportedStickerImage(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<StickerItem> importImageFolderItems(
    const AppPaths& appPaths,
    const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& destinationItemsDirectory)
{
    std::vector<StickerItem> items;
    for (const auto& sourceFile : collectImageFiles(sourceDirectory)) {
        items.push_back(copyStickerImage(appPaths,
                                         sourceFile,
                                         destinationItemsDirectory,
                                         pathToUtf8String(sourceFile.stem())));
    }
    return items;
}

StickerPack makeImportedPack(const AppPaths& appPaths,
                             const std::filesystem::path& sourceDirectory,
                             const std::string& fallbackDisplayName)
{
    if (!std::filesystem::is_directory(sourceDirectory)) {
        throw std::runtime_error("Sticker import source must be a directory.");
    }

    const std::filesystem::path relayDeskManifest = sourceDirectory / "manifest.json";
    const std::filesystem::path owoManifest = sourceDirectory / "OwO.json";
    const std::string displayName = fallbackDisplayName.empty()
        ? pathToUtf8String(sourceDirectory.filename())
        : fallbackDisplayName;
    std::string packId = makeSafeId(displayName, "pack");
    StickerPackImportFormat importFormat = StickerPackImportFormat::ImageFolder;
    nlohmann::json sourceManifest;

    if (std::filesystem::exists(relayDeskManifest)) {
        sourceManifest = readJsonFile(relayDeskManifest);
        packId = makeSafeId(readStringOrFallback(sourceManifest, "pack_id", packId),
                            "pack");
        importFormat = StickerPackImportFormat::RelayDeskManifest;
    } else if (std::filesystem::exists(owoManifest)) {
        sourceManifest = readJsonFile(owoManifest);
        importFormat = StickerPackImportFormat::OwOJson;
    }

    const std::filesystem::path destinationDirectory =
        appPaths.GetStickerPacksDirectory() / packId;
    std::filesystem::remove_all(destinationDirectory);
    const std::filesystem::path destinationItemsDirectory =
        getPackItemsDirectory(destinationDirectory);

    std::vector<StickerItem> items;
    switch (importFormat) {
    case StickerPackImportFormat::RelayDeskManifest:
        items = importRelayDeskManifestItems(appPaths,
                                             sourceDirectory,
                                             destinationItemsDirectory,
                                             sourceManifest);
        break;
    case StickerPackImportFormat::OwOJson:
        items = importOwOItems(appPaths,
                               sourceDirectory,
                               destinationItemsDirectory,
                               sourceManifest);
        break;
    case StickerPackImportFormat::ImageFolder:
        items = importImageFolderItems(appPaths,
                                       sourceDirectory,
                                       destinationItemsDirectory);
        break;
    }

    if (items.empty()) {
        throw std::runtime_error("Sticker import source contains no usable images.");
    }

    const std::string packName = sourceManifest.is_object()
        ? readStringOrFallback(sourceManifest, "name", displayName)
        : displayName;
    return StickerPack(packId, packName, importFormat, std::move(items));
}

} // namespace

StickerItem::StickerItem(std::string itemId,
                         std::string displayName,
                         std::string relativePath)
    : itemId_(std::move(itemId)),
      displayName_(std::move(displayName)),
      relativePath_(std::move(relativePath))
{
}

StickerPack::StickerPack(std::string packId,
                         std::string displayName,
                         StickerPackImportFormat importFormat,
                         std::vector<StickerItem> items)
    : packId_(std::move(packId)),
      displayName_(std::move(displayName)),
      importFormat_(importFormat),
      items_(std::move(items))
{
}

StickerPack loadFavoriteStickerPack(const AppPaths& appPaths)
{
    const std::filesystem::path manifestPath =
        getPackManifestPath(appPaths.GetFavoriteStickersDirectory());
    if (!std::filesystem::exists(manifestPath)) {
        return StickerPack(kFavoritesPackId,
                           kFavoritesPackName,
                           StickerPackImportFormat::RelayDeskManifest,
                           {});
    }
    return loadStickerPackManifest(manifestPath);
}

std::vector<StickerPack> loadStickerPacks(const AppPaths& appPaths)
{
    std::vector<StickerPack> packs;
    if (!std::filesystem::exists(appPaths.GetStickerPacksDirectory())) {
        return packs;
    }

    std::vector<std::filesystem::path> manifestPaths;
    for (const auto& entry : std::filesystem::directory_iterator(
             appPaths.GetStickerPacksDirectory())) {
        if (entry.is_directory()) {
            const std::filesystem::path manifestPath =
                getPackManifestPath(entry.path());
            if (std::filesystem::exists(manifestPath)) {
                manifestPaths.push_back(manifestPath);
            }
        }
    }
    std::sort(manifestPaths.begin(), manifestPaths.end());

    for (const auto& manifestPath : manifestPaths) {
        packs.push_back(loadStickerPackManifest(manifestPath));
    }
    return packs;
}

StickerItem addFavoriteStickerFromImage(const AppPaths& appPaths,
                                        const std::filesystem::path& imagePath,
                                        const std::string& displayName)
{
    StickerPack favoritePack = loadFavoriteStickerPack(appPaths);
    std::vector<StickerItem> items = favoritePack.GetItems();
    StickerItem item = copyStickerImage(appPaths,
                                        imagePath,
                                        getPackItemsDirectory(
                                            appPaths.GetFavoriteStickersDirectory()),
                                        displayName);
    items.push_back(item);
    saveStickerPackManifest(
        appPaths.GetFavoriteStickersDirectory(),
        StickerPack(kFavoritesPackId,
                    kFavoritesPackName,
                    StickerPackImportFormat::RelayDeskManifest,
                    items));
    return item;
}

StickerPack importStickerPack(const AppPaths& appPaths,
                              const std::filesystem::path& sourceDirectory,
                              const std::string& fallbackDisplayName)
{
    StickerPack pack = makeImportedPack(appPaths, sourceDirectory, fallbackDisplayName);
    saveStickerPackManifest(appPaths.GetStickerPacksDirectory() / pack.GetPackId(), pack);
    return pack;
}

}
