#include "platform/github_app_update.h"

#include <iostream>
#include <string>

namespace {

int expect(bool condition, const char* message)
{
    if (condition) {
        return 0;
    }

    std::cerr << message << '\n';
    return 1;
}

std::string makeDigest(char character)
{
    return std::string(64u, character);
}

int parsesGitHubReleaseAsset()
{
    const std::string digest = makeDigest('a');
    const std::string json =
        R"({"tag_name":"v62","assets":[)"
        R"({"name":"notes.txt","browser_download_url":"https://github.com/menghuijinxi/RelayDesk/releases/download/v62/notes.txt","size":32},)"
        R"({"name":"relaydesk_skiaui.exe","browser_download_url":"https://github.com/menghuijinxi/RelayDesk/releases/download/v62/relaydesk_skiaui.exe","size":9762816,"digest":"sha256:)"
        + digest
        + R"("}]})";

    const std::optional<relaydesk::platform::GitHubAppUpdateRelease> release =
        relaydesk::platform::parseGitHubAppUpdateReleaseJson(
            json,
            "relaydesk_skiaui.exe");
    if (const int result = expect(release.has_value(),
                                  "有效 GitHub 发布信息解析失败。");
        result != 0) {
        return result;
    }
    if (const int result = expect(release->GetTagName() == "v62",
                                  "GitHub 发布标签解析错误。");
        result != 0) {
        return result;
    }
    if (const int result = expect(release->GetAppVersion() == 62,
                                  "GitHub 发布版本号解析错误。");
        result != 0) {
        return result;
    }
    if (const int result = expect(release->GetAssetSize() == 9762816u,
                                  "GitHub 发布文件大小解析错误。");
        result != 0) {
        return result;
    }
    return expect(release->GetSha256() == digest,
                  "GitHub 发布 SHA-256 解析错误。");
}

int rejectsInvalidReleaseAsset()
{
    const std::string invalidHost =
        R"({"tag_name":"v62","assets":[{"name":"relaydesk_skiaui.exe","browser_download_url":"https://example.com/relaydesk_skiaui.exe","size":1}]})";
    if (const int result =
            expect(!relaydesk::platform::parseGitHubAppUpdateReleaseJson(
                        invalidHost,
                        "relaydesk_skiaui.exe")
                         .has_value(),
                    "未拒绝不可信的 GitHub 下载地址。");
        result != 0) {
        return result;
    }

    const std::string invalidTag =
        R"({"tag_name":"release-62","assets":[{"name":"relaydesk_skiaui.exe","browser_download_url":"https://github.com/menghuijinxi/RelayDesk/releases/download/v62/relaydesk_skiaui.exe","size":1}]})";
    if (const int result =
            expect(!relaydesk::platform::parseGitHubAppUpdateReleaseJson(
                        invalidTag,
                        "relaydesk_skiaui.exe")
                         .has_value(),
                    "未拒绝不符合约定的版本标签。");
        result != 0) {
        return result;
    }

    const std::string invalidDigest =
        R"({"tag_name":"v62","assets":[{"name":"relaydesk_skiaui.exe","browser_download_url":"https://github.com/menghuijinxi/RelayDesk/releases/download/v62/relaydesk_skiaui.exe","size":1,"digest":"sha256:bad"}]})";
    return expect(!relaydesk::platform::parseGitHubAppUpdateReleaseJson(
                       invalidDigest,
                       "relaydesk_skiaui.exe")
                       .has_value(),
                  "未拒绝无效的 SHA-256 摘要。");
}

}

int main()
{
    if (const int result = parsesGitHubReleaseAsset(); result != 0) {
        return result;
    }
    return rejectsInvalidReleaseAsset();
}
