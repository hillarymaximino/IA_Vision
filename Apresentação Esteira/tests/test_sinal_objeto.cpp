/**
 * @file    test_sinal_objeto.cpp
 * @brief   Testes da maquina de estados SinalObjeto (visao/inc/SinalObjeto.hpp).
 *
 * Simula a esteira e conta as descidas do PD7 que a InovaLink veria. Regra
 * protegida: UM 0xA1 por objeto fisico, inclusive quando a esteira para com o
 * objeto dentro da ROI e volta a andar com ele ainda la (bug da contagem dupla).
 *
 * Sem dependencias (nem OpenCV): roda com `ctest` em qualquer maquina.
 */

#include "SinalObjeto.hpp"

#include <cstdio>
#include <string>

namespace {

using Acao = SinalObjeto::Acao;
using Estado = SinalObjeto::Estado;

/** Mesmos limiares do Orchestrator (dCONFIRMACOES_ENTRADA / dCONFIRMACOES_SAIDA). */
constexpr int kEntrada = 2;
constexpr int kSaida = 3;

struct Esteira
{
    SinalObjeto sinal{kEntrada, kSaida};
    int contagemInovaLink = 0;   // descidas do PD7 (0xA1 com o PD7 em 1)
    bool pd7Baixo = false;

    void aplicar(Acao acao)
    {
        if (acao == Acao::EnviarObjetoEncontrado)
        {
            if (!pd7Baixo) contagemInovaLink++;
            pd7Baixo = true;
        }
        else if (acao == Acao::EnviarSemObjeto)
        {
            pd7Baixo = false;
        }
    }

    /** Uma letra por inferencia: 'C' = circulo na ROI, '.' = ROI sem circulo. */
    void inferencias(const std::string& padrao)
    {
        for (char c : padrao) aplicar(sinal.aoNovaInferencia(c == 'C'));
    }

    void pausa() { aplicar(sinal.aoPausar()); }
    void retoma() { sinal.aoRetomar(); }
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
        Esteira e;
        e.inferencias("..CC");      // circulo confirmado -> 0xA1
        e.pausa();                  // InovaLink para a esteira (0xB0) -> 0xA0
        e.retoma();                 // esteira volta (0xB1) com o MESMO circulo na ROI
        e.inferencias("CCCCCC");
        e.inferencias("....");
        verificar(e.contagemInovaLink == 1, "esteira para com o objeto na ROI: conta 1 (bug contava 2)");
    }
    {
        Esteira e;
        e.inferencias("..CC");
        e.pausa();
        e.retoma();
        e.inferencias("CCC....");   // primeiro objeto sai
        e.inferencias("..CC");      // segundo objeto
        verificar(e.contagemInovaLink == 2, "dois objetos separados contam 2");
    }
    {
        Esteira e;
        e.inferencias("..CC.C.CC.CC");
        verificar(e.contagemInovaLink == 1, "deteccao que pisca (1 inferencia vazia) conta 1");
    }
    {
        Esteira e;
        e.inferencias("...C...C...");
        verificar(e.contagemInovaLink == 0, "falso positivo em inferencia isolada nao conta");
    }
    {
        Esteira e;
        e.inferencias("CC");
        e.pausa();
        e.retoma();
        e.inferencias("...");       // servo tirou a peca da ROI
        verificar(e.sinal.estado() == Estado::Pronto, "ROI vazia apos retomar rearma o sinal");
        e.inferencias("CC");
        verificar(e.contagemInovaLink == 2, "objeto seguinte apos rearme conta");
    }
    {
        Esteira e;
        e.inferencias("CCCC...");   // objeto passa sem a controladora parar a esteira
        verificar(!e.pd7Baixo && e.sinal.estado() == Estado::Pronto, "objeto sai sem pausa: 0xA0 e rearma");
    }
    {
        Esteira e;
        e.inferencias("C");         // 1 de 2 confirmacoes
        e.pausa();
        e.retoma();
        e.inferencias("C.");
        verificar(e.contagemInovaLink == 0, "contagem parcial e zerada na pausa");
    }
    {
        Esteira e;
        e.inferencias("CC");
        for (int i = 0; i < 3; ++i)
        {
            e.pausa();
            e.retoma();
            e.inferencias("CCC");
        }
        verificar(e.contagemInovaLink == 1, "3 pausas com o objeto parado na ROI contam 1");
    }

    std::printf("\n%s\n", gFalhas ? "RESULTADO: FALHOU" : "RESULTADO: todos os casos passaram");
    return gFalhas == 0 ? 0 : 1;
}
