#include "Stm32Bridge.hpp"
#include "SerialClient.hpp"
#include "ISerialObserver.hpp"
#include "PortEnumerator.hpp"

#include <iostream>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <thread>
#include <cstdlib>

namespace bridge {

namespace {

constexpr const char* kStLinkVid = "0483";
constexpr const char* kStLinkPid = "374b";
constexpr uint32_t kBaudRate = 115200;
constexpr auto kAckTimeout = std::chrono::milliseconds(500);
// Validade do ultimo ResumeScan. O firmware reenvia o estado a cada ~1 s; 2,5 s
// tolera a perda de um heartbeat sem deixar a visao liberada com o STM32 mudo.
constexpr auto kScanStateTimeout = std::chrono::milliseconds(2500);
constexpr int kMaxRetries = 3;

/**
 * @brief Observador interno para monitoramento de ACKs e eventos assíncronos do STM32.
 */
class BridgeObserver : public serial::ISerialObserver {
public:
    explicit BridgeObserver(serial::SerialClient* client) : m_client(client) {}

    void onDataReceived(const std::vector<uint8_t>& data) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (uint8_t byte : data) {
            // Byte de confirmação (ACK) enviado pelo STM32
            if (byte == BYTE_ACK) {
                m_ackReceived = true;
                m_cv.notify_all();
            } else if (byte == BYTE_NACK) {
                m_nackReceived = true;
                m_cv.notify_all();
            } else if (byte == CMD_HOST_STOP_SCAN || byte == CMD_HOST_RESUME_SCAN) {
                // Estado de varredura vindo do STM32 (enviado na mudanca e reenviado a cada
                // ~1 s como heartbeat): atualiza a flag e responde ACK imediatamente
                const bool active = (byte == CMD_HOST_RESUME_SCAN);
                const bool changed = (active != m_scanActive) || !m_scanStateKnown;
                m_scanActive = active;
                m_scanStateKnown = true;
                m_lastScanMessage = std::chrono::steady_clock::now();
                if (m_client) {
                    std::vector<uint8_t> ack = {BYTE_ACK};
                    m_client->send(ack);
                }
                if (changed) {
                    std::cout << (active
                        ? "[Stm32Bridge] ResumeScan (0xB1): esteira em movimento. Varredura ATIVA."
                        : "[Stm32Bridge] StopScan (0xB0): esteira parada ou servo atuando. Varredura PAUSADA.")
                              << std::endl;
                }
            } else {
                // Bufferização de mensagens ASCII assíncronas do STM32 (ex: [btn]\r\n)
                if (byte == '\r') continue;
                if (byte == '\n') {
                    if (!m_asciiLine.empty()) {
                        processAsciiMessage(m_asciiLine);
                        m_asciiLine.clear();
                    }
                } else if (m_asciiLine.size() < 128) {
                    m_asciiLine.push_back(static_cast<char>(byte));
                }
            }
        }
    }

    void onError(const std::string& errorMessage) override {
        std::cerr << "[Stm32Bridge] Erro de comunicação serial: " << errorMessage << std::endl;
    }

    void onConnectionClosed() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_scanActive = false; // fail-safe: sem STM32 nao ha permissao para detectar
        m_cv.notify_all();
    }

    bool waitForAck(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        bool success = m_cv.wait_for(lock, timeout, [this] {
            return m_ackReceived || m_nackReceived || m_closed;
        });

        if (!success || m_closed || m_nackReceived) {
            return false;
        }

        return m_ackReceived;
    }

    void resetAck() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ackReceived = false;
        m_nackReceived = false;
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    /**
     * Fail-safe: so libera a varredura com conexao aberta, ultimo estado = ResumeScan
     * e esse estado renovado ha menos de kScanStateTimeout (o firmware reenvia a cada
     * ~1 s). STM32 travado, sem energia ou cabo solto => varredura pausada.
     */
    bool isScanActive() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || !m_scanActive) {
            return false;
        }
        return (std::chrono::steady_clock::now() - m_lastScanMessage) < kScanStateTimeout;
    }

private:
    void processAsciiMessage(const std::string& msg) {
        // Se o STM32 enviar notificação assíncrona com espera de confirmação (ex: [btn])
        if (msg == "[btn]") {
            if (m_client) {
                std::vector<uint8_t> ack = {BYTE_ACK};
                m_client->send(ack);
                std::cout << "[Stm32Bridge] Evento '" << msg << "' recebido do STM32. ACK enviado instantaneamente!" << std::endl;
            }
        } else {
            std::cout << "[Stm32Bridge] Mensagem recebida do STM32: " << msg << std::endl;
        }
    }

    serial::SerialClient* m_client;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_ackReceived = false;
    bool m_nackReceived = false;
    bool m_closed = false;
    bool m_scanActive = false;
    bool m_scanStateKnown = false;
    std::chrono::steady_clock::time_point m_lastScanMessage{};
    std::string m_asciiLine;
};

/**
 * @brief Singleton para gerenciar a instância de conexão e o ciclo de vida.
 */
class BridgeManager {
public:
    static BridgeManager& instance() {
        static BridgeManager mgr;
        return mgr;
    }

    bool connect(const std::string& customPort = "") {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        return connectInternal(customPort);
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        if (m_connected) {
            m_client.disconnect();
            m_observer.reset();
            m_connected = false;
            m_currentPort.clear();
            std::cout << "[Stm32Bridge] Desconectado." << std::endl;
        }
    }

    std::string getCurrentPort() const {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        return m_currentPort;
    }

    bool isConnected() const {
        return m_connected && (!m_observer || !m_observer->isClosed());
    }

    bool isScanActive() const {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        // Fail-safe: sem conexao valida (inclusive apos falha de escrita) nao ha permissao
        if (m_connected && m_observer) {
            return m_observer->isScanActive();
        }
        return false;
    }

    bool sendWithHandshake(uint8_t cmdByte, const char* cmdDescription) {
        std::lock_guard<std::mutex> lock(m_managerMutex);

        if (!m_connected || (m_observer && m_observer->isClosed())) {
            std::cout << "[Stm32Bridge] Não conectado ao enviar " << cmdDescription << ". Tentando conectar..." << std::endl;
            if (!connectInternal()) {
                std::cerr << "[Stm32Bridge] Falha ao conectar automaticamente." << std::endl;
                return false;
            }
        }

        const std::vector<uint8_t> payload = {cmdByte};
        const std::vector<uint8_t> hostAck = {BYTE_ACK};

        for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
            m_observer->resetAck();

            std::cout << "[Stm32Bridge] [Tentativa " << attempt << "/" << kMaxRetries 
                      << "] Enviando comando " << cmdDescription 
                      << " (0x" << std::hex << static_cast<int>(cmdByte) << std::dec << ")..." << std::endl;

            // 1. Host envia o byte de comando para o STM32
            if (!m_client.send(payload)) {
                // Escrita recusada pelo SO: o dispositivo USB caiu. Marca como desconectado
                // para que a proxima chamada (ou a reconexao da visao) reabra a porta.
                std::cerr << "[Stm32Bridge] Falha ao transmitir byte pela serial "
                             "(ST-Link desconectado?). Tentando reconectar..." << std::endl;
                m_connected = false;
                if (!connectInternal(m_currentPort)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            // 2. Host aguarda o ACK do STM32
            if (m_observer->waitForAck(kAckTimeout)) {
                std::cout << "[Stm32Bridge] ACK (0x06) recebido do STM32!" << std::endl;

                // 3. Handshake mútuo: Host envia ACK de volta ao STM32 para confirmar a conclusão do ciclo
                std::cout << "[Stm32Bridge] Enviando ACK (0x06) de confirmação mútua ao STM32..." << std::endl;
                m_client.send(hostAck);

                std::cout << "[Stm32Bridge] Handshake bidirecional concluído com sucesso para " << cmdDescription << "." << std::endl;
                return true;
            } else {
                std::cerr << "[Stm32Bridge] Timeout aguardando ACK do STM32 na tentativa " << attempt << "." << std::endl;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cerr << "[Stm32Bridge] Erro: Falha no handshake após " << kMaxRetries << " tentativas." << std::endl;
        return false;
    }

private:
    BridgeManager() = default;
    ~BridgeManager() {
        disconnect();
    }

    /**
     * Conecta (ou reconecta) ao STM32. Ordem: porta informada / SERIAL_PORT; se ela
     * nao abrir, busca automatica do ST-Link. Depois de uma queda do USB o ST-Link
     * volta com outro nome (/dev/ttyACM0 -> /dev/ttyACM1), entao a busca automatica
     * e o que permite reconectar sem intervencao.
     */
    bool connectInternal(const std::string& customPort = "") {
        if (m_connected && m_observer && !m_observer->isClosed()) {
            return true;
        }

        std::string requested = customPort;
        if (requested.empty()) {
            const char* envPort = std::getenv("SERIAL_PORT");
            if (envPort && envPort[0] != '\0') {
                requested = envPort;
            }
        }

        if (!requested.empty()) {
            std::cout << "[Stm32Bridge] Utilizando porta especificada: " << requested << std::endl;
            if (openPort(requested)) {
                return true;
            }
            std::cout << "[Stm32Bridge] Porta especificada indisponivel (o ST-Link pode ter reconectado "
                         "com outro nome). Procurando o STM32 automaticamente..." << std::endl;
        }

        const std::string found = findStLinkPort();
        if (found.empty()) {
            std::cerr << "[Stm32Bridge] Nenhum microcontrolador STM32 detectado automaticamente." << std::endl;
            auto available = serial::PortEnumerator::listPorts();
            if (!available.empty()) {
                std::cout << "[Stm32Bridge] Portas seriais encontradas no sistema:" << std::endl;
                for (const auto& p : available) {
                    std::cout << "  - " << p.portPath << " (VID: " << p.vendorId << ", PID: " << p.productId
                              << ", Desc: " << p.description << ")" << std::endl;
                }
            }
            return false;
        }
        if (found == requested) {
            return false;   // ja tentada acima
        }
        return openPort(found);
    }

    /** Procura o ST-Link: VID/PID oficial (0483:374b) ou descricao/VID da STMicroelectronics. */
    static std::string findStLinkPort() {
        auto stLink = serial::PortEnumerator::findByVidPid(kStLinkVid, kStLinkPid);
        if (stLink) {
            std::cout << "[Stm32Bridge] Dispositivo STM32 ST-Link localizado em: "
                      << stLink->portPath << " (" << stLink->description << ")" << std::endl;
            return stLink->portPath;
        }
        for (const auto& p : serial::PortEnumerator::listPorts()) {
            if (p.description.find("STM32") != std::string::npos ||
                p.description.find("STLink") != std::string::npos ||
                p.vendorId == kStLinkVid) {
                std::cout << "[Stm32Bridge] Dispositivo compatível localizado em: "
                          << p.portPath << " (" << p.description << ")" << std::endl;
                return p.portPath;
            }
        }
        return {};
    }

    /** Abre a porta e instala um observador novo (o anterior pode estar fechado). */
    bool openPort(const std::string& targetPort) {
        std::cout << "[Stm32Bridge] Conectando a " << targetPort << " @ " << kBaudRate << " 8N1..." << std::endl;
        if (!m_client.connect(targetPort, kBaudRate)) {
            std::cerr << "[Stm32Bridge] Falha ao abrir porta serial: " << targetPort << std::endl;
            return false;
        }

        m_observer = std::make_unique<BridgeObserver>(&m_client);
        m_client.addObserver(m_observer.get());
        m_connected = true;
        m_currentPort = targetPort;

        // Descarta dados residuais na inicialização
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)m_client.readBufferedMessages();

        std::cout << "[Stm32Bridge] Conexão com STM32 estabelecida com sucesso em " << targetPort << "." << std::endl;
        return true;
    }

    serial::SerialClient m_client;
    std::unique_ptr<BridgeObserver> m_observer;
    bool m_connected = false;
    std::string m_currentPort;
    mutable std::mutex m_managerMutex;
};

} // namespace

// ==============================================================================
// Implementação das Funções da Interface Pública
// ==============================================================================

bool conectar(const std::string& customPort) {
    return BridgeManager::instance().connect(customPort);
}

std::string obter_porta_conectada() {
    return BridgeManager::instance().getCurrentPort();
}

bool enviar_objeto_encontrado() {
    return BridgeManager::instance().sendWithHandshake(CMD_OBJETO_ENCONTRADO, "OBJETO_ENCONTRADO");
}

bool enviar_sem_objeto() {
    return BridgeManager::instance().sendWithHandshake(CMD_SEM_OBJETO, "SEM_OBJETO");
}

void desconectar() {
    BridgeManager::instance().disconnect();
}

bool esta_conectado() {
    return BridgeManager::instance().isConnected();
}

bool esta_varredura_ativa() {
    return BridgeManager::instance().isScanActive();
}

} // namespace bridge
