#include "core/uuid.h"

#include <array>
#include <iomanip>
#include <random>
#include <sstream>

namespace relaydesk::core {

std::string createUuidV4()
{
    std::array<unsigned char, 16> bytes{};
    std::random_device randomDevice;
    std::uniform_int_distribution<int> byteDistribution(0, 255);
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(byteDistribution(randomDevice));
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

}
