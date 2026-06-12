#include "net/boost_asio_udp_discovery_transport.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;

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

int sendsAndReceivesLoopbackPacket()
{
    relaydesk::net::BoostAsioUdpDiscoveryTransport receiver(0);
    relaydesk::net::BoostAsioUdpDiscoveryTransport sender(0);

    sender.sendTo("relaydesk.discovery.test", "127.0.0.1", receiver.GetLocalPort());
    const auto packet = receiver.tryReceiveFor(500ms);

    if (const int result = expect(packet.has_value(),
                                  "Loopback discovery packet was not received.");
        result != 0) {
        return result;
    }

    if (const int result = expect(packet->GetPayload() == "relaydesk.discovery.test",
                                  "Loopback discovery payload mismatch.");
        result != 0) {
        return result;
    }

    if (const int result = expect(packet->GetObservedAddress() == "127.0.0.1",
                                  "Loopback observed address mismatch.");
        result != 0) {
        return result;
    }

    return expect(packet->GetObservedPort() == sender.GetLocalPort(),
                  "Loopback observed source port mismatch.");
}

int returnsEmptyWhenNoPacketArrives()
{
    relaydesk::net::BoostAsioUdpDiscoveryTransport receiver(0);
    const auto packet = receiver.tryReceiveFor(20ms);
    return expect(!packet.has_value(), "Empty discovery receive should time out.");
}

int sendsBroadcastWithoutThrowing()
{
    relaydesk::net::BoostAsioUdpDiscoveryTransport receiver(0);
    relaydesk::net::BoostAsioUdpDiscoveryTransport sender(0);

    sender.sendBroadcast("relaydesk.discovery.broadcast.test",
                         receiver.GetLocalPort());
    return 0;
}

int rejectsInvalidSendInputs()
{
    relaydesk::net::BoostAsioUdpDiscoveryTransport sender(0);

    try {
        sender.sendTo("", "127.0.0.1", 39171);
    } catch (const std::invalid_argument&) {
        try {
            sender.sendTo("payload", "127.0.0.1", 0);
        } catch (const std::invalid_argument&) {
            try {
                sender.sendTo("payload", "not-an-address", 39171);
            } catch (const std::invalid_argument&) {
                return 0;
            }
        }
    }

    return fail("Invalid discovery UDP send input was accepted.");
}

int rejectsInvalidReceiveTimeout()
{
    relaydesk::net::BoostAsioUdpDiscoveryTransport receiver(0);

    try {
        static_cast<void>(receiver.tryReceiveFor(-1ms));
    } catch (const std::invalid_argument&) {
        return 0;
    }

    return fail("Negative discovery UDP receive timeout was accepted.");
}

} // namespace

int main()
{
    if (const int result = sendsAndReceivesLoopbackPacket(); result != 0) {
        return result;
    }

    if (const int result = returnsEmptyWhenNoPacketArrives(); result != 0) {
        return result;
    }

    if (const int result = sendsBroadcastWithoutThrowing(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidSendInputs(); result != 0) {
        return result;
    }

    if (const int result = rejectsInvalidReceiveTimeout(); result != 0) {
        return result;
    }

    return 0;
}
