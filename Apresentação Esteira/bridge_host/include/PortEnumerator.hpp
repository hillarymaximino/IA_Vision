#pragma once

#include "SerialTypes.hpp"
#include <vector>
#include <optional>
#include <string>

namespace serial {

class PortEnumerator {
public:
    static std::vector<DeviceInfo> listPorts();
    static std::optional<DeviceInfo> findByVidPid(const std::string& vid, const std::string& pid);
};

} // namespace serial
