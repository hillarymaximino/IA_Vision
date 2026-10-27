/******************************************************************************/
/**
 * @file    SinalObjeto.hpp
 * @addtogroup ORCHESTRATOR
 * @brief   Maquina de estados do sinal "objeto encontrado" (0xA1/0xA0) enviado ao STM32.
 * @author  Luiz Neto
 * @details
 * \n <b>Grupo de Pesquisa:</b> ASSERT - IFPB
 * \n <b>Projeto:</b> SICOV
 *
 * Regra da esteira (um ciclo de servo por objeto):
 *  1. Circulo confirmado na ROI: envia 0xA1 UMA vez e PARA de detectar.
 *  2. Esteira parou (0xB0): envia 0xA0 (PD7 = 1). A InovaLink so devolve o servo e
 *     religa o motor depois que o PD7 volta para 1; segurar o PD7 em 0 ate a esteira
 *     andar trava os dois lados (servo parado para sempre, visto na versao 2.5.0).
 *     Com a esteira parada, deteccoes sao ignoradas e nada mais e enviado.
 *  3. Esteira volta a andar (0xB1): volta a detectar, mas o objeto recem-tratado so
 *     rearma o sinal depois de sair da ROI (evita contar o mesmo circulo de novo).
 *  4. Seguranca: se a esteira nao parar em `timeoutSemParadaMs` apos o 0xA1, libera
 *     o PD7 (0xA0) para o sistema nao ficar travado.
 *
 * \code
 *           circulo em N_ENTRADA inferencias seguidas (0xA1)
 *  PRONTO ----------------------------------------------> SINALIZADO
 *    ^                                                      |  (nao detecta)
 *    |                                 0xB0 recebido (0xA0) |   timeout sem parada
 *    |                                                      v   (0xA0) ------------+
 *    |                                              ESTEIRA_PARADA                  |
 *    |                                               (nao detecta)                  |
 *    |                                                      |  0xB1 recebido        |
 *    |   ROI sem circulo em N_SAIDA inferencias             v                       |
 *    +------------------------------------------- AGUARDANDO_SAIDA <-----------------+
 * \endcode
 *
 * Sem dependencia de OpenCV, serial ou relogio: o Orchestrator informa o tempo e
 * traduz as Acoes em chamadas da bridge. Testado em tests/test_sinal_objeto.cpp.
 *
 * @{
 ******************************************************************************/

#ifndef SINAL_OBJETO_HPP
#define SINAL_OBJETO_HPP

#include <cstdint>

class SinalObjeto
{
public:
    enum class Estado
    {
        Pronto,           /**< Detectando; o proximo circulo confirmado gera 0xA1. */
        Sinalizado,       /**< 0xA1 enviado; deteccao parada, esperando a esteira parar. */
        EsteiraParada,    /**< 0xA0 enviado na parada; servo atuando; nada mais e enviado. */
        AguardandoSaida   /**< Esteira andando de novo; espera o objeto sair da ROI. */
    };

    enum class Acao
    {
        Nenhuma,
        EnviarObjetoEncontrado,   /**< 0xA1 */
        EnviarSemObjeto           /**< 0xA0 */
    };

    /**
     * @param confirmacoesEntrada Inferencias seguidas COM circulo na ROI para enviar 0xA1.
     * @param confirmacoesSaida   Inferencias seguidas SEM circulo na ROI para rearmar.
     * @param timeoutSemParadaMs  Tempo maximo esperando a esteira parar apos o 0xA1.
     */
    SinalObjeto(int confirmacoesEntrada, int confirmacoesSaida, int64_t timeoutSemParadaMs)
        : mEntrada(confirmacoesEntrada), mSaida(confirmacoesSaida), mTimeoutMs(timeoutSemParadaMs) {}

    /** @brief true se a IA deve receber quadros (esteira andando e sem objeto pendente). */
    bool detectando() const
    {
        return mEstado == Estado::Pronto || mEstado == Estado::AguardandoSaida;
    }

    /**
     * @brief Processa UMA inferencia nova feita com a esteira andando.
     * @param circuloNaRoi Circulo valido com a caixa inteira dentro da ROI.
     * @param agoraMs      Relogio monotonic em ms (marca o instante do 0xA1).
     */
    Acao aoNovaInferencia(bool circuloNaRoi, int64_t agoraMs)
    {
        if (!detectando())
        {
            return Acao::Nenhuma;   // depois do 0xA1 nenhuma deteccao conta
        }

        if (circuloNaRoi)
        {
            mComCirculo++;
            mSemCirculo = 0;
        }
        else
        {
            mSemCirculo++;
            mComCirculo = 0;
        }

        if (mEstado == Estado::Pronto && mComCirculo >= mEntrada)
        {
            mudarPara(Estado::Sinalizado);
            mInstanteSinalMs = agoraMs;
            return Acao::EnviarObjetoEncontrado;
        }
        if (mEstado == Estado::AguardandoSaida && mSemCirculo >= mSaida)
        {
            mudarPara(Estado::Pronto);
        }
        return Acao::Nenhuma;
    }

    /**
     * @brief A esteira parou (0xB0).
     * @return EnviarSemObjeto se havia um objeto sinalizado: devolve o PD7 para 1 para
     *         a InovaLink completar o ciclo do servo e religar a esteira.
     */
    Acao aoPausar()
    {
        zerarContagem();
        if (mEstado == Estado::Sinalizado)
        {
            mEstado = Estado::EsteiraParada;
            return Acao::EnviarSemObjeto;
        }
        return Acao::Nenhuma;
    }

    /** @brief A esteira voltou a andar (0xB1): volta a detectar, sem enviar nada. */
    void aoRetomar()
    {
        zerarContagem();
        if (mEstado == Estado::EsteiraParada)
        {
            mEstado = Estado::AguardandoSaida;
        }
    }

    /**
     * @brief Chamar periodicamente. Libera o PD7 se a esteira nao parar a tempo.
     * @param agoraMs Relogio monotonic em ms.
     */
    Acao verificarTimeout(int64_t agoraMs)
    {
        if (mEstado == Estado::Sinalizado && (agoraMs - mInstanteSinalMs) >= mTimeoutMs)
        {
            mudarPara(Estado::AguardandoSaida);
            return Acao::EnviarSemObjeto;
        }
        return Acao::Nenhuma;
    }

    Estado estado() const { return mEstado; }

private:
    void mudarPara(Estado novo)
    {
        mEstado = novo;
        zerarContagem();
    }

    void zerarContagem()
    {
        mComCirculo = 0;
        mSemCirculo = 0;
    }

    const int mEntrada;
    const int mSaida;
    const int64_t mTimeoutMs;
    Estado mEstado{Estado::Pronto};
    int mComCirculo{0};
    int mSemCirculo{0};
    int64_t mInstanteSinalMs{0};
};

#endif /* SINAL_OBJETO_HPP */
/** @} DOXYGEN GROUP TAG END OF FILE */
