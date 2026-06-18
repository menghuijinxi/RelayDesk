#include "net/peer_frame.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

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

int expectThrows(const std::function<void()>& action, const char* message)
{
    try {
        action();
    } catch (const std::exception&) {
        return 0;
    }

    return fail(message);
}

relaydesk::net::PeerFrame makeSampleFrame()
{
    relaydesk::net::PeerFrame frame(
        relaydesk::net::PeerFrameType::ChatMessage,
        R"({"type":"chat_message"})",
        std::vector<std::uint8_t>{0x01, 0x02, 0x03});
    frame.SetFlags(0x01020304u);
    return frame;
}

int encodesWithNetworkByteOrderAndRoundTrips()
{
    const auto bytes = relaydesk::net::encodePeerFrame(makeSampleFrame());
    if (const int check = expect(bytes.size()
                                     == relaydesk::net::kPeerFrameHeaderSize + 26,
                                 "Encoded peer frame size mismatch.");
        check != 0) {
        return check;
    }

    if (const int check = expect(bytes[0] == 'R' && bytes[1] == 'L'
                                     && bytes[2] == 'D' && bytes[3] == 'K',
                                 "Encoded peer frame magic mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(bytes[4] == 0x00 && bytes[5] == 0x01,
                                 "Encoded peer frame version is not big-endian.");
        check != 0) {
        return check;
    }
    if (const int check = expect(bytes[6] == 0x00 && bytes[7] == 0x03,
                                 "Encoded peer frame type is not big-endian.");
        check != 0) {
        return check;
    }
    if (const int check = expect(bytes[8] == 0x01 && bytes[9] == 0x02
                                     && bytes[10] == 0x03 && bytes[11] == 0x04,
                                 "Encoded peer frame flags are not big-endian.");
        check != 0) {
        return check;
    }
    if (const int check = expect(bytes[15] == 23 && bytes[23] == 3,
                                 "Encoded peer frame lengths mismatch.");
        check != 0) {
        return check;
    }

    const relaydesk::net::PeerFrame decoded =
        relaydesk::net::decodePeerFrame(bytes);
    if (const int check = expect(decoded.GetType()
                                     == relaydesk::net::PeerFrameType::ChatMessage,
                                 "Decoded peer frame type mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetFlags() == 0x01020304u,
                                 "Decoded peer frame flags mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(decoded.GetHeader() == R"({"type":"chat_message"})",
                                 "Decoded peer frame header mismatch.");
        check != 0) {
        return check;
    }

    return expect(decoded.GetBody() == std::vector<std::uint8_t>({0x01, 0x02, 0x03}),
                  "Decoded peer frame body mismatch.");
}

int rejectsInvalidMagic()
{
    auto bytes = relaydesk::net::encodePeerFrame(makeSampleFrame());
    bytes[0] = 'X';
    return expectThrows(
        [&] {
            (void)relaydesk::net::decodePeerFrame(bytes);
        },
        "Invalid peer frame magic was accepted.");
}

int rejectsUnsupportedVersion()
{
    auto bytes = relaydesk::net::encodePeerFrame(makeSampleFrame());
    bytes[5] = 0x02;
    return expectThrows(
        [&] {
            (void)relaydesk::net::decodePeerFrame(bytes);
        },
        "Unsupported peer frame version was accepted.");
}

int rejectsUnsupportedFrameType()
{
    auto bytes = relaydesk::net::encodePeerFrame(makeSampleFrame());
    bytes[6] = 0x7F;
    bytes[7] = 0x7F;
    return expectThrows(
        [&] {
            (void)relaydesk::net::decodePeerFrame(bytes);
        },
        "Unsupported peer frame type was accepted.");
}

int rejectsPayloadLengthMismatch()
{
    auto bytes = relaydesk::net::encodePeerFrame(makeSampleFrame());
    bytes.pop_back();
    return expectThrows(
        [&] {
            (void)relaydesk::net::decodePeerFrame(bytes);
        },
        "Truncated peer frame payload was accepted.");
}

int roundTripsAppUpdateFrameTypes()
{
    if (const int check = expect(
            relaydesk::net::peerFrameTypeFromWireValue(13)
                == relaydesk::net::PeerFrameType::AppUpdateRequest,
            "App update request frame type value mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(
            relaydesk::net::peerFrameTypeFromWireValue(14)
                == relaydesk::net::PeerFrameType::AppUpdateChunk,
            "App update chunk frame type value mismatch.");
        check != 0) {
        return check;
    }
    if (const int check = expect(
            relaydesk::net::peerFrameTypeFromWireValue(15)
                == relaydesk::net::PeerFrameType::AppUpdateComplete,
            "App update complete frame type value mismatch.");
        check != 0) {
        return check;
    }

    return expect(relaydesk::net::toWireValue(
                      relaydesk::net::PeerFrameType::AppUpdateComplete) == 15,
                  "App update complete wire value mismatch.");
}

} // namespace

int main()
{
    if (const int result = encodesWithNetworkByteOrderAndRoundTrips();
        result != 0) {
        return result;
    }
    if (const int result = rejectsInvalidMagic(); result != 0) {
        return result;
    }
    if (const int result = rejectsUnsupportedVersion(); result != 0) {
        return result;
    }
    if (const int result = rejectsUnsupportedFrameType(); result != 0) {
        return result;
    }
    if (const int result = rejectsPayloadLengthMismatch(); result != 0) {
        return result;
    }
    if (const int result = roundTripsAppUpdateFrameTypes(); result != 0) {
        return result;
    }

    return 0;
}
