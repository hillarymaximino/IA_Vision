#include "SerialPort.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#endif

namespace serial {

#ifdef _WIN32

// ==============================================================================
// Backend Windows (Win32 COM, I/O overlapped)
// ==============================================================================

namespace {

/** "COM12" precisa do prefixo "\\.\" para abrir; "COM1".."COM9" tambem aceitam. */
std::string toDevicePath(const std::string& path) {
    if (path.rfind("\\\\.\\", 0) == 0) {
        return path;
    }
    return "\\\\.\\" + path;
}

/**
 * Espera a conclusao de uma operacao overlapped iniciada por ReadFile/WriteFile.
 * Retorna o numero de bytes transferidos ou -1 em erro.
 */
ssize_t finishOverlapped(HANDLE handle, BOOL started, OVERLAPPED& ov, DWORD transferred) {
    if (!started) {
        if (GetLastError() != ERROR_IO_PENDING) {
            return -1;
        }
        if (!GetOverlappedResult(handle, &ov, &transferred, TRUE)) {
            return -1;
        }
    }
    return static_cast<ssize_t>(transferred);
}

} // namespace

SerialPort::SerialPort() : m_handle(INVALID_HANDLE_VALUE) {}

SerialPort::~SerialPort() {
    close();
}

bool SerialPort::open(const std::string& path, const SerialConfig& config) {
    if (isOpen()) {
        close();
    }

    HANDLE handle = CreateFileA(toDevicePath(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    m_handle = handle;

    if (!configure(config)) {
        close();
        return false;
    }

    return true;
}

void SerialPort::close() {
    if (isOpen()) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = INVALID_HANDLE_VALUE;
    }
}

ssize_t SerialPort::write(const std::vector<uint8_t>& data) {
    if (!isOpen() || data.empty()) {
        return -1;
    }

    // Evento local por chamada: o ACK sai da thread RX e os comandos da thread
    // principal, entao duas escritas podem estar em andamento ao mesmo tempo.
    OVERLAPPED ov{};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        return -1;
    }

    HANDLE handle = static_cast<HANDLE>(m_handle);
    DWORD written = 0;
    BOOL started = WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, &ov);
    ssize_t result = finishOverlapped(handle, started, ov, written);

    CloseHandle(ov.hEvent);
    return result;
}

ssize_t SerialPort::read(uint8_t* buffer, size_t maxLen) {
    if (!isOpen() || buffer == nullptr || maxLen == 0) {
        return -1;
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        return -1;
    }

    HANDLE handle = static_cast<HANDLE>(m_handle);
    DWORD received = 0;
    BOOL started = ReadFile(handle, buffer, static_cast<DWORD>(maxLen), &received, &ov);
    ssize_t result = finishOverlapped(handle, started, ov, received);

    CloseHandle(ov.hEvent);
    return result;
}

bool SerialPort::isOpen() const {
    return m_handle != INVALID_HANDLE_VALUE;
}

bool SerialPort::configure(const SerialConfig& config) {
    HANDLE handle = static_cast<HANDLE>(m_handle);

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) {
        return false;
    }

    dcb.BaudRate = config.baudRate;
    dcb.ByteSize = config.dataBits;
    dcb.StopBits = (config.stopBits == 2) ? TWOSTOPBITS : ONESTOPBIT;

    switch (config.parity) {
        case ParityMode::EVEN: dcb.Parity = EVENPARITY; dcb.fParity = TRUE; break;
        case ParityMode::ODD:  dcb.Parity = ODDPARITY;  dcb.fParity = TRUE; break;
        case ParityMode::NONE:
        default:               dcb.Parity = NOPARITY;   dcb.fParity = FALSE; break;
    }

    // Modo Raw: binario, sem controle de fluxo por hardware ou software
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(handle, &dcb)) {
        return false;
    }

    // ReadFile retorna assim que chegar ao menos 1 byte, ou com 0 bytes apos
    // kReadTimeoutMs. Equivale ao poll() de 100 ms do backend POSIX.
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = kReadTimeoutMs;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 500;
    if (!SetCommTimeouts(handle, &timeouts)) {
        return false;
    }

    SetupComm(handle, 4096, 4096);
    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

#else

// ==============================================================================
// Backend POSIX (termios)
// ==============================================================================

SerialPort::SerialPort() : m_fd(-1) {}

SerialPort::~SerialPort() {
    close();
}

bool SerialPort::open(const std::string& path, const SerialConfig& config) {
    if (isOpen()) {
        close();
    }

    m_fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0) {
        return false;
    }

    if (!configure(config)) {
        close();
        return false;
    }

    return true;
}

void SerialPort::close() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

ssize_t SerialPort::write(const std::vector<uint8_t>& data) {
    if (!isOpen() || data.empty()) {
        return -1;
    }
    return ::write(m_fd, data.data(), data.size());
}

ssize_t SerialPort::read(uint8_t* buffer, size_t maxLen) {
    if (!isOpen() || buffer == nullptr || maxLen == 0) {
        return -1;
    }
    return ::read(m_fd, buffer, maxLen);
}

int SerialPort::getFileDescriptor() const {
    return m_fd;
}

bool SerialPort::isOpen() const {
    return m_fd >= 0;
}

int SerialPort::getBaudRateCode(uint32_t baudRate) {
    switch (baudRate) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        case 1000000: return B1000000;
        case 2000000: return B2000000;
        case 3000000: return B3000000;
        default: return B115200; // Default
    }
}

bool SerialPort::configure(const SerialConfig& config) {
    struct termios tty;
    if (tcgetattr(m_fd, &tty) != 0) {
        return false;
    }

    // Set baud rate
    int baudCode = getBaudRateCode(config.baudRate);
    cfsetospeed(&tty, baudCode);
    cfsetispeed(&tty, baudCode);

    // 8N1 default or configurable
    tty.c_cflag &= ~CSIZE;
    switch (config.dataBits) {
        case 5: tty.c_cflag |= CS5; break;
        case 6: tty.c_cflag |= CS6; break;
        case 7: tty.c_cflag |= CS7; break;
        case 8: default: tty.c_cflag |= CS8; break;
    }

    if (config.stopBits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= ~CSTOPB;
    }

    switch (config.parity) {
        case ParityMode::EVEN:
            tty.c_cflag |= PARENB;
            tty.c_cflag &= ~PARODD;
            break;
        case ParityMode::ODD:
            tty.c_cflag |= PARENB;
            tty.c_cflag |= PARODD;
            break;
        case ParityMode::NONE:
        default:
            tty.c_cflag &= ~PARENB;
            break;
    }

    // Disable hardware flow control
    tty.c_cflag &= ~CRTSCTS;
    // Enable receiver, ignore modem control lines
    tty.c_cflag |= CREAD | CLOCAL;

    // Disable canonical mode, echo, and signals
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    // Disable software flow control
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    // Disable special handling of bytes
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    // Disable output processing
    tty.c_oflag &= ~(OPOST | ONLCR);

    // VMIN and VTIME
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0; // Non-blocking read

    if (tcsetattr(m_fd, TCSANOW, &tty) != 0) {
        return false;
    }

    return true;
}

#endif

} // namespace serial
