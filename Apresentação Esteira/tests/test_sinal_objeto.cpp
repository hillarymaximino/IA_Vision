/**
 * @file    test_sinal_objeto.cpp
 * @brief   Testes da maquina de estados SinalObjeto (visao/inc/SinalObjeto.hpp).
 *
 * Simula a esteira COM a InovaLink, como observado na bancada:
 *  - PD7 desce (0xA1)  -> InovaLink para o motor e aciona o servo (STM32 envia 0xB0);
 *  - PD7 sobe  (0xA0)  -> InovaLink devolve o servo e religa o motor (STM32 envia 0xB1).
 *
 * Regras protegidas:
 *  - UM ciclo de servo por objeto fisico (o mesmo circulo nao e contado de novo);
 *  - o ciclo SEMPRE termina: servo volta e esteira religa (a versao 2.5.0 travava,
 *    porque segurava o PD7 em 0 ate a esteira andar);
 *  - depois do 0xA1 a visao para de detectar; com a esteira parada nada e enviado.
 *
 * Sem dependencias (nem OpenCV): roda com `ctest` em qualquer maquina.
 */

#include "SinalObjeto.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using Acao = SinalObjeto::Acao;
using Estado = SinalObjeto::Estado;

/** Mesmos parametros do Orchestrator. */
constexpr int kEntrada = 2;
constexpr int kSaida = 3;
constexpr int64_t kTimeoutMs = 5000;
constexpr int64_t kPeriodoInferenciaMs = 100;

struct Bancada
{
    SinalObjeto sinal{kEntrada, kSaida, kTimeoutMs};
    int64_t agoraMs = 0;

    // Estado fisico
    bool pd7Baixo = false;
    bool motorLigado = true;
    bool servoAtivo = false;
    bool inovaLinkAutomatica = true;   // false: simula controladora que nao reage

    // O que observamos
    int ciclosServo = 0;
    int enviosComEsteiraParada = 0;    // envios ALEM do 0xA0 da propria parada

    void aplicar(Acao acao, bool naTransicaoDaParada = false)
    {
        if (acao == Acao::Nenhuma) return;
        if (!motorLigado && !naTransicaoDaParada) enviosComEsteiraParada++;
        pd7Baixo = (acao == Acao::EnviarObjetoEncontrado);
        reagirInovaLink();
    }

    /** InovaLink: reage ao nivel do PD7 e o STM32 informa o estado do motor. */
    void reagirInovaLink()
    {
        if (!inovaLinkAutomatica) return;
        if (pd7Baixo && motorLigado)
        {
            motorLigado = false;          // para a esteira
            servoAtivo = true;            // aciona o servo
            ciclosServo++;
            aplicar(sinal.aoPausar(), true);   // STM32 -> 0xB0
        }
        else if (!pd7Baixo && !motorLigado)
        {
            servoAtivo = false;           // devolve o servo
            motorLigado = true;           // religa a esteira
            sinal.aoRetomar();            // STM32 -> 0xB1
        }
    }

    /**
     * Uma letra por inferencia: 'C' = circulo na ROI, '.' = ROI sem circulo.
     * Como no Orchestrator, a IA so e consultada com a esteira andando e se
     * sinal.detectando(); caso contrario o tempo passa e o quadro e ignorado.
     */
    void inferencias(const std::string& padrao)
    {
        for (char c : padrao)
        {
            agoraMs += kPeriodoInferenciaMs;
            aplicar(sinal.verificarTimeout(agoraMs));
            if (motorLigado && sinal.detectando())
            {
                aplicar(sinal.aoNovaInferencia(c == 'C', agoraMs));
            }
        }
    }
};

int gFalhas = 0;

void verificar(bool ok, const char* descricao)
{
    std::printf("[%s] %s\n", ok ? " OK  " : "FALHA", descricao);
    if (!ok) gFalhas++;
}

} // namespace

int main()
{
    {
        Bancada b;
        b.inferencias("..CC");     // circulo confirmado -> 0xA1 -> InovaLink para e aciona o servo
        verificar(b.ciclosServo == 1, "circulo confirmado: servo atua 1 vez");
        verificar(!b.servoAtivo && b.motorLigado,
                  "ciclo termina: servo volta e esteira religa (2.5.0 travava aqui)");
        b.inferencias("CCCCCC....");   // mesmo circulo ainda na ROI apos religar, depois sai
        verificar(b.ciclosServo == 1, "mesmo circulo apos a esteira religar: servo continua em 1 ciclo");
        verificar(b.enviosComEsteiraParada == 0, "nada enviado com a esteira parada alem do 0xA0 da parada");
    }
    {
        Bancada b;
        b.motorLigado = false;          // esteira desligada pelo operador
        b.inovaLinkAutomatica = false;
        b.sinal.aoPausar();
        b.inferencias("CCCCCCCC");
        verificar(b.ciclosServo == 0 && !b.pd7Baixo && b.enviosComEsteiraParada == 0,
                  "esteira desligada: circulo ignorado, nada enviado");
    }
    {
        Bancada b;
        b.inferencias("..CC" "CC...." "..CC" "CC....");
        verificar(b.ciclosServo == 2, "dois objetos separados: 2 ciclos de servo");
    }
    {
        Bancada b;
        b.inferencias("CC...");         // servo tirou a peca; ROI vazia ao religar
        verificar(b.sinal.estado() == Estado::Pronto, "ROI vazia apos religar rearma o sinal");
    }
    {
        Bancada b;
        b.inferencias("...C...C...");
        verificar(b.ciclosServo == 0, "falso positivo isolado nao aciona o servo");
    }
    {
        Bancada b;
        b.inovaLinkAutomatica = false;  // controladora nao para a esteira
        b.inferencias("CC");
        verificar(b.pd7Baixo, "0xA1 enviado (PD7 = 0)");
        b.inferencias(std::string(kTimeoutMs / kPeriodoInferenciaMs, 'C'));
        verificar(!b.pd7Baixo, "sem parada em 5 s: 0xA0 automatico (nao fica travado)");
        b.inferencias("CCCC");
        verificar(!b.pd7Baixo, "apos o timeout, o mesmo circulo nao gera novo 0xA1");
        b.inferencias("....CC");
        verificar(b.pd7Baixo, "objeto seguinte (ROI esvaziou) gera 0xA1");
    }
    {
        Bancada b;
        b.inferencias("CC");
        for (int i = 0; i < 5; ++i) b.inferencias("CCC");   // objeto continua na ROI
        verificar(b.ciclosServo == 1, "objeto parado na ROI por varios ciclos: 1 ciclo de servo");
    }

    std::printf("\n%s\n", gFalhas ? "RESULTADO: FALHOU" : "RESULTADO: todos os casos passaram");
    return gFalhas == 0 ? 0 : 1;
}
