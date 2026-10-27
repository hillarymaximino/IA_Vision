#pragma once

#include "SerialTypes.hpp"
#include "PortEnumerator.hpp"
#include "SerialPort.hpp"
#include "AsyncSerialReceiver.hpp"
#include "MessageBufferObserver.hpp"
#include <memory>
#include <vector>
#include <string>

namespace serial {

class SerialClient {
public:
    SerialClient();
    ~SerialClient();

    std::vector<DeviceInfo> getAvailablePorts();
    
    bool connect(const DeviceInfo& device, const SerialConfig& config = {});
    bool connect(const std::string& portPath, uint32_t baudRate = 115200);
    
    void disconnect();
    
    bool send(const std::vector<uint8_t>& data);
    bool send(const std::string& text);
    
    void addObserver(ISerialObserver* observer);
    std::vector<std::vector<uint8_t>> readBufferedMessages();

private:
    SerialPort m_port;
    std::unique_ptr<AsyncSerialReceiver> m_receiver;
    std::shared_ptr<MessageBufferObserver> m_internalBuffer;
};

} // namespace serial
