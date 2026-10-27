/******************************************************************************/
/**
 * @file    main.cpp
 * @addtogroup APP_MAIN
 * @brief   Ponto de entrada simplificado da aplicacao SICOV.
 * @author  Gonçalves
 * @details
 * \n <b>Grupo de Pesquisa:</b> ASSERT - IFPB
 * \n <b>Projeto:</b> SICOV
 *
 * Responsabilidade unica: ler a configuracao da linha de comando e invocar o
 * ciclo de vida do Orchestrator, sem gerenciar threads, drivers ou processamento
 * grafico diretamente.
 *
 * Uso:
 * \code
 *   VisionTest [--model <arquivo.onnx>] [--camera-sn <SN>] [--port <COMx|/dev/ttyACMx>]
 * \endcode
 *
 * @{
 ******************************************************************************/

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include "Orchestrator.hpp"
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

/*******************************************************************************
 * CONSTANTES DE CONFIGURACAO
 ******************************************************************************/

/** @brief Modelo neural padrao (relativo a pasta de execucao). */
static const char* const dDEFAULT_MODEL_PATH = "models/yolo11_formas.onnx";

/** @brief Camera da esteira. Use "" para conectar na primeira camera encontrada. */
static const char* const dDEFAULT_CAMERA_SN = "FBR26040067";

/*******************************************************************************
 * FUNCOES LOCAIS
 ******************************************************************************/

static void ImprimirUso(const char* programa)
{
    std::cout << "Uso: " << programa << " [opcoes]\n"
              << "  --model <arquivo>   Modelo ONNX (padrao: " << dDEFAULT_MODEL_PATH << ")\n"
              << "  --camera-sn <SN>    Numero de serie da camera Daheng (padrao: " << dDEFAULT_CAMERA_SN << ")\n"
              << "                      Use --camera-sn \"\" para a primeira camera encontrada.\n"
              << "  --port <porta>      Porta serial do STM32 (ex: COM5). Padrao: auto-deteccao\n"
              << "                      do ST-Link ou variavel de ambiente SERIAL_PORT.\n"
              << "  -h, --help          Mostra esta ajuda.\n";
}

/******************************************************************************/
/**
 * @brief  Funcao principal de entrada do executavel.
 * @param  argc Numero de argumentos via linha de comando.
 * @param  argv Vetor com os argumentos passados.
 * @return 0 em caso de encerramento normal; 1 em argumento invalido; -1 em erro na inicializacao.
 ******************************************************************************/
int main(int argc, char* argv[])
{
#ifdef _WIN32
    // Os logs dos modulos sao UTF-8; o console do Windows usa outra codepage por padrao
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string modelPath  = dDEFAULT_MODEL_PATH;
    std::string cameraSN   = dDEFAULT_CAMERA_SN;
    std::string serialPort;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool temValor = (i + 1 < argc);

        if (arg == "-h" || arg == "--help")
        {
            ImprimirUso(argv[0]);
            return 0;
        }
        else if (arg == "--model" && temValor)     { modelPath  = argv[++i]; }
        else if (arg == "--camera-sn" && temValor) { cameraSN   = argv[++i]; }
        else if (arg == "--port" && temValor)      { serialPort = argv[++i]; }
        else
        {
            std::cerr << "Argumento invalido: " << arg << "\n\n";
            ImprimirUso(argv[0]);
            return 1;
        }
    }

    // Inicializa todos os modulos integrados da arquitetura SICOV
    if (!Orchestrator_Init(modelPath, cameraSN, serialPort))
    {
        std::cerr << "Falha ao inicializar o Orchestrator." << std::endl;
        return -1;
    }

    // Transfere o controle para o orquestrador executar a pipeline continua
    Orchestrator_Run();

    return 0;
}

/** @} DOXYGEN GROUP TAG END OF FILE */
