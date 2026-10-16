/******************************************************************************/
/**
 * @file    CameraModule.hpp
 * @addtogroup CAMERA
 * @brief   Interface do driver de captura GigE Vision para cameras Daheng Imaging.
 * @author  Gonçalves
 * @details
 * \n <b>Grupo de Pesquisa:</b> ASSERT - IFPB
 * \n <b>Projeto:</b> SICOV
 * \n <b>Dispositivo:</b> MER2-302-37GM-P (Monocromatica, GigE Vision PoE).
 * \n <b>SDK:</b> Galaxy C++ API (GxIAPI).
 *
 * Declara as operacoes para inicializacao de barramento, configuracao de pacotes
 * GigE, captura continua em thread e exportacao de matrizes para OpenCV.
 *
 * @{
 ******************************************************************************/

#ifndef CAMERA_MODULE_HPP
#define CAMERA_MODULE_HPP

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include <opencv2/opencv.hpp>
#include <string>

/*******************************************************************************
 * PROTOTIPOS PUBLICOS
 ******************************************************************************/

/******************************************************************************/
/**
 * @brief  Inicializa a pilha de comunicacao do Galaxy SDK e abre a camera Daheng.
 * @param  cameraSN Numero de serie da camera. Se vazio (""), conecta na primeira encontrada.
 * @return true se o dispositivo foi localizado, aberto e o fluxo configurado; false caso contrario.
 * @note   Configura o parametro GevSCPSPacketSize para compatibilidade com Jumbo Frames.
 ******************************************************************************/
bool Camera_Init(const std::string& cameraSN = "");

/******************************************************************************/
/**
 * @brief  Dispara a thread de aquisicao de imagens do sensor em segundo plano.
 * @return void
 ******************************************************************************/
void Camera_Start(void);

/******************************************************************************/
/**
 * @brief  Encerra o fluxo de aquisicao e desaloca os recursos do Galaxy SDK.
 * @return void
 ******************************************************************************/
void Camera_Stop(void);

/******************************************************************************/
/**
 * @brief  Recupera de forma thread-safe o quadro mais recente da camera.
 * @param  outFrame Matriz cv::Mat de saida que recebera a imagem em formato BGR (3 canais).
 * @return true se existe um quadro novo disponivel; false se o buffer estiver vazio.
 ******************************************************************************/
bool Camera_GetFrame(cv::Mat& outFrame);

#endif /* CAMERA_MODULE_HPP */
/** @} DOXYGEN GROUP TAG END OF FILE */