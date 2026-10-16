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
 * Responsabilidade unica: invocar o ciclo de vida do Orchestrator de forma limpa,
 * sem gerenciar threads, drivers ou processamento grafico diretamente.
 *
 * @{
 ******************************************************************************/

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include "Orchestrator.hpp"
#include <iostream>

/******************************************************************************/
/**
 * @brief  Funcao principal de entrada do executavel.
 * @param  argc Numero de argumentos via linha de comando.
 * @param  argv Vetor com os argumentos passados.
 * @return 0 em caso de encerramento normal; -1 em caso de erro na inicializacao.
 ******************************************************************************/
int main(int argc, char* argv[])
{
    // Caminho para o modelo neural treinado e exportado em ONNX
    const std::string modelPath = "models/yolo11_formas.onnx";

    // Inicializa todos os modulos integrados da arquitetura SICOV
    // Altere a chamada para passar explicitamente o SN da camara irma:
    if (!Orchestrator_Init("models/yolo11_formas.onnx", "FBR26040067")) //ou FBR26040067 para segunda camera
    {
        std::cerr << "Falha ao inicializar o Orchestrator." << std::endl;
        return -1;
    }

    // Transfere o controle para o orquestrador executar a pipeline continua
    Orchestrator_Run();

    return 0;
}

/** @} DOXYGEN GROUP TAG END OF FILE */


///compila e roda
/*
cmake --build build -j$(nproc)
./build/VisionTest
*/