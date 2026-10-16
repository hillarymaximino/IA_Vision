/******************************************************************************/
/**
 * @file    CameraModule.cpp
 * @addtogroup CAMERA
 * @brief   Implementacao nativa Linux do modulo de captura da camera Daheng MER2-302-37GM-P.
 * @author  Gonçalves
 * @details
 * \n <b>Grupo de Pesquisa:</b> ASSERT - IFPB
 * \n <b>Protocolo:</b> GigE Vision via Galaxy SDK nativo (GxIAPI Linux).
 * \n <b>Ajustes:</b> Pacotes em 1500 bytes (MTU padrao), Trigger OFF e Modo Continuo.
 *
 * @{
 ******************************************************************************/

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include "CameraModule.hpp"
#include "GxIAPI.h"
#include <iostream>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

/*******************************************************************************
 * ESTRUTURAS DE DADOS LOCAIS
 ******************************************************************************/
static struct
{
    GX_DEV_HANDLE hDevice{nullptr};         /**< Manipulador do dispositivo aberto no SDK. */
    std::atomic<bool> isCapturing{false};   /**< Flag atomica do laco da thread produtora. */
    std::thread captureThread;              /**< Thread responsavel pela aquisicao de imagens. */

    cv::Mat currentFrame;                   /**< Buffer interno protegido com o ultimo frame. */
    std::mutex frameMutex;                  /**< Mutex para proteger o acesso concorrente. */
    bool hasFrame{false};                   /**< Flag indicadora de frame disponivel. */
} camCtx;

/*******************************************************************************
 * PROTOTIPOS LOCAIS
 ******************************************************************************/
static void CameraCaptureLoop(void);

/*******************************************************************************
 * FUNCOES PUBLICAS
 ******************************************************************************/

bool Camera_Init(const std::string& cameraSN)
{
    // 1. Inicializa o Galaxy SDK
    GX_STATUS status = GXInitLib();
    if (status != GX_STATUS_SUCCESS)
    {
        std::cerr << "[CAMERA] Erro ao inicializar GXInitLib (Status: " << status << ")" << std::endl;
        return false;
    }

    // 2. Enumera a rede para localizar as cameras conectadas
    uint32_t nDevNum = 0;
    status = GXUpdateDeviceList(&nDevNum, 1000);
    if (status != GX_STATUS_SUCCESS || nDevNum <= 0)
    {
        std::cerr << "[CAMERA] Erro: Nenhuma camera Daheng encontrada no switch." << std::endl;
        GXCloseLib();
        return false;
    }

    // 3. Abre comunicacao exclusiva com a camera
    GX_OPEN_PARAM stOpenParam;
    stOpenParam.accessMode = GX_ACCESS_EXCLUSIVE;

    if (cameraSN.empty())
    {
        stOpenParam.openMode = GX_OPEN_INDEX;
        stOpenParam.pszContent = const_cast<char*>("1");
    }
    else
    {
        stOpenParam.openMode = GX_OPEN_SN;
        stOpenParam.pszContent = const_cast<char*>(cameraSN.c_str());
    }

    status = GXOpenDevice(&stOpenParam, &camCtx.hDevice);
    if (status != GX_STATUS_SUCCESS)
    {
        std::cerr << "[CAMERA] Erro ao abrir a camera Daheng (Status: " << status << ")" << std::endl;
        GXCloseLib();
        return false;
    }

    // 4. Pacote universal de 1500 bytes (compativel com qualquer switch)
    GXSetInt(camCtx.hDevice, GX_INT_GEV_PACKETSIZE, 1500);

    // 5. Desativa o modo de disparo externo (Trigger) para transmissao livre
    GXSetEnum(camCtx.hDevice, GX_ENUM_TRIGGER_MODE, GX_TRIGGER_MODE_OFF);

    // 6. Forca modo de aquisicao continua
    GXSetEnum(camCtx.hDevice, GX_ENUM_ACQUISITION_MODE, GX_ACQ_MODE_CONTINUOUS);

    // 7. Configura tempo de exposicao inicial razoavel (20 ms = 20000 us)
    GXSetFloat(camCtx.hDevice, GX_FLOAT_EXPOSURE_TIME, 30000.0);
    GXSetEnum(camCtx.hDevice, GX_ENUM_GAIN_AUTO, GX_GAIN_AUTO_CONTINUOUS);

    // 8. Inicia a transmissao de rede (StreamOn)
    status = GXStreamOn(camCtx.hDevice);
    if (status != GX_STATUS_SUCCESS)
    {
        std::cerr << "[CAMERA] Erro no GXStreamOn (Status: " << status << ")" << std::endl;
        GXCloseDevice(camCtx.hDevice);
        camCtx.hDevice = nullptr;
        GXCloseLib();
        return false;
    }

    // 9. Aciona o sensor da camera
    GXSendCommand(camCtx.hDevice, GX_COMMAND_ACQUISITION_START);

    std::cout << "[CAMERA] MER2-302-37GM-P online e configurada em modo continuo." << std::endl;
    return true;
}

void Camera_Start(void)
{
    camCtx.isCapturing = true;
    camCtx.captureThread = std::thread(CameraCaptureLoop);
}

void Camera_Stop(void)
{
    camCtx.isCapturing = false;
    if (camCtx.captureThread.joinable())
    {
        camCtx.captureThread.join();
    }

    if (camCtx.hDevice != nullptr)
    {
        GXSendCommand(camCtx.hDevice, GX_COMMAND_ACQUISITION_STOP);
        GXStreamOff(camCtx.hDevice);
        GXCloseDevice(camCtx.hDevice);
        camCtx.hDevice = nullptr;
    }

    GXCloseLib();
}

bool Camera_GetFrame(cv::Mat& outFrame)
{
    std::lock_guard<std::mutex> lock(camCtx.frameMutex);
    if (!camCtx.hasFrame) return false;

    outFrame = camCtx.currentFrame.clone();
    camCtx.hasFrame = false;
    return true;
}

static void CameraCaptureLoop(void)
{
    int64_t nPayloadSize = 0;
    GX_STATUS status = GXGetInt(camCtx.hDevice, GX_INT_PAYLOAD_SIZE, &nPayloadSize);
    if (status != GX_STATUS_SUCCESS || nPayloadSize <= 0)
    {
        std::cerr << "[CAMERA] Erro ao obter PayloadSize." << std::endl;
        return;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(nPayloadSize));
    GX_FRAME_DATA frameData;
    frameData.pImgBuf = buffer.data();

    int falhasConsecutivas = 0;

    while (camCtx.isCapturing)
    {
        status = GXGetImage(camCtx.hDevice, &frameData, 500);

        if (status == GX_STATUS_SUCCESS && frameData.nStatus == GX_FRAME_STATUS_SUCCESS)
        {
            falhasConsecutivas = 0;

            int width  = frameData.nWidth;
            int height = frameData.nHeight;

            cv::Mat frameMono(height, width, CV_8UC1, frameData.pImgBuf);
            cv::Mat frameBGR;
            cv::cvtColor(frameMono, frameBGR, cv::COLOR_GRAY2BGR);

            {
                std::lock_guard<std::mutex> lock(camCtx.frameMutex);
                camCtx.currentFrame = frameBGR.clone();
                camCtx.hasFrame = true;
            }
        }
        else
        {
            falhasConsecutivas++;
            if (falhasConsecutivas % 10 == 1) // Mostra aviso a cada intervalo para nao sobrecarregar o log
            {
                std::cerr << "[CAMERA] Aguardando quadros... (status: " << status 
                          << ", frameStatus: " << frameData.nStatus << ")" << std::endl;
            }
        }
    }
}

/** @} DOXYGEN GROUP TAG END OF FILE */