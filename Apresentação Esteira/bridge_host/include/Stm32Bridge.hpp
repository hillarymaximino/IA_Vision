#pragma once

#include <cstdint>
#include <string>

namespace bridge {

// ==============================================================================
// Definição do Protocolo de Comunicação
// ==============================================================================

/**
 * @brief Byte de comando indicando detecção de objeto pelo Host.
 * Valor: 0xA1 (161 decimal)
 */
constexpr uint8_t CMD_OBJETO_ENCONTRADO = 0xA1;

/**
 * @brief Byte de comando indicando ausência de objeto pelo Host.
 * Valor: 0xA0 (160 decimal)
 */
constexpr uint8_t CMD_SEM_OBJETO        = 0xA0;

/**
 * @brief Byte de Acknowledge (ACK) utilizado no handshake bidirecional.
 * Valor: 0x06 (ASCII ACK - padrão ISO/IEC 646 e alinhado com a BSP STM32)
 */
constexpr uint8_t BYTE_ACK              = 0x06;

/**
 * @brief Byte de Negative Acknowledge (NAK).
 * Valor: 0x15 (ASCII NAK)
 */
constexpr uint8_t BYTE_NACK             = 0x15;

/**
 * @brief Byte de comando enviado pelo STM32 solicitando parar varredura (StopScan).
 * Valor: 0xB0 (176 decimal)
 */
constexpr uint8_t CMD_HOST_STOP_SCAN    = 0xB0;

/**
 * @brief Byte de comando enviado pelo STM32 solicitando retomar varredura (ResumeScan / RequestScan).
 * Valor: 0xB1 (177 decimal)
 */
constexpr uint8_t CMD_HOST_RESUME_SCAN  = 0xB1;

// ==============================================================================
// Funções Principais da Abstração Solicitada
// ==============================================================================

/**
 * @brief Inicializa a comunicação serial e conecta ao microcontrolador STM32.
 * 
 * Ordem de resolução da porta:
 *  1. customPort, se informado (ex: "/dev/ttyACM0" no Linux, "COM5" no Windows);
 *  2. variável de ambiente SERIAL_PORT;
 *  3. varredura automática (Linux: /sys/class/tty/; Windows: SetupAPI) pelo ST-Link
 *     da STMicroelectronics (VID 0483 / PID 374b) ou porta com "STM32"/"STLink"
 *     na descrição ou VID 0483.
 * A conexão é estabelecida em 115200 bps 8N1 no Modo Raw.
 * 
 * @param customPort Caminho opcional da porta serial (padrão "" para auto-descoberta).
 * @return true se o STM32 foi encontrado e a conexão foi estabelecida com sucesso.
 * @return false se nenhum dispositivo foi encontrado ou se a porta falhou ao abrir.
 */
bool conectar(const std::string& customPort = "");

/**
 * @brief Retorna o caminho da porta serial atualmente conectada (ex: "/dev/ttyACM0").
 */
std::string obter_porta_conectada();

/**
 * @brief Envia o byte de comando indicando objeto encontrado (0xA1) com handshake mútuo (ACK).
 * 
 * Fluxo de Handshake Bidirecional:
 * 1. Host envia o byte CMD_OBJETO_ENCONTRADO (0xA1) para o STM32.
 * 2. Host aguarda o recebimento do BYTE_ACK (0x06) vindo do STM32.
 * 3. Ao receber o ACK do STM32, o Host envia imediatamente um BYTE_ACK (0x06) de volta
 *    para o STM32, concluindo o ciclo completo de handshake entre ambos os lados.
 * 
 * @return true se o comando foi enviado e o handshake foi confirmado por ambos os lados.
 * @return false se houve timeout na espera pelo ACK ou falha de comunicação.
 */
bool enviar_objeto_encontrado();

/**
 * @brief Envia o byte de comando indicando ausência de objeto (0xA0) com handshake mútuo (ACK).
 * 
 * Fluxo de Handshake Bidirecional:
 * 1. Host envia o byte CMD_SEM_OBJETO (0xA0) para o STM32.
 * 2. Host aguarda o recebimento do BYTE_ACK (0x06) vindo do STM32.
 * 3. Ao receber o ACK do STM32, o Host envia imediatamente um BYTE_ACK (0x06) de volta
 *    para o STM32, concluindo o ciclo completo de handshake entre ambos os lados.
 * 
 * @return true se o comando foi enviado e o handshake foi confirmado por ambos os lados.
 * @return false se houve timeout na espera pelo ACK ou falha de comunicação.
 */
bool enviar_sem_objeto();

// ==============================================================================
// Funções Auxiliares de Gerenciamento
// ==============================================================================

/**
 * @brief Encerra a conexão serial com o STM32 e libera os recursos e threads (RAII).
 */
void desconectar();

/**
 * @brief Verifica se a conexão serial está atualmente aberta e ativa.
 * @return true se conectado.
 */
bool esta_conectado();

/**
 * @brief Retorna o status atual da flag de varredura (StopScan / ResumeScan).
 * @return true se a varredura estiver ativa (ResumeScan recebido).
 * @return false se a varredura estiver pausada/parada (StopScan recebido).
 */
bool esta_varredura_ativa();

// Aliases para conveniência sintática
inline bool envia_objeto_encotrado() { return enviar_objeto_encontrado(); }
inline bool enviarObjetoEncontrado() { return enviar_objeto_encontrado(); }
inline bool enviarSemObjeto() { return enviar_sem_objeto(); }
inline bool is_scan_active() { return esta_varredura_ativa(); }
inline bool isScanActive() { return esta_varredura_ativa(); }

} // namespace bridge
