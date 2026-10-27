#include "SerialClient.hpp"

namespace serial {

SerialClient::SerialClient() {
    m_internalBuffer = std::make_shared<MessageBufferObserver>();
}

SerialClient::~SerialClient() {
    disconnect();
}

std::vector<DeviceInfo> SerialClient::getAvailablePorts() {
    return PortEnumerator::listPorts();
}

bool SerialClient::connect(const DeviceInfo& device, const SerialConfig& config) {
    if (m_port.isOpen()) {
        disconnect();
    }

    if (!m_port.open(device.portPath, config)) {
        return false;
    }

    m_receiver = std::make_unique<AsyncSerialReceiver>(m_port);
    m_receiver->addObserver(m_internalBuffer.get());
    m_receiver->start();

    return true;
}

bool SerialClient::connect(const std::string& portPath, uint32_t baudRate) {
    SerialConfig config;
    config.baudRate = baudRate;
    
    DeviceInfo info;
    info.portPath = portPath;
    
    return connect(info, config);
}

void SerialClient::disconnect() {
    if (m_receiver) {
        m_receiver->stop();
        m_receiver.reset();
    }
    m_port.close();
    m_internalBuffer->clear();
}

bool SerialClient::send(const std::vector<uint8_t>& data) {
    return m_port.write(data) >= 0;
}

bool SerialClient::send(const std::string& text) {
    std::vector<uint8_t> data(text.begin(), text.end());
    return send(data);
}

void SerialClient::addObserver(ISerialObserver* observer) {
    if (m_receiver) {
        m_receiver->addObserver(observer);
    }
}

std::vector<std::vector<uint8_t>> SerialClient::readBufferedMessages() {
    return m_internalBuffer->getAllMessages();
}

} // namespace serial
