/******************************************************************************/
/**
 * @file    VisionModule.cpp
 * @addtogroup VISION
 * @brief   Implementacao do pipeline de IA desacoplado (Consumidor).
 * @author  Gonçalves
 * @author  Luiz Neto
 * @details
 * \n <b>Hardware:</b> NVIDIA GeForce RTX 4050 (Aceleração CUDA FP16).
 * \n Eliminada qualquer duplicacao de ROI: O modulo processa o frame completo.
 *
 * @{
 ******************************************************************************/

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include "VisionModule.hpp"
#include <opencv2/core/cuda.hpp>
#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

/*******************************************************************************
 * CONSTANTES LOCAIS
 ******************************************************************************/
static constexpr int dINPUT_SIZE = 640;
static constexpr float dCONFIDENCE_THRESHOLD = 0.20f;
static constexpr float dNMS_THRESHOLD        = 0.60f;

// Identificadores mapeados conforme data.yaml
static constexpr int dCLASS_ID_CIRCULO   = 0;
static constexpr int dCLASS_ID_HEXAGONO  = 1;
static constexpr int dCLASS_ID_LOSANGO   = 2;
static constexpr int dCLASS_ID_QUADRADO  = 3;
static constexpr int dCLASS_ID_RETANGULO = 4;
static constexpr int dCLASS_ID_TRIANGULO = 5;

/*******************************************************************************
 * ESTRUTURAS DE DADOS LOCAIS
 ******************************************************************************/
static struct
{
    cv::dnn::Net net;
    cv::Mat inputFrame;
    std::mutex inputMutex;
    std::condition_variable inputCv;
    bool hasNewFrame{false};

    std::vector<DetectionResult> results;
    std::mutex resultMutex;

    std::atomic<bool> isRunning{false};
    std::thread visionThread;
} visionCtx;

/*******************************************************************************
 * PROTOTIPOS LOCAIS
 ******************************************************************************/
static void RunInference(const cv::Mat& frame, std::vector<DetectionResult>& outDetections);
static void VisionLoop(void);

/*******************************************************************************
 * FUNCOES PUBLICAS
 ******************************************************************************/

bool Vision_Init(const std::string& modelPath)
{
    try
    {
        // O OpenCV pre-compilado para Windows nao tem CUDA: sem esta checagem o
        // backend cairia para CPU em silencio e o log abaixo afirmaria CUDA ativo.
        if (cv::cuda::getCudaEnabledDeviceCount() <= 0)
        {
            CV_Error(cv::Error::GpuNotSupported, "OpenCV sem suporte a CUDA ou nenhuma GPU encontrada");
        }

        visionCtx.net = cv::dnn::readNetFromONNX(modelPath);
        visionCtx.net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
        visionCtx.net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA_FP16);

        // Warm-up na RTX 4050 para compilar grafos nos nucleos Tensor
        cv::Mat dummy = cv::Mat::zeros(dINPUT_SIZE, dINPUT_SIZE, CV_8UC3);
        cv::Mat blob;
        cv::dnn::blobFromImage(dummy, blob, 1.0 / 255.0, cv::Size(dINPUT_SIZE, dINPUT_SIZE), cv::Scalar(), true, false);
        visionCtx.net.setInput(blob);
        std::vector<cv::Mat> warmupOut;
        visionCtx.net.forward(warmupOut, visionCtx.net.getUnconnectedOutLayersNames());

        std::cout << "[VISION] CUDA inicializado com sucesso na RTX 4050." << std::endl;
        return true;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[VISION] CUDA indisponivel (" << e.what() << "). Fallback CPU..." << std::endl;
        try
        {
            visionCtx.net = cv::dnn::readNetFromONNX(modelPath);
            visionCtx.net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            visionCtx.net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            std::cout << "[VISION] Inferencia em CPU." << std::endl;
            return true;
        }
        catch (const cv::Exception& ex)
        {
            std::cerr << "[VISION] Falha critica ao carregar ONNX: " << ex.what() << std::endl;
            return false;
        }
    }
}

void Vision_Start(void)
{
    visionCtx.isRunning = true;
    visionCtx.visionThread = std::thread(VisionLoop);
}

void Vision_Stop(void)
{
    visionCtx.isRunning = false;
    visionCtx.inputCv.notify_all();
    if (visionCtx.visionThread.joinable())
    {
        visionCtx.visionThread.join();
    }
}

void Vision_SubmitFrame(const cv::Mat& frame)
{
    std::lock_guard<std::mutex> lock(visionCtx.inputMutex);
    frame.copyTo(visionCtx.inputFrame);
    visionCtx.hasNewFrame = true;
    visionCtx.inputCv.notify_one();
}

void Vision_GetOverlay(std::vector<DetectionResult>& outDetections)
{
    std::lock_guard<std::mutex> lock(visionCtx.resultMutex);
    outDetections = visionCtx.results;
}

/*******************************************************************************
 * FUNCOES LOCAIS
 ******************************************************************************/

static void RunInference(const cv::Mat& frame, std::vector<DetectionResult>& outDetections)
{
    // 1. Prepara o tensor de entrada (Blob normalizado 640x640)
    cv::Mat blob;
    cv::dnn::blobFromImage(frame, blob, 1.0 / 255.0, cv::Size(dINPUT_SIZE, dINPUT_SIZE), cv::Scalar(), true, false);
    visionCtx.net.setInput(blob);

    // 2. Executa a inferencia na GPU RTX 4050
    std::vector<cv::Mat> outputs;
    visionCtx.net.forward(outputs, visionCtx.net.getUnconnectedOutLayersNames());

    cv::Mat raw = outputs[0];
    int numAttrs = (raw.dims == 3) ? raw.size[1] : raw.size[0];
    int numBoxes = (raw.dims == 3) ? raw.size[2] : raw.size[1];

    cv::Mat outMat(numAttrs, numBoxes, CV_32F, raw.ptr<float>());
    cv::Mat predictions = outMat.t();

    std::vector<float> confidences;
    std::vector<cv::Rect> boxes;
    std::vector<int> classIds;
    std::vector<bool> validity;

    float x_factor = frame.cols / static_cast<float>(dINPUT_SIZE);
    float y_factor = frame.rows / static_cast<float>(dINPUT_SIZE);
    int numClasses = numAttrs - 4;

    // 3. Varre e decodifica os tensores de saida do YOLO
    for (int i = 0; i < predictions.rows; ++i)
    {
        float* row = predictions.ptr<float>(i);
        float* classesScores = row + 4;

        cv::Mat scores(1, numClasses, CV_32F, classesScores);
        cv::Point classIdPoint;
        double maxScore;
        cv::minMaxLoc(scores, 0, &maxScore, 0, &classIdPoint);

        if (maxScore > dCONFIDENCE_THRESHOLD)
        {
            int classId = classIdPoint.x;

            float cx = row[0] * x_factor;
            float cy = row[1] * y_factor;
            float w  = row[2] * x_factor;
            float h  = row[3] * y_factor;

            int left = static_cast<int>(cx - w / 2.0f);
            int top  = static_cast<int>(cy - h / 2.0f);

            bool isValid = (classId >= dCLASS_ID_CIRCULO && classId <= dCLASS_ID_TRIANGULO);

            boxes.push_back(cv::Rect(left, top, static_cast<int>(w), static_cast<int>(h)));
            confidences.push_back(static_cast<float>(maxScore));
            classIds.push_back(classId);
            validity.push_back(isValid);
        }
    }

    // 4. TRUQUE DO OFFSET: Torna o NMS sensivel a cada classe
    // Ao deslocar virtualmente as coordenadas de acordo com o classId, caixas de classes
    // diferentes (ex: Circulo ao lado de Quadrado) nunca irao se anular entre si.
    std::vector<cv::Rect> offsetBoxes;
    offsetBoxes.reserve(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        int offset = classIds[i] * 4096;
        offsetBoxes.push_back(cv::Rect(boxes[i].x + offset, boxes[i].y + offset, boxes[i].width, boxes[i].height));
    }

    std::vector<int> indices;
    cv::dnn::NMSBoxes(offsetBoxes, confidences, dCONFIDENCE_THRESHOLD, dNMS_THRESHOLD, indices);

    // 5. Salva as deteccoes finais preservando as coordenadas reais da tela
    outDetections.clear();
    for (int idx : indices)
    {
        outDetections.push_back({boxes[idx], classIds[idx], confidences[idx], validity[idx]});
    }
}

static void VisionLoop(void)
{
    while (visionCtx.isRunning)
    {
        cv::Mat frame;
        {
            std::unique_lock<std::mutex> lock(visionCtx.inputMutex);
            visionCtx.inputCv.wait(lock, [] { return visionCtx.hasNewFrame || !visionCtx.isRunning; });

            if (!visionCtx.isRunning) break;

            frame = visionCtx.inputFrame.clone();
            visionCtx.hasNewFrame = false;
        }

        std::vector<DetectionResult> detections;
        RunInference(frame, detections);

        {
            std::lock_guard<std::mutex> lock(visionCtx.resultMutex);
            visionCtx.results = std::move(detections);
        }
    }
}

/** @} DOXYGEN GROUP TAG END OF FILE */