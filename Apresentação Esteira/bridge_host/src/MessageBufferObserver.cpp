#include "MessageBufferObserver.hpp"

namespace serial {

void MessageBufferObserver::onDataReceived(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_messages.push_back(data);
}

void MessageBufferObserver::onError(const std::string& /*errorMessage*/) {
    // Optionally log or handle errors
}

void MessageBufferObserver::onConnectionClosed() {
    // Optionally handle disconnection
}

std::optional<std::vector<uint8_t>> MessageBufferObserver::popMessage() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_messages.empty()) {
        return std::nullopt;
    }
    auto msg = m_messages.front();
    m_messages.erase(m_messages.begin());
    return msg;
}

std::vector<std::vector<uint8_t>> MessageBufferObserver::getAllMessages() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::vector<uint8_t>> msgs = std::move(m_messages);
    m_messages.clear(); // Ensure it's empty after move
    return msgs;
}

void MessageBufferObserver::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_messages.clear();
}

} // namespace serial
