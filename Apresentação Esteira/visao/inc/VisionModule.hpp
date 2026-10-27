/******************************************************************************/
/**
 * @file    VisionModule.hpp
 * @addtogroup VISION
 * @brief   Interface do pipeline de IA desacoplado (Consumidor).
 * @author  Gonçalves
 * @author  Luiz Neto
 * @details
 * \n <b>Grupo de Pesquisa:</b> ASSERT - IFPB
 * \n <b>Projeto:</b> SICOV
 *
 * Executa o YOLO11 (ONNX) em uma thread propria. O Orchestrator submete quadros
 * com Vision_SubmitFrame() e le o resultado mais recente com Vision_GetLatest().
 *
 * @{
 ******************************************************************************/

#ifndef VISION_MODULE_HPP
#define VISION_MODULE_HPP

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include <opencv2/opencv.hpp>
#include <cstdint>
#include <vector>
#include <string>

/*******************************************************************************
 * TIPOS PUBLICOS
 ******************************************************************************/

/** @brief Uma deteccao apos NMS, em coordenadas de pixel do quadro original. */
struct DetectionResult
{
    cv::Rect box;       /**< Caixa delimitadora. */
    int classId;        /**< Classe conforme data.yaml (0 = Circulo ... 5 = Triangulo). */
    float confidence;   /**< Confianca da classe vencedora. */
    bool isValid;       /**< true se classId pertence ao catalogo treinado. */
};

/**
 * @brief Resultado de uma inferencia junto com o quadro exato em que ela rodou.
 * @note  As caixas so valem para `frame`. Desenhar ou testar a ROI num quadro mais
 *        novo da camera desloca as caixas em relacao ao objeto com a esteira andando.
 */
struct InferenceResult
{
    cv::Mat frame;                          /**< Quadro avaliado pela IA (copia). */
    std::vector<DetectionResult> detections;
    uint64_t sequence{0};                   /**< Cresce a cada inferencia publicada; 0 = nenhuma. */
};

/*******************************************************************************
 * PROTOTIPOS PUBLICOS
 ******************************************************************************/

/** @brief Carrega o modelo ONNX (CUDA FP16 se disponivel, senao CPU) e faz warm-up. */
bool Vision_Init(const std::string& modelPath);

/** @brief Dispara a thread de inferencia. */
void Vision_Start(void);

/** @brief Sinaliza parada e aguarda o termino da thread de inferencia. */
void Vision_Stop(void);

/** @brief Entrega uma copia do quadro para a proxima inferencia (descarta o anterior pendente). */
void Vision_SubmitFrame(const cv::Mat& frame);

/**
 * @brief Copia, de forma thread-safe, a ultima inferencia concluida (quadro + deteccoes).
 * @return false se ainda nao ha inferencia desde o inicio ou desde o ultimo Vision_Reset().
 */
bool Vision_GetLatest(InferenceResult& out);

/**
 * @brief Descarta o quadro pendente, as deteccoes publicadas e o resultado de
 *        qualquer inferencia em andamento.
 * @note  Chamado quando a varredura e pausada ou retomada, para que nenhuma deteccao
 *        de antes da pausa seja usada depois dela.
 */
void Vision_Reset(void);

#endif /* VISION_MODULE_HPP */
/** @} DOXYGEN GROUP TAG END OF FILE */
