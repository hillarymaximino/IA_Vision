/******************************************************************************/
/**
 * @file    Orchestrator.hpp
 * @addtogroup ORCHESTRATOR
 * @brief   Interface do orquestrador central e coordenador de fluxo do SICOV.
 * @author  Gonçalves
 * @details
 * \n <b>Grupo de Pesquisa:</b> ASSERT - IFPB
 * \n <b>Projeto:</b> SICOV
 *
 * Responsavel por sincronizar o Produtor (CameraModule) e o Consumidor (VisionModule),
 * gerenciar o ciclo de vida global e atuar como Fonte Unica da Verdade para as
 * dimensoes da Zona de Inspecao (ROI) e renderizacao do HUD.
 *
 * @{
 ******************************************************************************/

#ifndef ORCHESTRATOR_HPP
#define ORCHESTRATOR_HPP

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include <string>

/*******************************************************************************
 * PROTOTIPOS PUBLICOS
 ******************************************************************************/

/******************************************************************************/
/**
 * @brief  Inicializa todos os subsistemas dependentes do projeto SICOV.
 * @param  modelPath Caminho para o modelo ONNX da IA.
 * @param  cameraSN Numero de serie opcional da camera industrial.
 * @return true se o modulo de camera e o modulo de visao inicializarem sem erros.
 ******************************************************************************/
bool Orchestrator_Init(const std::string& modelPath, const std::string& cameraSN = "");

/******************************************************************************/
/**
 * @brief  Executa a maquina de estados principal do sistema.
 * @return void
 * @details Coordena o recebimento de quadros, a submissao a inferencia, a 
 *          filtragem espacial pelo enquadramento central e a apresentacao em tela.
 ******************************************************************************/
void Orchestrator_Run(void);

/******************************************************************************/
/**
 * @brief  Desliga graciosamente todos os modulos e libera buffers de video.
 * @return void
 ******************************************************************************/
void Orchestrator_Stop(void);

#endif /* ORCHESTRATOR_HPP */
/** @} DOXYGEN GROUP TAG END OF FILE */