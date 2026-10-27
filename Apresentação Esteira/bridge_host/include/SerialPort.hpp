#pragma once

#include "SerialTypes.hpp"
#include <cstddef>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#endif

namespace serial {

#ifdef _WIN32
/** MSVC nao define ssize_t; o tipo e equivalente ao do POSIX (com sinal, tamanho de ponteiro). */
using ssize_t = std::ptrdiff_t;
#endif

/**
 * @brief Porta serial em modo Raw (115200 8N1 por padrao).
 *
 * Backends:
 *  - POSIX  : termios, leitura nao bloqueante (o receptor usa poll()).
 *  - Windows: Win32 COM com I/O overlapped. read() bloqueia no maximo
 *             kReadTimeoutMs e retorna 0 quando nao chega nada (timeout, nao EOF).
 *
 * Caminhos aceitos: "/dev/ttyACM0" no Linux; "COM5" ou "\\\\.\\COM12" no Windows.
 */
class SerialPort {
public:
    /** Tempo maximo de bloqueio de read() no Windows, em milissegundos. */
    static constexpr unsigned kReadTimeoutMs = 100;

    SerialPort();
    ~SerialPort();

    // Disable copy
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool open(const std::string& path, const SerialConfig& config);
    void close();
    ssize_t write(const std::vector<uint8_t>& data);
    ssize_t read(uint8_t* buffer, size_t maxLen);

#ifndef _WIN32
    int getFileDescriptor() const;
#endif
    bool isOpen() const;

private:
    bool configure(const SerialConfig& config);

#ifdef _WIN32
    void* m_handle; // HANDLE, mantido como void* para nao expor <windows.h> no cabecalho
#else
    int m_fd;
    int getBaudRateCode(uint32_t baudRate);
#endif
};

} // namespace serial
