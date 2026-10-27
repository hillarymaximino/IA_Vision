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
#include "SinalObjeto.hpp"
#include "Stm32Bridge.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdint>
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

/** @brief Inferencias seguidas COM circulo na ROI para enviar 0xA1 (filtra falso positivo isolado). */
static constexpr int dCONFIRMACOES_ENTRADA = 2;

/** @brief Inferencias seguidas SEM circulo na ROI para rearmar apos um objeto ja tratado. */
static constexpr int dCONFIRMACOES_SAIDA = 3;

/** @brief Tempo maximo esperando a esteira parar apos o 0xA1; depois disso envia 0xA0. */
static constexpr int64_t dTIMEOUT_SEM_PARADA_MS = 5000;

/** @brief Intervalo entre tentativas de reconectar ao STM32 apos uma queda do USB. */
static constexpr int64_t dINTERVALO_RECONEXAO_MS = 2000;

/*******************************************************************************
 * VARIAVEIS DA ROI E SINAIS DE CONTROLE
 ******************************************************************************/

/* Em permil (1/1000) do frame, para funcionar em qualquer resolucao de camera.
 * Precisam ser 'int' simples e nao-const: o OpenCV escreve nelas via trackbar. */
static int gRoiCx = 388;   /**< Centro X da ROI (500 = meio da largura).        */
static int gRoiCy = 236;   /**< Centro Y da ROI (260 = 26% da altura).          */
static int gRoiW  = 665;   /**< Largura da ROI (380 = 38% do frame).            */
static int gRoiH  = 444;   /**< Altura da ROI (380 = 38% do frame).             */

/** @brief Sinal global ativo enquanto um CIRCULO e reconhecido na ROI. */
static std::atomic<bool> gSinalCirculo{false};

/** @brief Porta pedida na inicializacao, reutilizada nas reconexoes ("" = auto-deteccao). */
static std::string gPortaSerial;

/*******************************************************************************
 * PROTOTIPOS LOCAIS
 ******************************************************************************/
static cv::Rect ObterEnquadramentoInspecao(const cv::Size& frameSize);
static cv::Rect DesenharZonaInspecao(cv::Mat& frame);
static void DesenharDeteccao(cv::Mat& frame, const cv::Rect& box, const std::string& label);
static void ExecutarAcao(SinalObjeto::Acao acao, const char* motivo);
static int64_t AgoraMs(void);
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

    gPortaSerial = serialPort;
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
 * \n 1. Recupera o quadro mais recente da camera industrial.
 * \n 2. Se o STM32 pausou a varredura: nao envia o quadro para a IA, reseta o sinal
 *       e mostra o quadro ao vivo.
 * \n 3. Se a varredura esta ativa: envia o quadro para a IA e pega a ultima inferencia
 *       JUNTO com o quadro em que ela rodou. A ROI e o HUD sao desenhados nesse quadro,
 *       entao a tela mostra exatamente o que foi avaliado (sem caixa "atrasada").
 * \n 4. Avalia cada objeto (so conta se a caixa estiver INTEIRA na zona):
 *       - Vermelho + "CIRCULO": circulo reconhecido.
 *       - Vermelho + "Nao detectada": anomalia / forma desconhecida.
 *       - Outras formas: so contam em "Na zona", sem desenho.
 * \n 5. A cada inferencia NOVA, alimenta o SinalObjeto: 0xA1 apos
 *       dCONFIRMACOES_ENTRADA inferencias seguidas com circulo e a deteccao PARA.
 *       Quando a esteira para, envia 0xA0 (a InovaLink precisa do PD7 em 1 para
 *       devolver o servo e religar o motor) e nada mais ate ela voltar a andar; entao
 *       volta a detectar e o objeto recem-tratado so rearma o sinal ao sair da ROI.
 *       Um 0xA1 por objeto, um ciclo de servo por objeto.
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
    InferenceResult inferencia;
    SinalObjeto sinal(dCONFIRMACOES_ENTRADA, dCONFIRMACOES_SAIDA, dTIMEOUT_SEM_PARADA_MS);
    bool varreduraAnterior = false;
    bool detectandoAnterior = true;
    bool avisouQuedaStm32 = false;
    int64_t ultimaReconexaoMs = 0;
    uint64_t ultimaSequencia = 0;   // ultima inferencia ja contabilizada no sinal

    std::cout << "[ORCHESTRATOR] Pipeline ativo. 's' = salvar ROI | ESC = finalizar." << std::endl;

    while (true)
    {
        // 1. Obtem o quadro gerado pela camera (ainda limpo, sem HUD)
        if (Camera_GetFrame(frame))
        {
            // Permissao do STM32: so com a esteira em movimento. Falso tambem se o STM32
            // estiver desconectado ou sem enviar o estado ha mais de 2,5 s (fail-safe).
            // Queda do USB do ST-Link (ruido do servo/motor): reconecta sozinho. Enquanto
            // isso esta_varredura_ativa() e false (fail-safe) e a visao fica pausada.
            const bool stm32Conectado = bridge::esta_conectado();
            if (!stm32Conectado)
            {
                if (!avisouQuedaStm32)
                {
                    std::cout << "[STM32] Conexao perdida. Tentando reconectar a cada "
                              << dINTERVALO_RECONEXAO_MS / 1000 << " s..." << std::endl;
                    avisouQuedaStm32 = true;
                }
                if (AgoraMs() - ultimaReconexaoMs >= dINTERVALO_RECONEXAO_MS)
                {
                    ultimaReconexaoMs = AgoraMs();
                    if (bridge::conectar(gPortaSerial))
                    {
                        std::cout << "[STM32] Reconectado em " << bridge::obter_porta_conectada() << "." << std::endl;
                        avisouQuedaStm32 = false;
                        // O 0xA0 pode ter se perdido na queda: garante o PD7 em 1, a menos
                        // que um objeto sinalizado ainda espere a esteira parar
                        if (sinal.estado() != SinalObjeto::Estado::Sinalizado)
                        {
                            ExecutarAcao(SinalObjeto::Acao::EnviarSemObjeto, "ressincronizando apos reconexao");
                        }
                    }
                }
            }

            const bool varreduraAtiva = bridge::esta_varredura_ativa();

            if (varreduraAtiva != varreduraAnterior)
            {
                if (varreduraAtiva)
                {
                    // Esteira voltou a andar: volta a detectar (objeto tratado so rearma ao sair da ROI)
                    sinal.aoRetomar();
                }
                else
                {
                    // Esteira parou: devolve o PD7 para 1 (0xA0). A InovaLink so devolve o
                    // servo e religa o motor com o PD7 em 1; segura-lo em 0 trava o ciclo.
                    ExecutarAcao(sinal.aoPausar(), "esteira parou");
                }
                varreduraAnterior = varreduraAtiva;
            }

            // Seguranca: a controladora nao parou a esteira apos o 0xA1
            ExecutarAcao(sinal.verificarTimeout(AgoraMs()), "esteira nao parou em 5 s");

            // A IA so trabalha com a esteira andando e sem objeto pendente. Ao entrar ou
            // sair desse modo, descarta tudo o que ela produziu antes: nenhuma deteccao
            // antiga pode acionar o STM32 depois.
            const bool detectar = varreduraAtiva && sinal.detectando();
            if (detectar != detectandoAnterior)
            {
                Vision_Reset();
                detectandoAnterior = detectar;
            }
            gSinalCirculo = (sinal.estado() == SinalObjeto::Estado::Sinalizado);   // PD7 = 0

            // Quadro exibido: ao vivo sem deteccao; detectando, o quadro avaliado pela IA
            cv::Mat tela;

            if (!detectar)
            {
                // 2. Sem deteccao: esteira parada, ou circulo ja sinalizado esperando a parada
                tela = frame;
                DesenharZonaInspecao(tela);
                if (!stm32Conectado)
                {
                    cv::putText(tela, "STM32 DESCONECTADO: RECONECTANDO...", cv::Point(10, 30),
                                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
                }
                else if (!varreduraAtiva)
                {
                    cv::putText(tela, "PAUSADO: ESTEIRA PARADA (AGUARDANDO 0xB1)", cv::Point(10, 30),
                                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 165, 255), 2);
                }
                if (sinal.estado() == SinalObjeto::Estado::Sinalizado)
                {
                    cv::putText(tela, "CIRCULO DETECTADO: AGUARDANDO A ESTEIRA PARAR", cv::Point(10, 65),
                                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
                }
                else if (sinal.estado() == SinalObjeto::Estado::EsteiraParada)
                {
                    cv::putText(tela, "OBJETO TRATADO: SERVO EM ACAO (DETECCAO PAUSADA)", cv::Point(10, 65),
                                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 165, 255), 2);
                }
            }
            else
            {
                // 3. Detectando: o quadro limpo vai para a IA
                Vision_SubmitFrame(frame);

                if (!Vision_GetLatest(inferencia))
                {
                    // Primeira inferencia apos (re)liberar ainda em andamento: nada a decidir
                    tela = frame;
                    DesenharZonaInspecao(tela);
                    cv::putText(tela, "AGUARDANDO A IA...", cv::Point(10, 30),
                                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);
                }
                else
                {
                    // ROI, caixas e decisao no MESMO quadro que a IA avaliou
                    tela = inferencia.frame;
                    const cv::Rect roi = DesenharZonaInspecao(tela);

                    // 4. Avalia e renderiza as deteccoes
                    int totalNaZona = 0;
                    bool circuloNaInferencia = false;

                    for (const auto& det : inferencia.detections)
                    {
                        // Caixa degenerada nao e uma deteccao utilizavel
                        if (det.box.area() <= 0) continue;

                        // Regra de enquadramento: a caixa INTEIRA precisa estar dentro da ROI
                        if ((roi & det.box) != det.box) continue;

                        if (det.isValid && det.classId == dCLASS_ID_CIRCULO)
                        {
                            circuloNaInferencia = true;
                            DesenharDeteccao(tela, det.box, "CIRCULO");
                        }
                        else if (!det.isValid)
                        {
                            DesenharDeteccao(tela, det.box, "Nao detectada");
                        }

                        totalNaZona++;
                    }

                    // 5. Sinal 0xA1 (SinalObjeto): so inferencias NOVAS contam; a mesma
                    //    inferencia e reaproveitada em varios quadros enquanto a IA trabalha.
                    //    Apos o 0xA1 a deteccao para (detectando() = false no proximo quadro).
                    if (inferencia.sequence != ultimaSequencia)
                    {
                        ultimaSequencia = inferencia.sequence;
                        ExecutarAcao(sinal.aoNovaInferencia(circuloNaInferencia, AgoraMs()), "inferencia");
                    }

                    // HUD normal quando a varredura esta ativa
                    cv::putText(tela, "Na zona: " + std::to_string(totalNaZona), cv::Point(10, 30),
                                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);

                    if (sinal.estado() == SinalObjeto::Estado::AguardandoSaida)
                    {
                        cv::putText(tela, "OBJETO JA TRATADO: AGUARDANDO SAIR DA ZONA", cv::Point(10, 65),
                                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 165, 255), 2);
                    }
                }
            }

            cv::putText(tela, "[s] salvar ROI  [ESC] sair", cv::Point(10, tela.rows - 15),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);

            cv::imshow(dWINDOW_NAME, tela);
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
 * @brief  Desenha a moldura da Zona de Inspecao (amarelo, BGR: 0, 255, 255).
 * @param  frame Quadro onde a moldura sera desenhada.
 * @return cv::Rect A ROI em pixels deste quadro (a mesma usada para filtrar as caixas).
 ******************************************************************************/
static cv::Rect DesenharZonaInspecao(cv::Mat& frame)
{
    const cv::Rect roi = ObterEnquadramentoInspecao(frame.size());
    cv::rectangle(frame, roi, cv::Scalar(0, 255, 255), 2);
    cv::putText(frame, "ZONA DE INSPECAO", cv::Point(roi.x, std::max(15, roi.y - 10)),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);
    return roi;
}

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
 * @brief  Relogio monotonic em milissegundos (nao muda com ajuste de hora do sistema).
 * @return int64_t Milissegundos desde um instante arbitrario fixo.
 ******************************************************************************/
static int64_t AgoraMs(void)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/******************************************************************************/
/**
 * @brief  Traduz uma acao do SinalObjeto no comando correspondente ao STM32.
 * @param  acao Acao decidida pela maquina de estados do sinal.
 * @param  motivo Contexto exibido no log (ex: "inferencia", "varredura pausada").
 * @return void
 ******************************************************************************/
static void ExecutarAcao(SinalObjeto::Acao acao, const char* motivo)
{
    switch (acao)
    {
        case SinalObjeto::Acao::EnviarObjetoEncontrado:
            std::cout << "[SINAL] >>> CIRCULO confirmado na ROI (" << motivo
                      << "). Avisando STM32 (0xA1)... <<<" << std::endl;
            bridge::enviar_objeto_encontrado();
            break;

        case SinalObjeto::Acao::EnviarSemObjeto:
            std::cout << "[SINAL] >>> Sem objeto (" << motivo
                      << "). Avisando STM32 (0xA0)... <<<" << std::endl;
            bridge::enviar_sem_objeto();
            break;

        case SinalObjeto::Acao::Nenhuma:
            break;
    }
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
