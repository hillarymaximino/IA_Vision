#pragma once

#include "SerialPort.hpp"
#include "ISerialObserver.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

namespace serial {

class AsyncSerialReceiver {
public:
    AsyncSerialReceiver(SerialPort& serialPort);
    ~AsyncSerialReceiver();

    void start();
    void stop();

    void addObserver(ISerialObserver* observer);
    void removeObserver(ISerialObserver* observer);

private:
    void receiveLoop();
    void notifyData(const std::vector<uint8_t>& data);
    void notifyError(const std::string& errorMsg);
    void notifyClosed();

    SerialPort& m_serialPort;
    std::thread m_workerThread;
    std::atomic<bool> m_isRunning;

    std::mutex m_observersMutex;
    std::vector<ISerialObserver*> m_observers;
};

} // namespace serial
