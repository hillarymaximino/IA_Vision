#pragma once

#include <string>
#include <cstdint>

namespace serial {

enum class ParityMode { NONE, EVEN, ODD };

struct DeviceInfo {
    std::string portPath;
    std::string vendorId;
    std::string productId;
    std::string description;
};

struct SerialConfig {
    uint32_t baudRate = 115200;
    uint8_t dataBits = 8;
    uint8_t stopBits = 1;
    ParityMode parity = ParityMode::NONE;
};

} // namespace serial
