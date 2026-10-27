#include "PortEnumerator.hpp"

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <algorithm>
#include <cctype>
#else
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
#endif

namespace serial {

#ifdef _WIN32

namespace {

/** Classe de dispositivos "Ports (COM & LPT)": {4D36E978-E325-11CE-BFC1-08002BE10318}. */
const GUID kGuidDevClassPorts = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

/** Converte UTF-16 (API "W") para UTF-8, o mesmo encoding usado no restante do projeto. */
std::string toUtf8(const wchar_t* text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string readRegistryProperty(HDEVINFO devs, SP_DEVINFO_DATA& dev, DWORD property) {
    wchar_t buffer[512] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(devs, &dev, property, nullptr,
                                           reinterpret_cast<PBYTE>(buffer), sizeof(buffer) - sizeof(wchar_t), nullptr)) {
        return {};
    }
    // REG_MULTI_SZ (HARDWAREID): o primeiro item ja e o mais especifico
    return toUtf8(buffer);
}

std::string readPortName(HDEVINFO devs, SP_DEVINFO_DATA& dev) {
    HKEY key = SetupDiOpenDevRegKey(devs, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) {
        return {};
    }
    wchar_t name[64] = {};
    DWORD size = sizeof(name) - sizeof(wchar_t);
    DWORD type = 0;
    LONG status = RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<LPBYTE>(name), &size);
    RegCloseKey(key);
    return (status == ERROR_SUCCESS && type == REG_SZ) ? toUtf8(name) : std::string();
}

/** Extrai o valor hexadecimal apos "VID_" / "PID_" em minusculas, como no sysfs ("0483"). */
std::string extractHexId(const std::string& hardwareId, const char* tag) {
    std::string upper = hardwareId;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    size_t pos = upper.find(tag);
    if (pos == std::string::npos || pos + 4 + 4 > upper.size()) {
        return {};
    }
    std::string id = hardwareId.substr(pos + 4, 4);
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return id;
}

} // namespace

std::vector<DeviceInfo> PortEnumerator::listPorts() {
    std::vector<DeviceInfo> ports;

    HDEVINFO devs = SetupDiGetClassDevsA(&kGuidDevClassPorts, nullptr, nullptr, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        return ports;
    }

    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); ++i) {
        std::string portName = readPortName(devs, dev);
        if (portName.rfind("COM", 0) != 0) {
            continue; // ignora LPT e afins
        }

        std::string hardwareId = readRegistryProperty(devs, dev, SPDRP_HARDWAREID);

        DeviceInfo info;
        info.portPath = portName;
        info.vendorId = extractHexId(hardwareId, "VID_");
        info.productId = extractHexId(hardwareId, "PID_");
        // Ex.: "STMicroelectronics STLink Virtual COM Port (COM5)"
        info.description = readRegistryProperty(devs, dev, SPDRP_FRIENDLYNAME);
        if (info.description.empty()) {
            info.description = readRegistryProperty(devs, dev, SPDRP_DEVICEDESC);
        }

        ports.push_back(info);
    }

    SetupDiDestroyDeviceInfoList(devs);
    return ports;
}

#else

std::vector<DeviceInfo> PortEnumerator::listPorts() {
    std::vector<DeviceInfo> ports;
    const std::string sysClassTty = "/sys/class/tty/";

    if (!fs::exists(sysClassTty)) {
        return ports;
    }

    for (const auto& entry : fs::directory_iterator(sysClassTty)) {
        if (!entry.is_symlink() && !entry.is_directory()) continue;

        std::string ttyName = entry.path().filename().string();
        std::string devicePath = entry.path().string() + "/device";

        if (!fs::exists(devicePath)) continue;

        DeviceInfo info;
        info.portPath = "/dev/" + ttyName;

        // Try to find idVendor and idProduct by going up the device tree
        fs::path currentPath = fs::canonical(devicePath);

        while (currentPath != "/" && currentPath.string().find("/sys/devices") != std::string::npos) {
            fs::path vendorPath = currentPath / "idVendor";
            fs::path productPath = currentPath / "idProduct";

            if (fs::exists(vendorPath) && fs::exists(productPath)) {
                std::ifstream vendorFile(vendorPath);
                std::ifstream productFile(productPath);
                
                if (vendorFile >> info.vendorId && productFile >> info.productId) {
                    // Trim newlines if any
                    info.vendorId.erase(info.vendorId.find_last_not_of(" \n\r\t") + 1);
                    info.productId.erase(info.productId.find_last_not_of(" \n\r\t") + 1);
                }
                
                fs::path manufacturerPath = currentPath / "manufacturer";
                fs::path productDescPath = currentPath / "product";
                std::string mfg, prod;
                
                if (fs::exists(manufacturerPath)) {
                    std::ifstream mfgFile(manufacturerPath);
                    std::getline(mfgFile, mfg);
                }
                if (fs::exists(productDescPath)) {
                    std::ifstream prodFile(productDescPath);
                    std::getline(prodFile, prod);
                }
                
                if (!mfg.empty() && !prod.empty()) {
                    info.description = mfg + " " + prod;
                } else if (!prod.empty()) {
                    info.description = prod;
                } else if (!mfg.empty()) {
                    info.description = mfg;
                }
                
                break;
            }
            currentPath = currentPath.parent_path();
        }
        
        ports.push_back(info);
    }

    return ports;
}

#endif

std::optional<DeviceInfo> PortEnumerator::findByVidPid(const std::string& vid, const std::string& pid) {
    auto ports = listPorts();
    for (const auto& port : ports) {
        if (port.vendorId == vid && port.productId == pid) {
            return port;
        }
    }
    return std::nullopt;
}

} // namespace serial
