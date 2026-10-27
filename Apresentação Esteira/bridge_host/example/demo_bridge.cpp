#include "Stm32Bridge.hpp"
#include "PortEnumerator.hpp"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/** No Windows, habilita UTF-8 e sequencias ANSI (cores) no console. */
void setupConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

// Códigos ANSI para estilização no terminal
constexpr const char* COLOR_RESET   = "\033[0m";
constexpr const char* COLOR_RED     = "\033[1;31m";
constexpr const char* COLOR_GREEN   = "\033[1;32m";
constexpr const char* COLOR_YELLOW  = "\033[1;33m";
constexpr const char* COLOR_BLUE    = "\033[1;34m";
constexpr const char* COLOR_MAGENTA = "\033[1;35m";
constexpr const char* COLOR_CYAN    = "\033[1;36m";
constexpr const char* COLOR_BOLD    = "\033[1m";

void printBanner() {
    std::cout << COLOR_CYAN << COLOR_BOLD
              << "╔════════════════════════════════════════════════════════════════════╗\n"
              << "║            STM32 UART BRIDGE - INTERFACE TERMINAL (CLI)            ║\n"
              << "║       Handshake Bidirecional (ACK 0x06) & Controle de Varredura    ║\n"
              << "║           (Host PC <-> Microcontrolador STM32 <-> Controladora)    ║\n"
              << "╚════════════════════════════════════════════════════════════════════╝\n"
              << COLOR_RESET << std::endl;
}

void printHelp() {
    std::cout << COLOR_BOLD << "\nComandos disponíveis no terminal:\n" << COLOR_RESET
              << "  " << COLOR_GREEN << "1" << COLOR_RESET << " ou " << COLOR_GREEN << "obj" << COLOR_RESET
              << "          -> Chama " << COLOR_BOLD << "enviar_objeto_encontrado()" << COLOR_RESET << " (0xA1) com ACK mútuo\n"
              << "  " << COLOR_YELLOW << "0" << COLOR_RESET << " ou " << COLOR_YELLOW << "sem" << COLOR_RESET
              << "          -> Chama " << COLOR_BOLD << "enviar_sem_objeto()" << COLOR_RESET << " (0xA0) com ACK mútuo\n"
              << "  " << COLOR_CYAN << "v" << COLOR_RESET << " ou " << COLOR_CYAN << "scan" << COLOR_RESET
              << "         -> Consulta o status da flag de varredura (StopScan 0xB0 / ResumeScan 0xB1)\n"
              << "  " << COLOR_CYAN << "c" << COLOR_RESET << " [porta]     -> Conecta ou reconecta (sem argumento = auto-detecta STM32)\n"
              << "  " << COLOR_MAGENTA << "d" << COLOR_RESET << "             -> Desconecta da porta serial\n"
              << "  " << COLOR_BLUE << "s" << COLOR_RESET << "             -> Mostra status da conexão, varredura e portas seriais\n"
              << "  " << COLOR_CYAN << "t" << COLOR_RESET << " [N]         -> Teste de benchmark / estresse com N repetições (padrão: 5)\n"
              << "  " << COLOR_BOLD << "l" << COLOR_RESET << "             -> Limpa a tela do terminal\n"
              << "  " << COLOR_BOLD << "h" << COLOR_RESET << " ou " << COLOR_BOLD << "?" << COLOR_RESET
              << "          -> Exibe este menu de ajuda\n"
              << "  " << COLOR_RED << "q" << COLOR_RESET << " ou " << COLOR_RED << "sair" << COLOR_RESET
              << "       -> Finaliza a aplicação de forma limpa\n"
              << std::endl;
}

void showStatus() {
    std::cout << "\n" << COLOR_BOLD << "--- Status Atual da Conexão e Aplicação ---" << COLOR_RESET << "\n";
    if (bridge::esta_conectado()) {
        bool scanActive = bridge::esta_varredura_ativa();
        std::cout << "  Estado     : " << COLOR_GREEN << "CONECTADO" << COLOR_RESET << "\n"
                  << "  Porta      : " << COLOR_CYAN << bridge::obter_porta_conectada() << COLOR_RESET << "\n"
                  << "  Config     : 115200 8N1 (Raw Mode)\n"
                  << "  Varredura  : " << (scanActive ? (std::string(COLOR_GREEN) + "ATIVA (ResumeScan recebido)") : (std::string(COLOR_YELLOW) + "PAUSADA (StopScan recebido)")) << COLOR_RESET << "\n"
                  << "  Protocolo:\n"
                  << "    - Objeto Detectado (Host -> Micro) : 0x" << std::hex << (int)bridge::CMD_OBJETO_ENCONTRADO << std::dec << "\n"
                  << "    - Sem Objeto (Host -> Micro)       : 0x" << std::hex << (int)bridge::CMD_SEM_OBJETO << std::dec << "\n"
                  << "    - StopScan (Micro -> Host)         : 0x" << std::hex << (int)bridge::CMD_HOST_STOP_SCAN << std::dec << "\n"
                  << "    - ResumeScan (Micro -> Host)       : 0x" << std::hex << (int)bridge::CMD_HOST_RESUME_SCAN << std::dec << "\n"
                  << "    - Handshake ACK                    : 0x" << std::hex << (int)bridge::BYTE_ACK << std::dec << "\n";
    } else {
        std::cout << "  Estado     : " << COLOR_RED << "DESCONECTADO" << COLOR_RESET << "\n";
    }

    std::cout << "\n" << COLOR_BOLD << "Portas seriais detectadas no sistema:" << COLOR_RESET << "\n";
    auto ports = serial::PortEnumerator::listPorts();
    if (ports.empty()) {
        std::cout << "  (Nenhuma porta serial encontrada em /sys/class/tty/)\n";
    } else {
        for (const auto& p : ports) {
            bool isCurrent = bridge::esta_conectado() && (p.portPath == bridge::obter_porta_conectada());
            std::cout << "  " << (isCurrent ? COLOR_GREEN : COLOR_YELLOW)
                      << p.portPath << COLOR_RESET
                      << " [VID: " << (p.vendorId.empty() ? "----" : p.vendorId)
                      << " PID: " << (p.productId.empty() ? "----" : p.productId) << "] "
                      << p.description << (isCurrent ? " (EM USO)" : "") << "\n";
        }
    }
    std::cout << std::endl;
}

void executeBenchmark(int iterations) {
    if (!bridge::esta_conectado()) {
        std::cout << COLOR_RED << "Erro: Não é possível rodar o teste. Conecte primeiro com 'c'.\n" << COLOR_RESET;
        return;
    }

    if (iterations <= 0) iterations = 5;

    std::cout << "\n" << COLOR_CYAN << "Iniciando benchmark de " << iterations 
              << " ciclos com handshake mútuo..." << COLOR_RESET << "\n";

    int successCount = 0;
    double totalTimeMs = 0.0;
    double minTimeMs = 999999.0;
    double maxTimeMs = 0.0;

    for (int i = 1; i <= iterations; ++i) {
        std::cout << "  [" << i << "/" << iterations << "] ";

        auto tStart = std::chrono::high_resolution_clock::now();
        bool ok = (i % 2 == 1) ? bridge::enviar_objeto_encontrado() : bridge::enviar_sem_objeto();
        auto tEnd = std::chrono::high_resolution_clock::now();

        double elapsedMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

        if (ok) {
            successCount++;
            totalTimeMs += elapsedMs;
            if (elapsedMs < minTimeMs) minTimeMs = elapsedMs;
            if (elapsedMs > maxTimeMs) maxTimeMs = elapsedMs;
            std::cout << COLOR_GREEN << "SUCESSO" << COLOR_RESET 
                      << " (" << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";
        } else {
            std::cout << COLOR_RED << "FALHA / TIMEOUT\n" << COLOR_RESET;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    std::cout << "\n" << COLOR_BOLD << "=== Resultado do Benchmark ===" << COLOR_RESET << "\n"
              << "  Taxa de Sucesso: " 
              << (successCount == iterations ? COLOR_GREEN : COLOR_YELLOW)
              << successCount << "/" << iterations << " (" 
              << (successCount * 100.0 / iterations) << "%)\n" << COLOR_RESET;

    if (successCount > 0) {
        double avgTimeMs = totalTimeMs / successCount;
        std::cout << "  Latência Mínima: " << minTimeMs << " ms\n"
                  << "  Latência Média : " << avgTimeMs << " ms\n"
                  << "  Latência Máxima: " << maxTimeMs << " ms\n";
    }
    std::cout << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    setupConsole();
    printBanner();

    // 1. Conexão inicial
    std::string initialPort;
    if (argc > 1) {
        initialPort = argv[1];
        std::cout << "Tentando conectar na porta especificada: " << COLOR_CYAN << initialPort << COLOR_RESET << "...\n";
        bridge::conectar(initialPort);
    } else {
        std::cout << "Procurando microcontrolador STM32 automaticamente...\n";
        bridge::conectar();
    }

    if (bridge::esta_conectado()) {
        std::cout << COLOR_GREEN << "STM32 Conectado com sucesso em " 
                  << bridge::obter_porta_conectada() << "!" << COLOR_RESET << "\n";
    } else {
        std::cout << COLOR_YELLOW 
                  << "Aviso: Placa não conectada no momento. Você ainda pode usar o terminal e conectar depois com 'c'." 
                  << COLOR_RESET << "\n";
    }

    printHelp();

    // 2. Loop de interação via terminal
    std::string line;
    while (true) {
        // Exibe prompt dinâmico com o status da conexão e da varredura
        if (bridge::esta_conectado()) {
            bool scanActive = bridge::esta_varredura_ativa();
            std::cout << COLOR_GREEN << "[" << bridge::obter_porta_conectada() << "]" << COLOR_RESET
                      << (scanActive ? (std::string(COLOR_GREEN) + "[SCAN: ATIVO]") : (std::string(COLOR_YELLOW) + "[SCAN: PAUSADO]")) << COLOR_RESET
                      << COLOR_BOLD << " bridge > " << COLOR_RESET << std::flush;
        } else {
            std::cout << COLOR_RED << "[Desconectado]" << COLOR_RESET 
                      << COLOR_BOLD << " bridge > " << COLOR_RESET << std::flush;
        }

        if (!std::getline(std::cin, line)) {
            std::cout << "\nEOF recebido. Saindo...\n";
            break;
        }

        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "q" || cmd == "sair" || cmd == "exit") {
            break;
        } else if (cmd == "1" || cmd == "obj" || cmd == "objeto") {
            std::cout << COLOR_CYAN << ">> Enviando CMD_OBJETO_ENCONTRADO (0x" 
                      << std::hex << (int)bridge::CMD_OBJETO_ENCONTRADO << std::dec << ")..." << COLOR_RESET << "\n";
            auto start = std::chrono::high_resolution_clock::now();
            bool ok = bridge::enviar_objeto_encontrado();
            auto end = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();

            if (ok) {
                std::cout << COLOR_GREEN << "✓ Handshake Mútuo Concluído com Sucesso!" << COLOR_RESET 
                          << " (Tempo RTT: " << std::fixed << std::setprecision(2) << ms << " ms)\n";
            } else {
                std::cout << COLOR_RED << "✗ Falha no Handshake (Timeout ou Conexão fechada).\n" << COLOR_RESET;
            }
        } else if (cmd == "0" || cmd == "sem") {
            std::cout << COLOR_CYAN << ">> Enviando CMD_SEM_OBJETO (0x" 
                      << std::hex << (int)bridge::CMD_SEM_OBJETO << std::dec << ")..." << COLOR_RESET << "\n";
            auto start = std::chrono::high_resolution_clock::now();
            bool ok = bridge::enviar_sem_objeto();
            auto end = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();

            if (ok) {
                std::cout << COLOR_GREEN << "✓ Handshake Mútuo Concluído com Sucesso!" << COLOR_RESET 
                          << " (Tempo RTT: " << std::fixed << std::setprecision(2) << ms << " ms)\n";
            } else {
                std::cout << COLOR_RED << "✗ Falha no Handshake (Timeout ou Conexão fechada).\n" << COLOR_RESET;
            }
        } else if (cmd == "c" || cmd == "conectar") {
            std::string portArg;
            ss >> portArg;
            if (portArg.empty()) {
                std::cout << "Buscando STM32 automaticamente...\n";
                bridge::conectar();
            } else {
                std::cout << "Conectando na porta " << portArg << "...\n";
                bridge::conectar(portArg);
            }

            if (bridge::esta_conectado()) {
                std::cout << COLOR_GREEN << "Conectado com sucesso em " 
                          << bridge::obter_porta_conectada() << "!\n" << COLOR_RESET;
            } else {
                std::cout << COLOR_RED << "Falha ao conectar.\n" << COLOR_RESET;
            }
        } else if (cmd == "d" || cmd == "desconectar") {
            bridge::desconectar();
            std::cout << COLOR_YELLOW << "Desconectado pelo usuário.\n" << COLOR_RESET;
        } else if (cmd == "s" || cmd == "status") {
            showStatus();
        } else if (cmd == "v" || cmd == "scan" || cmd == "varredura") {
            if (!bridge::esta_conectado()) {
                std::cout << COLOR_RED << "Placa não conectada.\n" << COLOR_RESET;
            } else {
                bool scanActive = bridge::esta_varredura_ativa();
                std::cout << "Status da Varredura pelo STM32: "
                          << (scanActive ? (std::string(COLOR_GREEN) + "ATIVA (Câmera liberada para buscar objetos)")
                                         : (std::string(COLOR_YELLOW) + "PAUSADA (Aguardando atuação do servo/motor)"))
                          << COLOR_RESET << "\n";
            }
        } else if (cmd == "t" || cmd == "teste" || cmd == "bench") {
            int iterations = 5;
            ss >> iterations;
            executeBenchmark(iterations);
        } else if (cmd == "l" || cmd == "clear" || cmd == "limpar") {
            std::cout << "\033[2J\033[H";
            printBanner();
        } else if (cmd == "h" || cmd == "help" || cmd == "?") {
            printHelp();
        } else {
            std::cout << COLOR_YELLOW << "Comando desconhecido: '" << cmd 
                      << "'. Digite 'h' para listar os comandos.\n" << COLOR_RESET;
        }
    }

    // 3. Limpeza RAII
    bridge::desconectar();
    std::cout << COLOR_CYAN << "Sessão finalizada. Até logo!\n" << COLOR_RESET;
    return 0;
}
