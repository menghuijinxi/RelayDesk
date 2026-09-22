#include "net/discovery_local_announcement.h"

#include "core/app_version.h"
#include "net/discovery_message.h"
#include "storage/local_identity.h"

#include <iostream>
#include <stdexcept>
#include <string>

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

relaydesk::storage::LocalIdentity makeIdentity()
{
    return relaydesk::storage::LocalIdentity("local-device",
                                             "install-id",
                                             "2026-06-12T00:00:00Z",
                                             "HOST-A",
                                             "Alice-PC");
}

int createsAnnouncementFromLocalIdentity()
{
    const auto announcement = relaydesk::net::makeLocalDiscoveryAnnouncement(
        makeIdentity(),
        39171,
        {"text", "file"},
        "2026-06-12T12:00:00Z");

    if (const int result = expect(announcement.GetDeviceId() == "local-device",
                                  "Announcement device ID mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(announcement.GetHostName() == "HOST-A",
                                  "Announcement host name mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(announcement.GetDisplayName() == "Alice-PC",
                                  "Announcement display name mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(announcement.GetTcpPort() == 39171,
                                  "Announcement TCP port mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(
            announcement.GetAppVersion() == relaydesk::core::kAppVersion,
            "Announcement app version mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(announcement.GetCapabilities().size() == 2,
                                  "Announcement capability count mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(announcement.GetAvatarSha256Specified(),
                                  "Local announcement should specify avatar hash.");
        result != 0) {
        return result;
    }
    if (const int result = expect(announcement.GetAvatarSha256().empty(),
                                  "Missing local avatar should broadcast empty.");
        result != 0) {
        return result;
    }

    const std::string payload =
        relaydesk::net::serializeDiscoveryAnnouncement(announcement);
    const relaydesk::net::DiscoveryAnnouncement parsed =
        relaydesk::net::parseDiscoveryAnnouncement(payload);
    if (const int result = expect(parsed.GetTimestamp() == "2026-06-12T12:00:00Z",
                                  "Serialized announcement timestamp mismatch.");
        result != 0) {
        return result;
    }
    return expect(parsed.GetAvatarSha256Specified()
                      && parsed.GetAvatarSha256().empty(),
                  "Serialized announcement omitted the empty avatar hash.");
}

int carriesAvatarHashFromLocalIdentity()
{
    auto identity = makeIdentity();
    const std::string avatarHash(64, 'c');
    identity.SetAvatarSha256(avatarHash);
    const auto announcement = relaydesk::net::makeLocalDiscoveryAnnouncement(
        identity,
        39171,
        {"text"},
        "2026-06-12T12:00:00Z");
    if (const int result = expect(announcement.GetAvatarSha256Specified()
                                      && announcement.GetAvatarSha256()
                                          == avatarHash,
                                  "Local announcement dropped the avatar hash.");
        result != 0) {
        return result;
    }

    const auto parsed = relaydesk::net::parseDiscoveryAnnouncement(
        relaydesk::net::serializeDiscoveryAnnouncement(announcement));
    return expect(parsed.GetAvatarSha256Specified()
                      && parsed.GetAvatarSha256() == avatarHash,
                  "Serialized local avatar hash did not round-trip.");
}

int rejectsIdentityWithMissingDiscoveryField()
{
    const relaydesk::storage::LocalIdentity identity("local-device",
                                                     "install-id",
                                                     "2026-06-12T00:00:00Z",
                                                     "",
                                                     "Alice-PC");
    try {
        static_cast<void>(relaydesk::net::makeLocalDiscoveryAnnouncement(
            identity,
            39171,
            {"text"},
            "2026-06-12T12:00:00Z"));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Identity with missing discovery field was accepted.");
}

int rejectsZeroTcpPort()
{
    try {
        static_cast<void>(relaydesk::net::makeLocalDiscoveryAnnouncement(
            makeIdentity(),
            0,
            {"text"},
            "2026-06-12T12:00:00Z"));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Zero discovery TCP port was accepted.");
}

int rejectsEmptyCapability()
{
    try {
        static_cast<void>(relaydesk::net::makeLocalDiscoveryAnnouncement(
            makeIdentity(),
            39171,
            {"text", ""},
            "2026-06-12T12:00:00Z"));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Empty discovery capability was accepted.");
}

int rejectsEmptyTimestamp()
{
    try {
        static_cast<void>(relaydesk::net::makeLocalDiscoveryAnnouncement(
            makeIdentity(),
            39171,
            {"text"},
            ""));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Empty discovery timestamp was accepted.");
}

} // namespace

int main()
{
    if (const int result = createsAnnouncementFromLocalIdentity(); result != 0) {
        return result;
    }

    if (const int result = carriesAvatarHashFromLocalIdentity(); result != 0) {
        return result;
    }

    if (const int result = rejectsIdentityWithMissingDiscoveryField(); result != 0) {
        return result;
    }

    if (const int result = rejectsZeroTcpPort(); result != 0) {
        return result;
    }

    if (const int result = rejectsEmptyCapability(); result != 0) {
        return result;
    }

    if (const int result = rejectsEmptyTimestamp(); result != 0) {
        return result;
    }

    return 0;
}
