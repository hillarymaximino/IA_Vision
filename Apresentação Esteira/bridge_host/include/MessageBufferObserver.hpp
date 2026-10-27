#pragma once

#include "ISerialObserver.hpp"
#include <vector>
#include <mutex>
#include <optional>

namespace serial {

class MessageBufferObserver : public ISerialObserver {
public:
    void onDataReceived(const std::vector<uint8_t>& data) override;
    void onError(const std::string& errorMessage) override;
    void onConnectionClosed() override;

    std::optional<std::vector<uint8_t>> popMessage();
    std::vector<std::vector<uint8_t>> getAllMessages();
    void clear();

private:
    std::mutex m_mutex;
    std::vector<std::vector<uint8_t>> m_messages;
};

} // namespace serial
