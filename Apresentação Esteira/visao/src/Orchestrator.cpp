/******************************************************************************/
/**
 * @file    Orchestrator.cpp
 * @addtogroup ORCHESTRATOR
 * @brief   Implementacao do Orquestrador: gestao do pipeline, unificacao da ROI, HUD e STM32.
 * @author  Gonçalves
 * @author  Luiz Neto
 * @details
 * \n <b>Regra de Arquitetura:</b> Fonte Unica da Verdade (Single Source of Truth).
 * \n O enquadramento (ROI) e calculado exclusivamente aqui, a partir das variaveis
 *    gRoiCx/gRoiCy/gRoiW/gRoiH. O mesmo retangulo e usado para desenhar a moldura
 *    e para filtrar as deteccoes.
 * \n <b>Calibracao:</b> a ROI e ajustada ao vivo por trackbars e salva em arquivo
 *    (tecla 's'). Na proxima execucao o arquivo e carregado automaticamente.
 * \n <b>Regra de validade:</b> so e reconhecida/contada a forma cuja caixa esta
 *    TOTALMENTE dentro da ROI. Parcialmente fora = ignorada.
 * \n <b>Integracao STM32:</b>
 *    - Varredura pausada pelo STM32 (0xB0): a IA nao recebe quadros e, se havia
 *      circulo sinalizado, o sinal e resetado com 0xA0.
 *    - Varredura ativa (0xB1): na borda de subida do circulo na ROI envia 0xA1;
 *      na borda de descida envia 0xA0.
 *
 * @{
 ******************************************************************************/

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include "Orchestrator.hpp"
#include "CameraModule.hpp"
#include "VisionModule.hpp"
#include "Stm32Bridge.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>
#include <vector>

/*******************************************************************************
 * CONSTANTES DE CONFIGURACAO
 ******************************************************************************/

/** @brief Nome da janela (usado na janela e nas trackbars; deve ser identico). */
static const char* const dWINDOW_NAME = "SICOV - Inspecao de Formas Geometricas";

/** @brief Arquivo onde a calibracao da ROI e persistida (pasta de execucao). */
static const char* const dROI_CONFIG_FILE = "roi_config.yml";

/** @brief Escala das trackbars: 0..1000 (permil) do tamanho do frame. */
static constexpr int dROI_MAX = 1000;

/** @brief Identificador da classe Circulo no modelo YOLO (data.yaml). */
static constexpr int dCLASS_ID_CIRCULO = 0;

/*******************************************************************************
 * VARIAVEIS DA ROI E SINAIS DE CONTROLE
 ******************************************************************************/

/* Em permil (1/1000) do frame, para funcionar em qualquer resolucao de camera.
 * Precisam ser 'int' simples e nao-const: o OpenCV escreve nelas via trackbar. */
static int gRoiCx = 500;   /**< Centro X da ROI (500 = meio da largura).        */
static int gRoiCy = 330;   /**< Centro Y da ROI (valor inicial aproximado).     */
static int gRoiW  = 250;   /**< Largura da ROI (250 = 25% do frame).            */
static int gRoiH  = 250;   /**< Altura da ROI (250 = 25% do frame).             */

/** @brief Sinal global ativo enquanto um CIRCULO e reconhecido na ROI. */
static std::atomic<bool> gSinalCirculo{false};

/*******************************************************************************
 * PROTOTIPOS LOCAIS
 ******************************************************************************/
static cv::Rect ObterEnquadramentoInspecao(const cv::Size& frameSize);
static void DesenharDeteccao(cv::Mat& frame, const cv::Rect& box, const std::string& label);
static void CarregarROI(void);
static void SalvarROI(void);

/*******************************************************************************
 * FUNCOES PUBLICAS
 ******************************************************************************/

/******************************************************************************/
/**
 * @brief  Realiza a inicializacao coordenada dos modulos de Visao, Camera e STM32.
 * @param  modelPath Caminho para o arquivo ONNX treinado.
 * @param  cameraSN Numero de serie opcional da camera.
 * @param  serialPort Porta serial do STM32; vazio = auto-deteccao.
 * @return true se todos os modulos responderem com sucesso; false caso contrario.
 ******************************************************************************/
bool Orchestrator_Init(const std::string& modelPath, const std::string& cameraSN,
                       const std::string& serialPort)
{
    // Habilita processamento multi-core paralelo para as rotinas graficas do OpenCV
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

    if (!bridge::conectar(serialPort))
    {
        std::cerr << "[ORCHESTRATOR] Falha critica ao conectar ao STM32." << std::endl;
        Camera_Stop();
        return false;
    }
    std::cout << "[ORCHESTRATOR] STM32 conectado na porta: " << bridge::obter_porta_conectada() << std::endl;

    std::cout << "[ORCHESTRATOR] Todos os subsistemas prontos para execucao." << std::endl;
    return true;
}

/******************************************************************************/
/**
 * @brief  Loop principal de orquestracao e renderizacao do HUD na tela.
 * @return void
 * @details Executa os seguintes passos a cada ciclo:
 * \n 1. Recupera o quadro mais recente da camera industrial e desenha a ROI.
 * \n 2. Se o STM32 pausou a varredura: nao envia o quadro para a IA e reseta o sinal.
 * \n 3. Se a varredura esta ativa: envia o quadro para a IA e coleta as deteccoes.
 * \n 4. Avalia cada objeto (so conta se a caixa estiver INTEIRA na zona):
 *       - Vermelho + "CIRCULO": circulo reconhecido, aciona o sinal.
 *       - Vermelho + "Nao detectada": anomalia / forma desconhecida.
 *       - Outras formas: so contam em "Na zona", sem desenho.
 * \n 5. Envia 0xA1/0xA0 ao STM32 nas bordas do sinal e atualiza o HUD.
 * \n Teclas: 's' salva a calibracao da ROI, ESC encerra.
 ******************************************************************************/
void Orchestrator_Run(void)
{
    Vision_Start();
    Camera_Start();

    // Carrega ANTES de criar as trackbars, para elas ja nascerem na posicao salva
    CarregarROI();

    cv::namedWindow(dWINDOW_NAME, cv::WINDOW_NORMAL);

    // Trackbars ligadas diretamente as variaveis globais da ROI
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
            // Copia limpa para a IA, antes de qualquer desenho do HUD
            const cv::Mat frameIA = frame.clone();

            // Calcula e desenha a moldura da Zona de Inspecao (Amarelo / BGR: 0, 255, 255)
            cv::Rect roi = ObterEnquadramentoInspecao(frame.size());
            cv::rectangle(frame, roi, cv::Scalar(0, 255, 255), 2);
            cv::putText(frame, "ZONA DE INSPECAO", cv::Point(roi.x, std::max(15, roi.y - 10)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);

            // 2. Varredura pausada pelo STM32 (esteira parada / servo atuando)
            if (!bridge::esta_varredura_ativa())
            {
                // Se o sinal estava ativo, devolve o pino foundObject para "sem objeto"
                if (estadoAnteriorSinal)
                {
                    std::cout << "[STM32] Varredura pausada. Resetando sinal para SEM OBJETO (0xA0)..." << std::endl;
                    bridge::enviar_sem_objeto();
                    estadoAnteriorSinal = false;
                    gSinalCirculo = false;
                }

                cv::putText(frame, "PAUSADO PELO STM32 (AGUARDANDO 0xB1)", cv::Point(10, 30),
                            cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 165, 255), 2);
            }
            else
            {
                // 3. Varredura ATIVA: envia o frame para a IA e busca as deteccoes
                Vision_SubmitFrame(frameIA);
                Vision_GetOverlay(detections);

                // 4. Avalia e renderiza as deteccoes
                int totalNaZona = 0;
                bool circuloDetectadoNesteCiclo = false;

                for (const auto& det : detections)
                {
                    // Caixa degenerada nao e uma deteccao utilizavel
                    if (det.box.area() <= 0) continue;

                    // Regra de enquadramento: a caixa INTEIRA precisa estar dentro da ROI
                    if ((roi & det.box) != det.box) continue;

                    if (det.isValid && det.classId == dCLASS_ID_CIRCULO)
                    {
                        circuloDetectadoNesteCiclo = true;
                        DesenharDeteccao(frame, det.box, "CIRCULO");
                    }
                    else if (!det.isValid)
                    {
                        DesenharDeteccao(frame, det.box, "Nao detectada");
                    }

                    totalNaZona++;
                }

                gSinalCirculo = circuloDetectadoNesteCiclo;

                // 5A. Borda de subida: circulo entrou na ROI
                if (gSinalCirculo && !estadoAnteriorSinal)
                {
                    std::cout << "[SINAL] >>> CIRCULO detectado na ROI! Avisando STM32 (0xA1)... <<<" << std::endl;
                    bridge::enviar_objeto_encontrado();
                }
                // 5B. Borda de descida: circulo saiu da ROI
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

        // Aguarda evento de teclado (1 ms). '& 0xFF' normaliza o retorno entre backends
        // (alguns devolvem bits extras de modificadores); sem tecla vira 255 e nao casa.
        int key = cv::waitKey(1) & 0xFF;
        if (key == 27)  { break; }        // ESC encerra
        if (key == 's') { SalvarROI(); }  // 's' persiste a calibracao
    }

    Orchestrator_Stop();
}

/******************************************************************************/
/**
 * @brief  Finaliza todas as threads ativas, a conexao com o STM32 e as janelas.
 * @return void
 ******************************************************************************/
void Orchestrator_Stop(void)
{
    std::cout << "[ORCHESTRATOR] Parando servicos..." << std::endl;
    bridge::desconectar();
    Camera_Stop();
    Vision_Stop();
    cv::destroyAllWindows();
    std::cout << "[ORCHESTRATOR] Sistema encerrado com seguranca." << std::endl;
}

/*******************************************************************************
 * FUNCOES LOCAIS
 ******************************************************************************/

/******************************************************************************/
/**
 * @brief  Desenha a caixa e o rotulo de uma deteccao em vermelho (BGR: 0, 0, 255).
 * @param  frame Imagem de saida do HUD.
 * @param  box Caixa da deteccao em pixels.
 * @param  label Texto exibido acima da caixa.
 * @return void
 ******************************************************************************/
static void DesenharDeteccao(cv::Mat& frame, const cv::Rect& box, const std::string& label)
{
    const cv::Scalar cor(0, 0, 255);
    cv::rectangle(frame, box, cor, 2);
    cv::Point textPos(box.x, std::max(20, box.y - 5));
    cv::putText(frame, label, textPos, cv::FONT_HERSHEY_SIMPLEX, 0.6, cor, 2);
}

/******************************************************************************/
/**
 * @brief  Le a calibracao da ROI do arquivo, se ele existir.
 * @return void
 * @details Se o arquivo nao existe (primeira execucao), mantem os valores padrao.
 *          Campos ausentes tambem mantem o padrao, e os valores lidos sao limitados
 *          a faixa 0..1000 para um arquivo editado a mao nao quebrar a ROI.
 ******************************************************************************/
static void CarregarROI(void)
{
    cv::FileStorage fs(dROI_CONFIG_FILE, cv::FileStorage::READ);
    if (!fs.isOpened()) return;

    // 'node >> int' grava 0 quando o campo nao existe; por isso so le se o no existir
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

/******************************************************************************/
/**
 * @brief  Salva a calibracao atual da ROI em arquivo YAML.
 * @return void
 ******************************************************************************/
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

/******************************************************************************/
/**
 * @brief  Converte a ROI calibrada (permil) em um retangulo em pixels do frame.
 * @param  frameSize Largura e altura nativas da imagem da camera.
 * @return cv::Rect Retangulo da Zona de Inspecao, sempre contido no frame.
 * @details x = largura * cx/1000 - w/2 (idem para y). Como tudo e proporcional ao
 *          frame, a calibracao continua valida se a resolucao da camera mudar.
 ******************************************************************************/
static cv::Rect ObterEnquadramentoInspecao(const cv::Size& frameSize)
{
    // Tamanho minimo de 1 px: com a trackbar em 0 o retangulo ficaria vazio
    int w = std::max(1, frameSize.width  * gRoiW / dROI_MAX);
    int h = std::max(1, frameSize.height * gRoiH / dROI_MAX);

    // O ponto de calibracao e o CENTRO da ROI; converte para o canto superior esquerdo
    int x = frameSize.width  * gRoiCx / dROI_MAX - w / 2;
    int y = frameSize.height * gRoiCy / dROI_MAX - h / 2;

    // Interseccao com o frame: impede ROI saindo da imagem
    return cv::Rect(x, y, w, h) & cv::Rect(0, 0, frameSize.width, frameSize.height);
}

/** @} DOXYGEN GROUP TAG END OF FILE */
