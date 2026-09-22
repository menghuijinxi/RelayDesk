#include "net/discovery_message.h"

#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int expect(bool condition, const char* message)
{
    if (!condition) {
        return fail(message);
    }
    return 0;
}

relaydesk::net::DiscoveryAnnouncement makeAnnouncement()
{
    relaydesk::net::DiscoveryAnnouncement announcement;
    announcement.SetDeviceId("device-id");
    announcement.SetHostName("DESKTOP-OFFICE-12");
    announcement.SetDisplayName("中文用户");
    announcement.SetTcpPort(39171);
    announcement.SetAppVersion(42);
    announcement.SetCapabilities({"text", "emoji", "file", "folder"});
    announcement.SetTimestamp("2026-06-12T10:00:00Z");
    return announcement;
}

int roundTripsAnnouncement()
{
    const auto parsed = relaydesk::net::parseDiscoveryAnnouncement(
        relaydesk::net::serializeDiscoveryAnnouncement(makeAnnouncement()));

    if (const int result = expect(parsed.GetVersion() == 1,
                                  "Discovery version did not round-trip.");
        result != 0) {
        return result;
    }

    if (const int result = expect(parsed.GetType() == "hello",
                                  "Discovery type did not round-trip.");
        result != 0) {
        return result;
    }

    if (const int result = expect(parsed.GetDeviceId() == "device-id",
                                  "Discovery device ID did not round-trip.");
        result != 0) {
        return result;
    }

    if (const int result = expect(parsed.GetDisplayName() == "中文用户",
                                  "Discovery display name did not round-trip.");
        result != 0) {
        return result;
    }

    if (const int result = expect(parsed.GetTcpPort() == 39171,
                                  "Discovery TCP port did not round-trip.");
        result != 0) {
        return result;
    }

    if (const int result = expect(parsed.GetAppVersion() == 42,
                                  "Discovery app version did not round-trip.");
        result != 0) {
        return result;
    }

    return expect(parsed.GetCapabilities().size() == 4,
                  "Discovery capabilities did not round-trip.");
}

int serializesExpectedJsonFields()
{
    const nlohmann::json value = nlohmann::json::parse(
        relaydesk::net::serializeDiscoveryAnnouncement(makeAnnouncement()));

    if (const int result = expect(value.value("protocol", "") == "relaydesk.discovery",
                                  "Discovery protocol field mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(value.value("version", 0) == 1,
                                  "Discovery version field mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(value.value("type", "") == "hello",
                                  "Discovery type field mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(value.value("app_version", 0) == 42,
                                  "Discovery app version field mismatch.");
        result != 0) {
        return result;
    }

    return expect(value.value("display_name", "") == "中文用户",
                  "Discovery display name field mismatch.");
}

int acceptsLegacyAnnouncementWithoutAppVersion()
{
    nlohmann::json value = nlohmann::json::parse(
        relaydesk::net::serializeDiscoveryAnnouncement(makeAnnouncement()));
    value.erase("app_version");

    const auto parsed = relaydesk::net::parseDiscoveryAnnouncement(value.dump());
    return expect(parsed.GetAppVersion() == 0,
                  "Legacy discovery app version should default to zero.");
}

int rejectsInvalidJson()
{
    try {
        static_cast<void>(relaydesk::net::parseDiscoveryAnnouncement("{not-json}"));
    } catch (const std::runtime_error&) {
        return 0;
    }

    return fail("Invalid discovery JSON was accepted.");
}

int rejectsMissingRequiredField()
{
    const nlohmann::json value{
        {"protocol", "relaydesk.discovery"},
        {"version", 1},
        {"type", "hello"},
        {"device_id", "device-id"},
        {"host_name", "DESKTOP-OFFICE-12"},
        {"tcp_port", 39171},
        {"capabilities", {"text"}},
        {"timestamp", "2026-06-12T10:00:00Z"},
    };

    try {
        static_cast<void>(relaydesk::net::parseDiscoveryAnnouncement(value.dump()));
    } catch (const std::runtime_error&) {
        return 0;
    }

    return fail("Discovery announcement with missing field was accepted.");
}


int roundTripsSpecifiedAvatarHash()
{
    auto announcement = makeAnnouncement();
    const std::string avatarHash(64, 'a');
    announcement.SetAvatarSha256(avatarHash);
    const auto parsed = relaydesk::net::parseDiscoveryAnnouncement(
        relaydesk::net::serializeDiscoveryAnnouncement(announcement));
    return expect(parsed.GetAvatarSha256Specified()
                      && parsed.GetAvatarSha256() == avatarHash,
                  "Specified avatar hash did not round-trip.");
}

int roundTripsExplicitEmptyAvatarHash()
{
    auto announcement = makeAnnouncement();
    announcement.SetAvatarSha256("");
    const nlohmann::json value = nlohmann::json::parse(
        relaydesk::net::serializeDiscoveryAnnouncement(announcement));
    if (const int result = expect(value.contains("avatar_sha256")
                                      && value["avatar_sha256"] == "",
                                  "Explicit empty avatar hash was omitted.");
        result != 0) {
        return result;
    }

    const auto parsed = relaydesk::net::parseDiscoveryAnnouncement(value.dump());
    return expect(parsed.GetAvatarSha256Specified()
                      && parsed.GetAvatarSha256().empty(),
                  "Explicit empty avatar hash did not round-trip.");
}

int leavesMissingAvatarHashUnspecified()
{
    const auto parsed = relaydesk::net::parseDiscoveryAnnouncement(
        relaydesk::net::serializeDiscoveryAnnouncement(makeAnnouncement()));
    return expect(!parsed.GetAvatarSha256Specified()
                      && parsed.GetAvatarSha256().empty(),
                  "Missing avatar hash should stay unspecified.");
}

int rejectsInvalidAvatarHash()
{
    nlohmann::json value = nlohmann::json::parse(
        relaydesk::net::serializeDiscoveryAnnouncement(makeAnnouncement()));
    value["avatar_sha256"] = std::string(64, 'g');
    try {
        static_cast<void>(
            relaydesk::net::parseDiscoveryAnnouncement(value.dump()));
    } catch (const std::runtime_error&) {
        value["avatar_sha256"] = 1;
        try {
            static_cast<void>(
                relaydesk::net::parseDiscoveryAnnouncement(value.dump()));
        } catch (const std::runtime_error&) {
            return 0;
        }
    }

    return fail("Invalid discovery avatar hash was accepted.");
}

int rejectsInvalidTcpPort()
{
    nlohmann::json value{
        {"protocol", "relaydesk.discovery"},
        {"version", 1},
        {"type", "hello"},
        {"device_id", "device-id"},
        {"host_name", "DESKTOP-OFFICE-12"},
        {"display_name", "Alice"},
        {"tcp_port", 70000},
        {"capabilities", {"text"}},
        {"timestamp", "2026-06-12T10:00:00Z"},
    };

    try {
        static_cast<void>(relaydesk::net::parseDiscoveryAnnouncement(value.dump()));
    } catch (const std::runtime_error&) {
        return 0;
    }

    return fail("Discovery announcement with invalid TCP port was accepted.");
}

} // namespace

int main()
{
    if (const int result = roundTripsAnnouncement(); result != 0) {
        return result;
    }

    if (const int result = serializesExpectedJsonFields(); result != 0) {
        return result;
    }

    if (const int result = acceptsLegacyAnnouncementWithoutAppVersion();
        result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidJson(); result != 0) {
        return result;
    }

    if (const int result = rejectsMissingRequiredField(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidTcpPort(); result != 0) {
        return result;
    }

    if (const int result = roundTripsSpecifiedAvatarHash(); result != 0) {
        return result;
    }

    if (const int result = roundTripsExplicitEmptyAvatarHash(); result != 0) {
        return result;
    }

    if (const int result = leavesMissingAvatarHashUnspecified(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidAvatarHash(); result != 0) {
        return result;
    }

    return 0;
}
