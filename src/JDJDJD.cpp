/******************************************************************************/
/**
 * @file    Orchestrator.cpp
 * @addtogroup ORCHESTRATOR
 * @brief   Implementacao do Orquestrador: gestao do pipeline, unificacao da ROI, HUD e STM32.
 * @author  Gonçalves
 ******************************************************************************/

#include "Orchestrator.hpp"
#include "CameraModule.hpp"
#include "VisionModule.hpp"
#include "Stm32Bridge.hpp" // [STM32] 1. Inclui a biblioteca de comunicacao com o STM32
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>
#include <vector>

static const char* const dWINDOW_NAME = "SICOV - Inspecao de Formas Geometricas";
static const char* const dROI_CONFIG_FILE = "roi_config.yml";
static constexpr int dROI_MAX = 1000;
static constexpr int dCLASS_ID_CIRCULO = 0;

static int gRoiCx = 500;
static int gRoiCy = 330;
static int gRoiW  = 250;
static int gRoiH  = 250;

static std::atomic<bool> gSinalCirculo{false};

static cv::Rect ObterEnquadramentoInspecao(const cv::Size& frameSize);
static std::string ObterNomeForma(int classId);
static void CarregarROI(void);
static void SalvarROI(void);

bool Orchestrator_Init(const std::string& modelPath, const std::string& cameraSN)
{
    cv::setNumThreads(8);

    std::cout << "[ORCHESTRATOR] Inicializando subsistemas..." << std::endl;

    if (!Vision_Init(modelPath))
    {
        std::cerr << "[ORCHESTRATOR] Falha critica ao inicializar VisionModule." << std::endl;
        return false;
    }

    if (!Camera_Init(cameraSN))
    {
        std::cerr << "[ORCHESTRATOR] Falha critica ao inicializar CameraModule." << std::endl;
        return false;
    }

    // [STM32] 2. Conecta automaticamente ao microcontrolador STM32
    if (!bridge::conectar())
    {
        std::cerr << "[ORCHESTRATOR] Falha critica ao conectar ao STM32." << std::endl;
        return false;
    }
    std::cout << "[ORCHESTRATOR] STM32 conectado na porta: " << bridge::obter_porta_conectada() << std::endl;

    std::cout << "[ORCHESTRATOR] Todos os subsistemas prontos para execucao." << std::endl;
    return true;
}

void Orchestrator_Run(void)
{
    Vision_Start();
    Camera_Start();

    CarregarROI();

    cv::namedWindow(dWINDOW_NAME, cv::WINDOW_NORMAL);

    cv::createTrackbar("ROI centro X", dWINDOW_NAME, &gRoiCx, dROI_MAX);
    cv::createTrackbar("ROI centro Y", dWINDOW_NAME, &gRoiCy, dROI_MAX);
    cv::createTrackbar("ROI largura",  dWINDOW_NAME, &gRoiW,  dROI_MAX);
    cv::createTrackbar("ROI altura",   dWINDOW_NAME, &gRoiH,  dROI_MAX);

    cv::Mat frame;
    std::vector<DetectionResult> detections;
    bool estadoAnteriorSinal = false;

    std::cout << "[ORCHESTRATOR] Pipeline ativo. 's' = salvar ROI | ESC = finalizar." << std::endl;

    while (true)
    {
        // 1. Obtem o quadro gerado pela camera
        if (Camera_GetFrame(frame))
        {
            // Calcula e desenha a moldura da Zona de Inspecao (ROI)
            cv::Rect roi = ObterEnquadramentoInspecao(frame.size());
            cv::rectangle(frame, roi, cv::Scalar(0, 255, 255), 2);
            cv::putText(frame, "ZONA DE INSPECAO", cv::Point(roi.x, std::max(15, roi.y - 10)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);

            // [STM32] 3. Verifica se o STM32 liberou a varredura (true) ou mandou pausar (false)
            if (!bridge::esta_varredura_ativa())
            {
                // Se o STM32 pausou a varredura (esteira parada / servo atuando) e o sinal
                // estava ativo antes, reseta o estado de "objeto encontrado" para "sem objeto"
                if (estadoAnteriorSinal)
                {
                    std::cout << "[STM32] Varredura pausada. Resetando sinal para SEM OBJETO (0xA0)..." << std::endl;
                    bridge::enviar_sem_objeto();
                    estadoAnteriorSinal = false;
                    gSinalCirculo = false;
                }

                // Mostra aviso na tela de que o codigo esta aguardando o STM32 liberar
                cv::putText(frame, "PAUSADO PELO STM32 (AGUARDANDO 0xB1)", cv::Point(10, 30),
                            cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 165, 255), 2);
            }
            else
            {
                // Varredura ATIVA: envia o frame para a IA e busca as deteccoes normalmente
                Vision_SubmitFrame(frame);
                Vision_GetOverlay(detections);

                int totalNaZona = 0;
                bool circuloDetectadoNesteCiclo = false;

                for (const auto& det : detections)
                {
                    if (det.box.area() <= 0) continue;
                    if ((roi & det.box) != det.box) continue;

                    if (det.isValid && det.classId == dCLASS_ID_CIRCULO)
                    {
                        circuloDetectadoNesteCiclo = true;

                        cv::Scalar boxColor(0, 0, 255);
                        std::string labelText = "CIRCULO";
                        cv::rectangle(frame, det.box, boxColor, 2);
                        cv::Point textPos(det.box.x, std::max(20, det.box.y - 5));
                        cv::putText(frame, labelText, textPos, cv::FONT_HERSHEY_SIMPLEX, 0.6, boxColor, 2);
                    }
                    else if (!det.isValid)
                    {
                        cv::Scalar boxColor(0, 0, 255);
                        std::string labelText = "Nao detectada";
                        cv::rectangle(frame, det.box, boxColor, 2);
                        cv::Point textPos(det.box.x, std::max(20, det.box.y - 5));
                        cv::putText(frame, labelText, textPos, cv::FONT_HERSHEY_SIMPLEX, 0.6, boxColor, 2);
                    }

                    totalNaZona++;
                }

                // Atualiza a flag global do sinal
                gSinalCirculo = circuloDetectadoNesteCiclo;

                // [STM32] 4A. Se ACABOU de detectar um circulo (mudou de false para true)
                if (gSinalCirculo && !estadoAnteriorSinal)
                {
                    std::cout << "[SINAL] >>> CIRCULO detectado na ROI! Avisando STM32 (0xA1)... <<<" << std::endl;
                    bridge::enviar_objeto_encontrado();
                }
                // [STM32] 4B. Se o circulo SAIU da ROI (mudou de true para false)
                else if (!gSinalCirculo && estadoAnteriorSinal)
                {
                    std::cout << "[SINAL] >>> Sem circulo na ROI. Avisando STM32 (0xA0)... <<<" << std::endl;
                    bridge::enviar_sem_objeto();
                }

                estadoAnteriorSinal = gSinalCirculo;

                // HUD normal quando a varredura esta ativa
                cv::putText(frame, "Na zona: " + std::to_string(totalNaZona), cv::Point(10, 30),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);

                if (gSinalCirculo)
                {
                    cv::putText(frame, "SINAL ATIVO: CIRCULO [FLAG = 1]", cv::Point(10, 65),
                                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
                }
            }

            cv::putText(frame, "[s] salvar ROI  [ESC] sair", cv::Point(10, frame.rows - 15),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);

            cv::imshow(dWINDOW_NAME, frame);
        }

        int key = cv::waitKey(1) & 0xFF;
        if (key == 27)  { break; }
        if (key == 's') { SalvarROI(); }
    }

    Orchestrator_Stop();
}

void Orchestrator_Stop(void)
{
    std::cout << "[ORCHESTRATOR] Parando servicos..." << std::endl;
    // [STM32] 5. Desconecta do STM32 com seguranca ao fechar o programa
    bridge::desconectar();
    Camera_Stop();
    Vision_Stop();
    cv::destroyAllWindows();
    std::cout << "[ORCHESTRATOR] Sistema encerrado com seguranca." << std::endl;
}

static void CarregarROI(void)
{
    cv::FileStorage fs(dROI_CONFIG_FILE, cv::FileStorage::READ);
    if (!fs.isOpened()) return;

    auto ler = [&fs](const char* chave, int& destino)
    {
        cv::FileNode node = fs[chave];
        if (!node.empty())
        {
            int v = 0;
            node >> v;
            destino = std::min(std::max(v, 0), dROI_MAX);
        }
    };

    ler("cx", gRoiCx);
    ler("cy", gRoiCy);
    ler("w",  gRoiW);
    ler("h",  gRoiH);

    std::cout << "[ORCHESTRATOR] ROI carregada de " << dROI_CONFIG_FILE << std::endl;
}

static void SalvarROI(void)
{
    cv::FileStorage fs(dROI_CONFIG_FILE, cv::FileStorage::WRITE);
    if (!fs.isOpened())
    {
        std::cerr << "[ORCHESTRATOR] Nao foi possivel gravar " << dROI_CONFIG_FILE << std::endl;
        return;
    }

    fs << "cx" << gRoiCx << "cy" << gRoiCy << "w" << gRoiW << "h" << gRoiH;

    std::cout << "[ORCHESTRATOR] ROI salva: cx=" << gRoiCx << " cy=" << gRoiCy
              << " w=" << gRoiW << " h=" << gRoiH << std::endl;
}

static cv::Rect ObterEnquadramentoInspecao(const cv::Size& frameSize)
{
    int w = std::max(1, frameSize.width  * gRoiW / dROI_MAX);
    int h = std::max(1, frameSize.height * gRoiH / dROI_MAX);

    int x = frameSize.width  * gRoiCx / dROI_MAX - w / 2;
    int y = frameSize.height * gRoiCy / dROI_MAX - h / 2;

    return cv::Rect(x, y, w, h) & cv::Rect(0, 0, frameSize.width, frameSize.height);
}

static std::string ObterNomeForma(int classId)
{
    switch (classId)
    {
        case 0: return "CIRCULO";
        case 1: return "Hexagono";
        case 2: return "Losango";
        case 3: return "Quadrado";
        case 4: return "Retangulo";
        case 5: return "Triangulo";
        default: return "Nao detectada";
    }
}
