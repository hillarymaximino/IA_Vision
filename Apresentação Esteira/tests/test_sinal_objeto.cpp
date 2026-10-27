/**
 * @file    test_sinal_objeto.cpp
 * @brief   Testes da maquina de estados SinalObjeto (visao/inc/SinalObjeto.hpp).
 *
 * Simula a esteira e registra o que a InovaLink veria no PD7. Regras protegidas:
 *  - UM 0xA1 por objeto fisico (uma descida do PD7 => um ciclo de servo);
 *  - depois do 0xA1 a visao para de detectar ate a esteira voltar a andar;
 *  - nada e enviado com a esteira parada; o 0xA0 so sai quando ela volta a andar.
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

struct Esteira
{
    SinalObjeto sinal{kEntrada, kSaida, kTimeoutMs};
    int64_t agoraMs = 0;
    bool andando = true;
    int descidasPd7 = 0;       // o que a InovaLink conta (cada uma dispara o servo)
    int subidasPd7 = 0;
    bool pd7Baixo = false;
    int enviosComEsteiraParada = 0;

    void aplicar(Acao acao)
    {
        if (acao == Acao::Nenhuma) return;
        if (!andando) enviosComEsteiraParada++;
        if (acao == Acao::EnviarObjetoEncontrado)
        {
            if (!pd7Baixo) descidasPd7++;
            pd7Baixo = true;
        }
        else
        {
            if (pd7Baixo) subidasPd7++;
            pd7Baixo = false;
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
            if (andando && sinal.detectando())
            {
                aplicar(sinal.aoNovaInferencia(c == 'C', agoraMs));
            }
        }
    }

    void parar() { andando = false; sinal.aoPausar(); }
    void andar() { andando = true; aplicar(sinal.aoRetomar()); }
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
        // Ciclo completo: circulo -> 0xA1 -> esteira para -> servo -> esteira anda
        Esteira e;
        e.inferencias("..CC");
        verificar(e.descidasPd7 == 1 && e.pd7Baixo, "circulo confirmado envia 0xA1 (PD7 = 0)");
        verificar(!e.sinal.detectando(), "apos o 0xA1 a visao para de detectar");
        e.inferencias("CCCC");             // circulo continua na ROI ate a esteira parar
        e.parar();
        verificar(e.pd7Baixo, "esteira parada: PD7 continua em 0 (sem 0xA0 no meio do servo)");
        e.inferencias("CCCCCCCC");         // servo atuando; IA nem e consultada
        e.andar();
        verificar(!e.pd7Baixo && e.subidasPd7 == 1, "esteira volta a andar: 0xA0 (PD7 = 1)");
        e.inferencias("CCCCCC....");       // mesmo circulo ainda na ROI, depois sai
        verificar(e.descidasPd7 == 1, "mesmo circulo apos a esteira voltar: conta 1 (servo 1x)");
        verificar(e.enviosComEsteiraParada == 0, "nenhum comando enviado com a esteira parada");
    }
    {
        // Deteccao com a esteira desligada e ignorada
        Esteira e;
        e.parar();
        e.inferencias("CCCCCCCC");
        verificar(e.descidasPd7 == 0 && e.enviosComEsteiraParada == 0,
                  "esteira desligada: circulo ignorado, nada enviado");
    }
    {
        // Dois objetos separados = dois ciclos de servo
        Esteira e;
        e.inferencias("..CC"); e.parar(); e.andar(); e.inferencias("CC....");
        e.inferencias("..CC"); e.parar(); e.andar(); e.inferencias("....");
        verificar(e.descidasPd7 == 2, "dois objetos separados contam 2");
    }
    {
        // Servo tira a peca: rearma logo que a ROI estiver vazia
        Esteira e;
        e.inferencias("CC"); e.parar(); e.andar();
        e.inferencias("...");
        verificar(e.sinal.estado() == Estado::Pronto, "ROI vazia apos a esteira voltar rearma");
    }
    {
        // Inferencia que "pisca" antes da confirmacao nao dispara
        Esteira e;
        e.inferencias("...C...C...");
        verificar(e.descidasPd7 == 0, "falso positivo isolado nao envia 0xA1");
    }
    {
        // Pausa no meio da confirmacao zera a contagem
        Esteira e;
        e.inferencias("C"); e.parar(); e.andar(); e.inferencias("C.");
        verificar(e.descidasPd7 == 0, "contagem parcial e zerada na pausa");
    }
    {
        // Controladora nao para a esteira: libera o PD7 apos o timeout e rearma depois
        Esteira e;
        e.inferencias("CC");
        e.inferencias(std::string(kTimeoutMs / kPeriodoInferenciaMs, 'C'));
        verificar(!e.pd7Baixo && e.descidasPd7 == 1, "sem parada em 5 s: 0xA0 automatico, sem novo 0xA1");
        e.inferencias("....CC");
        verificar(e.descidasPd7 == 2, "apos o timeout, proximo objeto (ROI esvaziou) conta");
    }
    {
        // Varias paradas/partidas com o objeto parado na ROI: um unico 0xA1
        Esteira e;
        e.inferencias("CC");
        for (int i = 0; i < 3; ++i)
        {
            e.parar();
            e.inferencias("CCC");
            e.andar();
            e.inferencias("CCC");
        }
        verificar(e.descidasPd7 == 1, "3 paradas com o objeto na ROI contam 1");
    }

    std::printf("\n%s\n", gFalhas ? "RESULTADO: FALHOU" : "RESULTADO: todos os casos passaram");
    return gFalhas == 0 ? 0 : 1;
}
