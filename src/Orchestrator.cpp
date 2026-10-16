/******************************************************************************/
/**
 * @file    Orchestrator.cpp
 * @addtogroup ORCHESTRATOR
 * @brief   Implementacao do Orquestrador: gestao do pipeline, unificacao da ROI e HUD.
 * @author  Gonçalves
 * @details
 * \n <b>Regra de Arquitetura:</b> Fonte Unica da Verdade (Single Source of Truth).
 * \n O enquadramento (ROI) e calculado exclusivamente aqui, a partir das variaveis
 *    gRoiCx/gRoiCy/gRoiW/gRoiH. O mesmo retangulo e usado para desenhar a moldura
 *    e para filtrar as deteccoes.
 * \n <b>Calibracao:</b> a ROI e ajustada ao vivo por trackbars e salva em arquivo
 *    (tecla 's'). Na proxima execucao o arquivo e carregado automaticamente.
 * \n <b>Regra de validade:</b> so e reconhecida/contada a forma cuja caixa esta
 *    TOTALMENTE dentro da ROI. Parcialmente fora = ignorada.
 *
 * @{
 ******************************************************************************/

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include "Orchestrator.hpp"
#include "CameraModule.hpp"
#include "VisionModule.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
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

/*******************************************************************************
 * VARIAVEIS DA ROI (AJUSTAVEIS EM TEMPO DE EXECUCAO)
 ******************************************************************************/

/* Em permil (1/1000) do frame, para funcionar em qualquer resolucao de camera.
 * Precisam ser 'int' simples e nao-const: o OpenCV escreve nelas via trackbar. */
static int gRoiCx = 500;   /**< Centro X da ROI (500 = meio da largura).        */
static int gRoiCy = 330;   /**< Centro Y da ROI (valor inicial aproximado).     */
static int gRoiW  = 250;   /**< Largura da ROI (250 = 25% do frame).            */
static int gRoiH  = 250;   /**< Altura da ROI (250 = 25% do frame).             */

/*******************************************************************************
 * PROTOTIPOS LOCAIS
 ******************************************************************************/
static cv::Rect ObterEnquadramentoInspecao(const cv::Size& frameSize);
static std::string ObterNomeForma(int classId);
static void CarregarROI(void);
static void SalvarROI(void);

/*******************************************************************************
 * FUNCOES PUBLICAS
 ******************************************************************************/

/******************************************************************************/
/**
 * @brief  Realiza a inicializacao coordenada dos modulos de Visao e Camera.
 * @param  modelPath Caminho para o arquivo ONNX treinado.
 * @param  cameraSN Numero de serie opcional da camera.
 * @return true se ambos os modulos responderem com sucesso; false caso contrario.
 ******************************************************************************/
bool Orchestrator_Init(const std::string& modelPath, const std::string& cameraSN)
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

    std::cout << "[ORCHESTRATOR] Todos os subsistemas prontos para execucao." << std::endl;
    return true;
}

/******************************************************************************/
/**
 * @brief  Loop principal de orquestracao e renderizacao do HUD na tela.
 * @return void
 * @details Executa os seguintes passos a cada ciclo:
 * \n 1. Recupera o quadro mais recente da camera industrial.
 * \n 2. Envia o quadro para a thread da IA (VisionModule).
 * \n 3. Recupera as caixas detectadas na ultima inferencia disponivel.
 * \n 4. Calcula a Zona de Inspecao (calibravel por trackbars).
 * \n 5. Avalia cada objeto: so considera se a caixa estiver INTEIRA na zona:
 *       - Verde + Nome da Forma: para formas treinadas e reconhecidas.
 *       - Vermelho + "Nao detectada": para anomalias ou formas nao cadastradas.
 * \n 6. Mostra no HUD quantas formas estao dentro da zona.
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

    std::cout << "[ORCHESTRATOR] Pipeline ativo. 's' = salvar ROI | ESC = finalizar." << std::endl;

    while (true)
    {
        // 1. Obtem o quadro gerado pelo produtor
        if (Camera_GetFrame(frame))
        {
            // 2. Submete o frame para analise em background
            //    (a IA recebe uma COPIA limpa, antes de qualquer desenho do HUD)
            Vision_SubmitFrame(frame);

            // 3. Coleta os ultimos resultados produzidos pela IA
            Vision_GetOverlay(detections);

            // 4. Calcula as coordenadas da Zona de Inspecao
            cv::Rect roi = ObterEnquadramentoInspecao(frame.size());

            // Desenha a moldura da Zona de Inspecao na tela (Amarelo / BGR: 0, 255, 255)
            cv::rectangle(frame, roi, cv::Scalar(0, 255, 255), 2);
            cv::putText(frame, "ZONA DE INSPECAO", cv::Point(roi.x, std::max(15, roi.y - 10)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);

            // 5. Avalia e renderiza as deteccoes
            int totalNaZona = 0;

            for (const auto& det : detections)
            {
                // Caixa degenerada nao e uma deteccao utilizavel
                if (det.box.area() <= 0) continue;

                // Regra de enquadramento: a caixa INTEIRA precisa estar dentro da ROI.
                // (roi & box) e a intersecao; so e igual a propria caixa se nenhum
                // pixel dela ficar para fora da ROI.
                if ((roi & det.box) != det.box) continue;

                cv::Scalar boxColor;
                std::string labelText;

                if (det.isValid)
                {
                    // Forma valida: Verde puro (BGR: 0, 255, 0)
                    boxColor = cv::Scalar(0, 255, 0);
                    labelText = ObterNomeForma(det.classId);
                }
                else
                {
                    // Anomalia / Fora do catalogo: Vermelho puro (BGR: 0, 0, 255)
                    boxColor = cv::Scalar(0, 0, 255);
                    labelText = "Nao detectada";
                }

                // Renderiza o retangulo de contorno da forma geometrica
                cv::rectangle(frame, det.box, boxColor, 2);

                // Renderiza o texto informativo sobre a caixa
                cv::Point textPos(det.box.x, std::max(20, det.box.y - 5));
                cv::putText(frame, labelText, textPos, cv::FONT_HERSHEY_SIMPLEX, 0.6, boxColor, 2);

                totalNaZona++;
            }

            // 6. HUD: contagem instantanea de formas dentro da zona
            cv::putText(frame, "Na zona: " + std::to_string(totalNaZona), cv::Point(10, 30),
                        cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);
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
 * @brief  Finaliza todas as threads ativas e destroi as janelas da aplicacao.
 * @return void
 ******************************************************************************/
void Orchestrator_Stop(void)
{
    std::cout << "[ORCHESTRATOR] Parando servicos..." << std::endl;
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

/******************************************************************************/
/**
 * @brief  Traduz o ID numerico da classe para o nome correspondente da forma.
 * @param  classId Identificador numérico retornado pelo modelo neural.
 * @return std::string com o nome em portugues da forma geometrica.
 ******************************************************************************/
static std::string ObterNomeForma(int classId)
{
    switch (classId)
    {
        case 0: return "Circulo";
        case 1: return "Hexagono";
        case 2: return "Losango";
        case 3: return "Quadrado";
        case 4: return "Retangulo";
        case 5: return "Triangulo";
        default: return "Nao detectada";
    }
}

/** @} DOXYGEN GROUP TAG END OF FILE */