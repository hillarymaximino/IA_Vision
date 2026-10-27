#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace serial {

class ISerialObserver {
public:
    virtual ~ISerialObserver() = default;

    virtual void onDataReceived(const std::vector<uint8_t>& data) = 0;
    virtual void onError(const std::string& errorMessage) = 0;
    virtual void onConnectionClosed() = 0;
};

} // namespace serial
