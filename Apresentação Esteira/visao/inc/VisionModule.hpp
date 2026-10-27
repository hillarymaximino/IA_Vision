/******************************************************************************/
/**
 * @file    VisionModule.hpp
 * @addtogroup VISION
 * @brief   Interface do pipeline de IA desacoplado (Consumidor).
 * @author  Gonçalves
 * @details
 * \n <b>Grupo de Pesquisa:</b> ASSERT - IFPB
 * \n <b>Projeto:</b> SICOV
 *
 * Executa o YOLO11 (ONNX) em uma thread propria. O Orchestrator submete quadros
 * com Vision_SubmitFrame() e le o resultado mais recente com Vision_GetOverlay().
 *
 * @{
 ******************************************************************************/

#ifndef VISION_MODULE_HPP
#define VISION_MODULE_HPP

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include <opencv2/opencv.hpp>
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

/** @brief Copia, de forma thread-safe, as deteccoes da ultima inferencia concluida. */
void Vision_GetOverlay(std::vector<DetectionResult>& outDetections);

#endif /* VISION_MODULE_HPP */
/** @} DOXYGEN GROUP TAG END OF FILE */
