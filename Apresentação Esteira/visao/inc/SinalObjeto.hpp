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
 * Garante UM 0xA1 por objeto fisico. A InovaLink conta cada descida do PD7; sem
 * rearme, o mesmo circulo era contado de novo quando a esteira voltava a andar com
 * ele ainda dentro da ROI (a esteira para justamente com o objeto na zona).
 *
 * \code
 *            circulo em N_ENTRADA inferencias seguidas
 *  PRONTO ------------------------------------------> SINALIZADO   (envia 0xA1)
 *    ^                                                  |      |
 *    |   ROI sem circulo em N_SAIDA inferencias         |      | pausa da varredura (0xB0)
 *    +--------------------------------------------------+      | (envia 0xA0)
 *    |        (envia 0xA0: objeto saiu sem parar a esteira)    v
 *    +------------------------------------------- AGUARDANDO_SAIDA
 *         ROI sem circulo em N_SAIDA inferencias (nao envia nada: so rearma)
 * \endcode
 *
 * Sem dependencia de OpenCV ou serial: o Orchestrator traduz as Acoes em chamadas
 * da bridge, e a logica pode ser testada isoladamente.
 *
 * @{
 ******************************************************************************/

#ifndef SINAL_OBJETO_HPP
#define SINAL_OBJETO_HPP

class SinalObjeto
{
public:
    enum class Estado
    {
        Pronto,           /**< Armado: o proximo circulo confirmado gera 0xA1. */
        Sinalizado,       /**< 0xA1 enviado; PD7 = 0 no STM32. */
        AguardandoSaida   /**< Objeto ja contado; espera a ROI esvaziar para rearmar. */
    };

    enum class Acao
    {
        Nenhuma,
        EnviarObjetoEncontrado,   /**< 0xA1 */
        EnviarSemObjeto           /**< 0xA0 */
    };

    /**
     * @param confirmacoesEntrada Inferencias seguidas COM circulo na ROI para sinalizar.
     * @param confirmacoesSaida   Inferencias seguidas SEM circulo na ROI para liberar/rearmar.
     */
    SinalObjeto(int confirmacoesEntrada, int confirmacoesSaida)
        : mEntrada(confirmacoesEntrada), mSaida(confirmacoesSaida) {}

    /** @brief Processa UMA inferencia nova (nunca a mesma inferencia duas vezes). */
    Acao aoNovaInferencia(bool circuloNaRoi)
    {
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

        switch (mEstado)
        {
            case Estado::Pronto:
                if (mComCirculo >= mEntrada)
                {
                    mudarPara(Estado::Sinalizado);
                    return Acao::EnviarObjetoEncontrado;
                }
                break;

            case Estado::Sinalizado:
                if (mSemCirculo >= mSaida)
                {
                    // Objeto saiu da ROI sem a controladora parar a esteira
                    mudarPara(Estado::Pronto);
                    return Acao::EnviarSemObjeto;
                }
                break;

            case Estado::AguardandoSaida:
                if (mSemCirculo >= mSaida)
                {
                    mudarPara(Estado::Pronto);
                }
                break;
        }
        return Acao::Nenhuma;
    }

    /**
     * @brief A varredura foi pausada pelo STM32 (esteira parada / servo atuando).
     * @return EnviarSemObjeto se havia sinal ativo (devolve o PD7 para 1). O objeto
     *         ja contado continua bloqueado ate a ROI esvaziar com a esteira andando.
     */
    Acao aoPausar()
    {
        zerarContagem();
        if (mEstado == Estado::Sinalizado)
        {
            mEstado = Estado::AguardandoSaida;
            return Acao::EnviarSemObjeto;
        }
        return Acao::Nenhuma;
    }

    /** @brief A varredura foi retomada: recomeca a contagem, mantendo o estado. */
    void aoRetomar() { zerarContagem(); }

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
    Estado mEstado{Estado::Pronto};
    int mComCirculo{0};
    int mSemCirculo{0};
};

#endif /* SINAL_OBJETO_HPP */
/** @} DOXYGEN GROUP TAG END OF FILE */
