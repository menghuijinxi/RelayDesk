#include "storage/sticker_store.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int expect(bool condition, const char* message)
{
    return condition ? 0 : fail(message);
}

std::filesystem::path testRoot()
{
    return std::filesystem::path(RELAYDESK_STICKER_TEST_WORK_DIR);
}

relaydesk::storage::AppPaths makeAppPaths(const std::filesystem::path& caseName)
{
    const std::filesystem::path workDirectory = testRoot() / caseName;
    std::filesystem::remove_all(workDirectory);
    std::filesystem::create_directories(workDirectory);
    return relaydesk::storage::AppPaths(workDirectory / "relaydesk.exe");
}

void writeBytes(const std::filesystem::path& filePath,
                const std::vector<unsigned char>& bytes)
{
    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void writeJson(const std::filesystem::path& filePath, const nlohmann::json& value)
{
    std::filesystem::create_directories(filePath.parent_path());
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    output << value.dump(4) << '\n';
}

std::filesystem::path resolveStoredPath(
    const relaydesk::storage::AppPaths& appPaths,
    const relaydesk::storage::StickerItem& item)
{
    return appPaths.GetWorkDirectory() / std::filesystem::path(item.GetRelativePath());
}

int addsFavoriteStickerFromImage()
{
    const auto appPaths = makeAppPaths("favorite");
    const std::filesystem::path sourceImage =
        appPaths.GetWorkDirectory() / "source" / "cat.png";
    writeBytes(sourceImage, {0x89, 0x50, 0x4e, 0x47});

    const auto item = relaydesk::storage::addFavoriteStickerFromImage(
        appPaths,
        sourceImage,
        "Cat");

    if (const int check = expect(item.GetDisplayName() == "Cat",
                                 "Favorite sticker display name mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(std::filesystem::exists(resolveStoredPath(appPaths, item)),
                                 "Favorite sticker image was not copied.");
        check != 0) {
        return check;
    }

    const auto pack = relaydesk::storage::loadFavoriteStickerPack(appPaths);
    if (const int check = expect(pack.GetItems().size() == 1u,
                                 "Favorite sticker manifest did not persist item.");
        check != 0) {
        return check;
    }
    return expect(pack.GetItems()[0].GetRelativePath().find("data/stickers/favorites/items/")
                      == 0,
                  "Favorite sticker path is not stored under favorites.");
}

int importsRelayDeskManifestPack()
{
    const auto appPaths = makeAppPaths("relaydesk-manifest");
    const std::filesystem::path sourceDirectory =
        appPaths.GetWorkDirectory() / "source-pack";
    writeBytes(sourceDirectory / "items" / "a.png", {1, 2, 3, 4});
    writeJson(sourceDirectory / "manifest.json",
              nlohmann::json{
                  {"schema_version", 1},
                  {"pack_id", "classic"},
                  {"name", "Classic"},
                  {"items",
                   nlohmann::json::array({
                       {{"id", "a"}, {"name", "A"}, {"path", "items/a.png"}},
                   })},
              });

    const auto pack = relaydesk::storage::importStickerPack(
        appPaths,
        sourceDirectory,
        "");

    if (const int check = expect(pack.GetPackId() == "classic",
                                 "RelayDesk manifest pack id mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(pack.GetImportFormat()
                                     == relaydesk::storage::StickerPackImportFormat::
                                            RelayDeskManifest,
                                 "RelayDesk manifest import format mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(std::filesystem::exists(
                                     resolveStoredPath(appPaths, pack.GetItems()[0])),
                                 "RelayDesk manifest image was not copied.");
        check != 0) {
        return check;
    }

    const auto loadedPacks = relaydesk::storage::loadStickerPacks(appPaths);
    return expect(loadedPacks.size() == 1u && loadedPacks[0].GetPackId() == "classic",
                  "Imported RelayDesk manifest pack did not persist.");
}

int importsOwOJsonPack()
{
    const auto appPaths = makeAppPaths("owo-json");
    const std::filesystem::path sourceDirectory =
        appPaths.GetWorkDirectory() / "owo-pack";
    writeBytes(sourceDirectory / "aru" / "1.png", {5, 6, 7, 8});
    writeJson(sourceDirectory / "OwO.json",
              nlohmann::json{
                  {"aru",
                   {
                       {"type", "image"},
                       {"container",
                        nlohmann::json::array({
                            {{"icon", "<img src=\"aru/1.png\">"}, {"text", "Aru"}},
                        })},
                   }},
              });

    const auto pack = relaydesk::storage::importStickerPack(
        appPaths,
        sourceDirectory,
        "OwO Pack");

    if (const int check = expect(pack.GetImportFormat()
                                     == relaydesk::storage::StickerPackImportFormat::
                                            OwOJson,
                                 "OwO import format mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(pack.GetItems().size() == 1u,
                                 "OwO pack imported wrong item count.");
        check != 0) {
        return check;
    }
    if (const int check = expect(pack.GetItems()[0].GetDisplayName() == "Aru",
                                 "OwO item display name mismatch.");
        check != 0) {
        return check;
    }
    return expect(std::filesystem::exists(resolveStoredPath(appPaths, pack.GetItems()[0])),
                  "OwO image was not copied.");
}

int importsImageFolderPack()
{
    const auto appPaths = makeAppPaths("image-folder");
    const std::filesystem::path sourceDirectory =
        appPaths.GetWorkDirectory() / "folder-pack";
    writeBytes(sourceDirectory / "a.png", {1});
    writeBytes(sourceDirectory / "nested" / "b.webp", {2});
    writeBytes(sourceDirectory / "ignored.txt", {3});

    const auto pack = relaydesk::storage::importStickerPack(
        appPaths,
        sourceDirectory,
        "Folder Pack");

    if (const int check = expect(pack.GetImportFormat()
                                     == relaydesk::storage::StickerPackImportFormat::
                                            ImageFolder,
                                 "Image folder import format mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(pack.GetItems().size() == 2u,
                                 "Image folder imported wrong item count.");
        check != 0) {
        return check;
    }

    return expect(std::filesystem::exists(resolveStoredPath(appPaths, pack.GetItems()[0]))
                      && std::filesystem::exists(resolveStoredPath(appPaths, pack.GetItems()[1])),
                  "Image folder pack images were not copied.");
}

}

int main()
{
    if (const int result = addsFavoriteStickerFromImage(); result != 0) {
        return result;
    }
    if (const int result = importsRelayDeskManifestPack(); result != 0) {
        return result;
    }
    if (const int result = importsOwOJsonPack(); result != 0) {
        return result;
    }
    if (const int result = importsImageFolderPack(); result != 0) {
        return result;
    }
    return 0;
}
