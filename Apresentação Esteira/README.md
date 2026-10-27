# SICOV — Esteira de Inspeção (Apresentação)

Célula de triagem do projeto **SICOV** (Grupo ASSERT — IFPB). Uma câmera industrial
observa a esteira, uma rede **YOLO11** reconhece formas geométricas e, quando um
**círculo** entra na Zona de Inspeção, o PC avisa o **STM32**. O STM32 repassa o aviso
à controladora **InovaLink**, que para o motor de passo e aciona o servo.

Esta pasta reúne tudo o que roda no PC e no microcontrolador. O código é
compilado **nativamente no Windows**, e o Linux continua suportado.

---

## Sumário

1. [Arquitetura](#1-arquitetura)
2. [Estrutura da pasta](#2-estrutura-da-pasta)
3. [Pré-requisitos (Windows)](#3-pré-requisitos-windows)
4. [Ordem de bring-up](#4-ordem-de-bring-up)
5. [Launcher gráfico (automação)](#5-launcher-gráfico-automação)
6. [Build](#6-build)
7. [Execução](#7-execução)
8. [Protocolo PC ↔ STM32](#8-protocolo-pc--stm32)
9. [Origem do código e alterações](#9-origem-do-código-e-alterações)
10. [Pendências conhecidas](#10-pendências-conhecidas)
11. [Solução de problemas](#11-solução-de-problemas)

---

## 1. Arquitetura

```mermaid
flowchart LR
    CAM["Câmera Daheng<br/>MER2-302-37GM-P<br/>(GigE PoE)"] -- imagens --> PC
    subgraph PC["PC Windows — visao/VisionTest.exe"]
        direction TB
        C[CameraModule<br/>thread produtora] --> O[Orchestrator<br/>ROI + HUD + regras]
        O <--> V[VisionModule<br/>YOLO11 ONNX<br/>CUDA ou CPU]
        O <--> B[bridge_host<br/>Stm32Bridge]
    end
    B <-- "USB ST-Link VCP<br/>115200 8N1<br/>0xA1/0xA0 ↔ 0xB0/0xB1 + ACK 0x06" --> STM["STM32F407VET6<br/>firmware_stm32"]
    STM -- "PD7 foundObject" --> INV["Controladora<br/>InovaLink"]
    INV -- "PD0 motor · PD1-4 velocidade<br/>PD5 sentido · PD6 servo" --> STM
    STM -- "PA6 STEP (TIM3) · PD8 EN · PD9 DIR" --> MOT[Motor de passo<br/>da esteira]
    STM -- "PD12 PWM 50 Hz (TIM4)" --> SERVO[Servo]
```

**Ciclo de operação:**

> **Regra de segurança:** a visão só detecta com a **esteira em movimento**, ou seja,
> motor ligado e servo parado. Esteira parada, servo atuando, STM32 desconectado ou
> STM32 sem mandar o estado há mais de 2,5 s significam **detecção pausada**.

1. O STM32 liga e envia o **estado real**: `0xB0` se o motor estiver parado, `0xB1` se estiver girando. Depois disso, reenvia o estado a cada 1 s.
2. A esteira anda (`0xB1`). Se o PC vê um círculo **inteiro** dentro da ROI em 2 inferências seguidas, envia `0xA1` **uma vez**, e o STM32 coloca `PD7` em 0. A partir daí, **a visão para de detectar**.
3. A InovaLink lê `PD7 = 0`, para o motor e aciona o servo. O STM32 envia `0xB0`. O PC **não envia nada** com a esteira parada, então o `PD7` fica em 0 durante todo o ciclo do servo.
4. Quando o servo termina e o motor volta a girar, o STM32 envia `0xB1`. Só então o PC envia `0xA0` (`PD7` volta para 1) e volta a detectar.
5. O objeto que acabou de ser tratado **não é sinalizado de novo**: o PC só rearma o `0xA1` depois que a ROI fica vazia por 3 inferências com a esteira andando. Resultado: **um `0xA1` e um ciclo de servo por objeto**.
6. Segurança: se a esteira não parar em 5 s depois do `0xA1`, o PC envia `0xA0` sozinho para o sistema não ficar travado.

---

## 2. Estrutura da pasta

```text
Apresentação Esteira/
├── CMakeLists.txt         # Projeto de topo (bridge + visão)
├── CMakePresets.json      # Presets: windows, windows-bridge, linux
├── visao/                 # Aplicação de visão (PC)
│   ├── CMakeLists.txt
│   ├── inc/  src/         # CameraModule, VisionModule, Orchestrator, main
│   └── models/            # yolo11_formas.onnx (fora do Git, ver §3)
├── bridge_host/           # Biblioteca serial PC ↔ STM32 (Windows + Linux)
│   ├── include/  src/
│   └── example/           # bridge_example (terminal de teste) e serial_example
├── tests/                 # Testes de unidade (ctest), sem hardware nem OpenCV
├── firmware_stm32/        # Projeto STM32CubeIDE (STM32F407VET6)
│   └── README.md          # Pinagem, timers e detalhes do firmware
└── tools/                 # Launcher gráfico PySide6 (ver §5)
    ├── sicov-launcher.sh  # Entrada no Linux
    ├── requirements.txt
    └── sicov_launcher/
```

---

## 3. Pré-requisitos (Windows)

| Componente | Versão testada / sugerida | Observação |
| :--- | :--- | :--- |
| Visual Studio 2022 Build Tools | carga "Desenvolvimento para desktop com C++" | Já traz o CMake ≥ 3.21 |
| OpenCV para Windows | 4.10 ou mais nova (`opencv-4.x-windows.exe`) | Extraia, por exemplo, em `C:\opencv` e defina `OpenCV_DIR=C:\opencv\build` |
| Galaxy SDK (Daheng) para Windows | versão atual do site da Daheng | Instala o `GxIAPI.h`/`GxIAPI.lib`, o driver GigE e o *GxGigeIPConfig* |
| STM32CubeIDE | 2.x (já instalado) | Inclui o driver do ST-Link e o VCP |
| Modelo ONNX | `yolo11_formas.onnx` | Fica fora do Git. Copie para `visao/models/` (há uma cópia em `../Vision_Geometrica (Cópia).zip`) |

> **OpenCV com CUDA:** o pacote pronto do OpenCV para Windows **não tem CUDA**.
> Com ele, a inferência roda em CPU e o log mostra `Inferencia em CPU`.
> Para usar a RTX 4050 é preciso compilar o OpenCV com `WITH_CUDA=ON` e
> `OPENCV_DNN_CUDA=ON`. Isso não muda nada neste projeto, basta apontar o `OpenCV_DIR` para o novo build.

**Rede da câmera GigE:** a placa de rede ligada ao switch PoE precisa estar na
mesma sub-rede da câmera. Use o *GxGigeIPConfig* do Galaxy SDK para ver e
ajustar o IP da câmera. O firewall do Windows precisa liberar o *GalaxyView*
e o `VisionTest.exe` na rede privada.

---

## 4. Ordem de bring-up

Suba o sistema uma camada por vez. Assim, cada problema aparece isolado.

1. **Firmware:** no launcher, clique em **Gravar no STM32**. Ele compila e grava pelo ST-Link. Outra opção é abrir `firmware_stm32/` no STM32CubeIDE (*File → Import → Existing Projects*), compilar e gravar por lá.
2. **Comunicação:** compile o preset `windows-bridge` e rode o `bridge_example`. Ao resetar o STM32, deve aparecer `ResumeScan (0xB1)`. Os comandos `1` e `0` precisam receber ACK.
3. **Câmera:** confirme no *GalaxyView* que a câmera aparece e transmite imagem.
4. **Visão completa:** compile o preset `windows` e rode o `VisionTest`.
5. **Integração com a InovaLink:** com a esteira em movimento, confirme o ciclo completo descrito na §1.

---

## 5. Launcher gráfico (automação)

O `tools/sicov_launcher` é uma interface em **PySide6** que automatiza tudo o que
este README descreve, tanto no Linux quanto no Windows: verificar e instalar dependências,
compilar, rodar a visão, abrir o terminal da bridge, testar o handshake com o STM32 e
**compilar e gravar o firmware do STM32**.

**Linux** (de dentro de `Apresentação Esteira/`):

```bash
sh tools/sicov-launcher.sh
```

**Windows:**

```powershell
python tools\sicov_launcher
```

**Na primeira execução**, o launcher instala sozinho o que precisa:

| O que falta | Como é instalado |
| :--- | :--- |
| `python3` (Linux) | O `sicov-launcher.sh` pergunta e instala via apt, dnf ou pacman |
| PySide6 | Fica num ambiente virtual em `tools/.venv`, criado automaticamente. No Ubuntu/Debian, se faltar o `python3-venv`, ele pergunta antes de instalar |
| `libxcb-cursor0` (exigida pelo Qt 6.5+ no X11) | Pergunta antes de instalar |

**Pela janela**, o botão **Instalar dependências** cuida do resto:
- **CMake ≥ 3.21:** instalado via pip no venv, sem sudo, o que funciona até em distros com CMake antigo.
- **Compilador e OpenCV:** pacotes `build-essential`, `pkg-config` e `libopencv-dev`, ou os equivalentes no dnf e no pacman. O launcher pede a senha de administrador pela janela do sistema (`pkexec`).
- **Grupo `dialout`:** o launcher adiciona o seu usuário. A permissão só vale depois de sair da sessão e entrar de novo.
- **Firmware:** `gcc-arm-none-eabi` e `libnewlib-arm-none-eabi` para compilar, e `stlink-tools` para gravar. O `stlink-tools` também instala as regras udev que liberam o ST-Link sem root (reconecte o USB depois). O Ninja vem via pip.

Se o STM32CubeIDE ou o STM32CubeProgrammer estiverem instalados (`/opt/st/...` no Linux, `C:\ST\...` no Windows),
o launcher usa o compilador ARM, o Ninja, o CMake e o `STM32_Programmer_CLI` deles e não instala nada.

Antes de executar, uma janela de confirmação mostra exatamente o que vai ser feito.
O **Galaxy SDK** é proprietário e não tem pacote: o launcher verifica se ele existe e indica onde baixar.

| Botão | O que faz |
| :--- | :--- |
| Verificar ambiente | CMake, compilador, OpenCV, Galaxy SDK, modelo, porta serial e grupo dialout, com a solução de cada item pendente |
| Extrair modelo do zip | Copia o `yolo11_formas.onnx` de `../Vision_Geometrica (Cópia).zip` para `visao/models/` |
| Compilar tudo / só a bridge | `cmake --preset ...` seguido de `cmake --build`, com os caminhos preenchidos na tela |
| Rodar visão | **Deixa tudo pronto e roda**, em sequência: compila tudo (visão + bridge), compila o firmware, grava o STM32 **só se o firmware mudou** desde a última gravação (compara o SHA-256 do `comEdge.bin`) e abre o `VisionTest` a partir de `visao/`, com `--model`, `--camera-sn` e `--port`. Para no primeiro erro |
| Terminal da bridge | Abre o `bridge_example`. A caixa de texto embaixo do log envia comandos (`1`, `0`, `v`, `s`, `q`) |
| Testar handshake com o STM32 | Envia `0xA1` e `0xA0` automaticamente e só dá sucesso se os dois receberem ACK |
| Fluxo completo | Verificar, instalar o que faltar, compilar tudo e testar o STM32, em sequência. Para no primeiro erro |
| Compilar firmware | Compila `firmware_stm32/` para ARM (`cmake --preset firmware`) e gera `build/comEdge.elf`, `.bin` e `.hex` |
| Gravar no STM32 | Compila e grava pelo ST-Link (SWD), verifica e reseta a placa. **Sempre grava**, mesmo sem mudança. Use depois de gravar outro programa pelo CubeIDE, porque o launcher só conhece as gravações que ele mesmo fez. Usa o primeiro gravador que encontrar: STM32CubeProgrammer, `st-flash` ou OpenOCD |

As configurações (caminhos, porta e SN da câmera) ficam salvas entre execuções.

---

## 6. Build

Use um terminal **"Developer PowerShell for VS 2022"**, na raiz desta pasta:

```powershell
$env:OpenCV_DIR = "C:\opencv\build"          # onde está o OpenCVConfig.cmake
cmake --preset windows
cmake --build --preset windows
```

Para compilar **só a bridge**, sem precisar de OpenCV nem de câmera:

```powershell
cmake --preset windows-bridge
cmake --build --preset windows-bridge
```

Testes de unidade (não precisam de câmera, OpenCV nem STM32):

```powershell
ctest --preset windows-bridge
```

No Linux, depois do build, rode `ctest --preset linux`.

Se o Galaxy SDK não estiver no caminho padrão
(`C:\Program Files\Daheng Imaging\GalaxySDK\Development\VC SDK`), informe a pasta:

```powershell
cmake --preset windows -DGALAXY_SDK_DIR="D:/Daheng/GalaxySDK/Development/VC SDK"
```

No Linux, use `cmake --preset linux` e depois `cmake --build --preset linux`.

---

## 7. Execução

O `VisionTest` procura `models/` e grava `roi_config.yml` **na pasta de execução**.
Por isso, rode o programa de dentro de `visao/`:

```powershell
cd visao
..\build\windows\visao\Release\VisionTest.exe --port COM5
```

| Opção | Padrão | Descrição |
| :--- | :--- | :--- |
| `--model <arquivo>` | `models/yolo11_formas.onnx` | Modelo ONNX |
| `--camera-sn <SN>` | `FBR26040067` | Câmera da esteira. Com `--camera-sn ""`, usa a primeira encontrada |
| `--port <COMx>` | auto-detecção | Porta do STM32. Também aceita a variável de ambiente `SERIAL_PORT` |

Para descobrir a porta, abra o *Gerenciador de Dispositivos → Portas (COM e LPT) →
"STMicroelectronics STLink Virtual COM Port (COMx)"*. Outra forma é rodar o `bridge_example`
e usar o comando `s`.

**Teclas:**
- `s` salva a calibração da ROI;
- `ESC` encerra.

As trackbars ajustam a ROI ao vivo. A ROI padrão é **centro X 500, centro Y 260,
largura 380 e altura 380**, em milésimos do quadro. Se existir um `roi_config.yml` salvo
com a tecla `s`, ele tem prioridade. Apague esse arquivo para voltar ao padrão.

Enquanto a varredura está ativa, a janela mostra **o quadro que a IA avaliou**, e não o
quadro mais novo da câmera. A caixa e a ROI aparecem exatamente onde o sistema decidiu.
Por isso, a imagem atualiza no ritmo da inferência, que é mais lento em CPU.

> **Firmware ≥ 1.1.0 obrigatório.** A visão só fica liberada enquanto o STM32 reenvia
> o estado a cada 1 s. Com o firmware antigo, que não reenvia, a tela volta para
> `PAUSADO` 2,5 s depois de cada liberação. Grave o firmware pelo launcher (§5).

Terminal de teste da bridge:

```powershell
.\build\windows\bridge_host\Release\bridge_example.exe COM5
```

---

## 8. Protocolo PC ↔ STM32

| Byte | Sentido | Significado |
| :---: | :---: | :--- |
| `0xA1` | PC → STM32 | Círculo na ROI. O STM32 coloca `PD7 = 0` |
| `0xA0` | PC → STM32 | Fim do ciclo do objeto. O STM32 coloca `PD7 = 1`. Enviado **só quando a esteira volta a andar** depois do `0xA1`, ou após 5 s sem a esteira parar |
| `0xB0` | STM32 → PC | Varredura **pausada**: motor parado **ou** servo ativo. Enviado na mudança de estado, no boot e reenviado a cada 1 s |
| `0xB1` | STM32 → PC | Varredura **liberada**: motor ativo **e** servo inativo. Mesmo esquema de envio. O PC só considera liberado se o último `0xB1` tiver menos de 2,5 s |
| `0x06` | ambos | ACK. O PC tenta até 3 vezes com timeout de 500 ms. O STM32 espera 50 ms |

A pinagem completa e os detalhes dos timers estão em [`firmware_stm32/README.md`](firmware_stm32/README.md).

---

## 9. Origem do código e alterações

| Pasta | Origem |
| :--- | :--- |
| `visao/` | `IA_Vision`, versão de `src/JDJDJD.cpp` (Orchestrator com STM32) |
| `bridge_host/` | [stm32-uart-host-bridge](https://github.com/EduardSilva774/stm32-uart-host-bridge) `host/` @ `5177b90` |
| `firmware_stm32/` | mesmo repositório, `firmware/` @ `5177b90` (alterado só o servo, ver abaixo) |

**Alterações na bridge:**
- **Backend Win32 em `SerialPort`:** usa `CreateFile`, `DCB`, `COMMTIMEOUTS` e I/O overlapped, o que permite ler e escrever ao mesmo tempo (o ACK sai da thread de RX).
- **`AsyncSerialReceiver`:** no Windows, troca o `poll()` por uma leitura com timeout de 100 ms.
- **`PortEnumerator`:** no Windows, enumera as portas pelo SetupAPI, com VID/PID, nome amigável em UTF-8 e porta COM.
- **Linux:** o código POSIX original foi mantido intacto, atrás de `#ifdef`.
- **CMake:** a opção `BRIDGE_BUILD_EXAMPLES` fica desligada quando a bridge é usada como subdiretório. Também foram adicionados o alias `stm32_bridge::serial_lib` e os warnings `/W4` e `-Wall -Wextra`.
- **`bridge_example`:** passa a ter UTF-8 e cores ANSI no console do Windows.
- **Fail-safe da varredura (1.2.0):** `esta_varredura_ativa()` só retorna `true` com a conexão aberta e um `0xB1` recebido há menos de 2,5 s (watchdog). STM32 desconectado, travado ou sem energia significa varredura pausada. O log só registra mudanças de estado, e não cada reenvio.

**Alterações no firmware:**
- **Varredura segura (1.1.0):** a visão só é liberada com o motor ativo e o servo inativo. Antes, o `0xB0` só saía com o motor parado **e** o servo ativo, então a esteira simplesmente desligada continuava liberando a detecção. No boot também era enviado `0xB1` sem olhar o motor. Agora o firmware envia o estado real no boot, a cada mudança e a cada 1 s, e reenvia a cada 200 ms se o PC não confirmar com ACK.
- **UART (1.1.0):** um comando do PC (`0xA1`/`0xA0`) que chega enquanto o STM32 espera um ACK passa a ser entregue à aplicação. Antes, ele era descartado.
- **Servo:** começa e repousa em **1650 µs** e vai até **600 µs** quando a InovaLink aciona o servo (PD6 = 1). As constantes `SERVO_PULSE_REST_US` e `SERVO_PULSE_WORK_US` substituem `IDLE`/`ACTIVE`, que estavam com o sentido trocado em `BSP_StartServo`/`BSP_StopServo`.

**Alterações na visão, em relação ao JDJDJD:**
- **Linha de comando:** `--model`, `--camera-sn` e `--port` substituem os valores fixos no código.
- **Quadro limpo para a IA:** agora a IA recebe o quadro **sem** o desenho da ROI. No JDJDJD, a moldura amarela era desenhada antes do `Vision_SubmitFrame`.
- **CUDA:** a `VisionModule` verifica se a CUDA existe antes de usá-la. Sem isso, o log dizia "CUDA inicializado" mesmo rodando em CPU.
- **Falha na conexão:** se o STM32 não conectar na inicialização, a câmera é liberada.
- **Para de detectar depois do `0xA1` (2.5.0):** a visão envia `0xA1` uma vez e para de detectar até a esteira parar e voltar a andar. Com a esteira parada, nada é enviado. O `0xA0` saía na hora da parada (`0xB0`), no meio do ciclo do servo, e agora só sai quando a esteira volta a andar (`0xB1`), como no protocolo original. Com o `PD7` voltando para 1 no meio do ciclo, a InovaLink podia disparar o servo duas vezes. Há um timeout de 5 s caso a esteira não pare. A HUD mostra `CIRCULO DETECTADO: AGUARDANDO A ESTEIRA PARAR` e `OBJETO SINALIZADO: SERVO EM ACAO`.
- **Um `0xA1` por objeto (2.4.0):** a lógica do sinal virou a máquina de estados `SinalObjeto` (`visao/inc/SinalObjeto.hpp`), com os estados PRONTO, SINALIZADO e AGUARDANDO_SAIDA. A esteira para justamente com o círculo dentro da ROI. Antes, quando ela voltava a andar, o mesmo círculo era detectado de novo e a InovaLink contava 2. Agora, depois de sinalizar, o PC só rearma quando a ROI fica vazia por 3 inferências com a esteira andando. A HUD mostra `OBJETO JA CONTADO: AGUARDANDO SAIR DA ZONA` nesse intervalo. A regra é coberta pelo teste `tests/test_sinal_objeto.cpp` (`ctest`).
- **Decisão no quadro certo (2.3.0):** a `VisionModule` publica cada inferência junto com o quadro em que ela rodou (`Vision_GetLatest`, que substitui `Vision_GetOverlay`). Antes, as caixas de uma inferência antiga eram desenhadas e testadas contra a ROI no quadro **atual**. Com a esteira andando, a caixa ficava atrás do círculo, e um círculo que já tinha saído da área ainda era detectado dentro dela.
- **Confirmação do sinal (2.3.0):** o `0xA1` só sai com 2 inferências seguidas com círculo na ROI, e o `0xA0` só com 3 seguidas sem círculo. Antes, uma única inferência que "piscava" derrubava e religava o sinal (`0xA1`, `0xA0`, `0xA1`: a detecção dupla). A contagem considera só inferências novas, e não cada quadro da câmera.
- **NMS 0,45 (2.3.0):** caixas da mesma classe com sobreposição acima de 45% viram uma só. Com 0,60, duas caixas no mesmo círculo sobreviviam.
- **ROI padrão (2.3.0):** centro (500, 260) e tamanho 380×380, em milésimos do quadro.
- **Sem detecção antiga (2.2.0):** na pausa e na retomada, `Vision_Reset()` descarta o quadro pendente, as detecções publicadas e o resultado de inferências em andamento. Assim, nenhuma detecção de antes da pausa dispara `0xA1` depois dela.
- **Limpeza:**
  - a função `ObterNomeForma`, que não era usada, foi removida;
  - o desenho de caixa duplicado virou `DesenharDeteccao()`;
  - o comentário de exposição foi corrigido (30 ms);
  - `nDevNum <= 0` virou `== 0`;
  - a `VisionModule.hpp` ganhou documentação.
- **CMake:**
  - o Galaxy SDK passa a ser configurável, em vez de um caminho absoluto de `/home/iona50`;
  - a bridge é integrada via `add_subdirectory`;
  - as DLLs do OpenCV são copiadas para junto do `.exe`;
  - o *working directory* do Visual Studio aponta para `visao/`.

---

## 10. Pendências conhecidas

Estes pontos ficam documentados em vez de alterados, porque mudam o comportamento
da esteira e precisam ser validados na bancada.

Os itens "`0xB1` só no boot" e "a pausa depende do servo" foram **resolvidos** no firmware 1.1.0
e na bridge 1.2.0 (ver §9).

1. **O firmware não bate com a documentação.** O README do firmware diz `PD0 = 0` para motor ativo e `PD5 = 1` para a direita, mas o `bsp.c` faz o contrário. Confirme com a InovaLink qual lado está certo.
2. **Janela entre o `0xA1` e o `0xB0`.** Depois de enviar `0xA1`, o PC continua detectando até a InovaLink reagir.
3. **Memória da bridge.** O `MessageBufferObserver` interno da bridge guarda todo byte recebido e nunca é esvaziado após a conexão. Como o volume é de poucos bytes por ciclo, não é crítico, mas deve ser limpo em uso contínuo.

---

## 11. Solução de problemas

| Sintoma | Causa provável | O que fazer |
| :--- | :--- | :--- |
| `Nenhuma camera Daheng encontrada` | Câmera em outra sub-rede ou bloqueada pelo firewall | Use o *GxGigeIPConfig* para colocar câmera e placa de rede na mesma sub-rede e libere o app no firewall |
| `Falha ao abrir porta serial: COMx` | Porta aberta em outro programa (CubeIDE, monitor serial) | Feche o terminal serial ou a sessão de debug do CubeIDE |
| `Nenhum microcontrolador STM32 detectado` | ST-Link com outro PID ou adaptador USB-TTL | Passe `--port COMx` |
| Tela em `PAUSADO: ESTEIRA PARADA` com a esteira girando | Firmware antigo (< 1.1.0) sem reenvio do estado, ou motor desligado do ponto de vista do STM32 (PD0) | Grave o firmware pelo launcher e confira no `bridge_example` (comando `v`) se chega `0xB1` |
| Visão detectando com a esteira parada | Firmware antigo (< 1.1.0), ou o STM32 acha que o motor está ligado (PD0) | Grave o firmware 1.1.0 ou mais novo. Com a esteira parada, a tela tem que mostrar `PAUSADO`. Se não mostrar, confira no `bridge_example` (comando `v`) e veja a pendência 1 da §10 (polaridade do PD0) |
| Log com `CIRCULO detectado na ROI!` ou `Resetando sinal para SEM OBJETO`, ou "círculo fantasma" com o tamanho do círculo real | Está rodando um **executável antigo** da visão (anterior a 2.3.0), e não o build desta pasta | A primeira linha do log tem que ser `[SICOV] VisionTest 2.5.1` ou mais nova. Rode pelo launcher (**Compilar tudo** e depois **Rodar visão**) ou por `build/linux/visao/VisionTest`, de dentro de `visao/` |
| InovaLink conta 2 ou servo atua 2 vezes para um único círculo | Visão < 2.5.0: o mesmo círculo era sinalizado de novo, ou o `0xA0` saía no meio do ciclo do servo | Atualize para a visão 2.5.0 ou mais nova (**Compilar tudo** no launcher). No log, cada objeto deve gerar exatamente um `0xA1` e, depois de `esteira voltou a andar`, um `0xA0` |
| ROI diferente do padrão 500/260/380/380 | `roi_config.yml` antigo na pasta `visao/` | Apague o arquivo ou ajuste as trackbars e salve com `s` |
| `Timeout aguardando ACK` | Firmware não gravado, baud diferente ou VCP sem ligação com PA9/PA10 | Teste com o `bridge_example` e confira a ligação da USART1 com o ST-Link |
| Build do firmware no CubeIDE falha com "No such file" | Caminho com espaço ou acento ("Apresentação Esteira") | Use o botão **Gravar no STM32** do launcher, que aceita esse caminho, ou copie `firmware_stm32/` para um caminho sem espaços |
| `No debug probe detected` ao gravar | ST-Link desconectado, sem permissão USB ou ocupado | Reconecte o ST-Link, feche o debug do CubeIDE e, no Linux, reconecte depois de instalar o `stlink-tools` |
| `opencv_world*.dll não encontrada` | DLL fora do PATH | Recompile (o pós-build copia a DLL) ou adicione `C:\opencv\build\x64\vc16\bin` ao PATH |
