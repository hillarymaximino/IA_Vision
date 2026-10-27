# Firmware STM32F407VET6 - UART Host Bridge

Firmware em **C (STM32Cube HAL)** desenvolvido para o microcontrolador **STM32F407VET6**. Atua como a ponte física de tempo real entre o **Host PC (Visão Computacional)** e a **Controladora Externa (PLC / Célula Automatizada)**, gerenciando sensores, motor de esteira e servomotor.

---

## 📑 Sumário

- [1. Estrutura Modular de Arquivos](#1-estrutura-modular-de-arquivos)
- [2. Funcionamento Operacional do Firmware](#2-funcionamento-operacional-do-firmware)
  - [Máquina de Estados e Driver Serial (`UartHandler`)](#máquina-de-estados-e-driver-serial-uarthandler)
  - [Loop Principal e Debounce Unificado (`bsp.c`)](#loop-principal-e-debounce-unificado-bspc)
  - [Lógica do Pino de Objeto e Controle de Varredura](#lógica-do-pino-de-objeto-e-controle-de-varredura)
- [3. Mapeamento Completo de Pinos, Tags e Configurações](#3-mapeamento-completo-de-pinos-tags-e-configurações)
- [4. Funções Pendentes de Implementação (Drivers de Atuadores)](#4-funções-pendentes-de-implementação-drivers-de-atuadores)

---

## 1. Estrutura Modular de Arquivos

O código foi dividido em camadas desacopladas para isolar a infraestrutura de hardware da lógica de aplicação:

```text
firmware/
├── Core/
│   ├── Inc/
│   │   ├── main.h          # Macros geradas pelo CubeMX (tags de pinos, structs HAL)
│   │   ├── bsp.h           # Protótipos de atuadores, enums de motor e opcodes UART
│   │   └── UartHandler.h   # Driver de baixo nível da USART1 (Polling / IT / ACK)
│   └── Src/
│       ├── main.c          # Inicialização do clock (HSI/PLL 84MHz) e periféricos HAL
│       ├── bsp.c           # Aplicação central, debounce, rotinas de inputs e atuadores
│       └── UartHandler.c   # Máquina de estados UART, ISR e controle de fluxo por ACK
├── Drivers/                # Drivers CMSIS e STM32F4xx HAL
└── teste serial.ioc        # Arquivo de configuração de pinos e timers do CubeMX
```

---

### Build por linha de comando (sem abrir o CubeIDE)

Além do projeto do STM32CubeIDE, a pasta tem um `CMakeLists.txt` com as mesmas opções da
configuração Debug: Cortex-M4 com FPU hard, `USE_HAL_DRIVER`, `STM32F407xx` e `STM32F407VETX_FLASH.ld`.
É ele que o launcher (`tools/`) usa no botão **Gravar no STM32**.

```bash
cmake --preset firmware
cmake --build --preset firmware
```

Esses comandos geram `build/comEdge.elf`, `.bin` e `.hex`. Para gravar manualmente, use um destes:

```bash
STM32_Programmer_CLI -c port=SWD -w build/comEdge.elf -v -rst
st-flash --reset write build/comEdge.bin 0x08000000
```

O compilador `arm-none-eabi-gcc` é procurado no PATH. Para usar o que vem com o CubeIDE, passe
`-DARM_TOOLCHAIN_DIR=<pasta bin do gnu-tools-for-stm32>`. O launcher faz isso sozinho.

---

## 2. Funcionamento Operacional do Firmware

### Máquina de Estados e Driver Serial (`UartHandler`)
- **Periférico:** USART1 (`PA9` TX, `PA10` RX), configurada em `115200 bps 8N1`.
- **Recepção Contínua em Background:** O driver inicia a escuta via interrupção de 1 byte com `UART_Handler_StartReceive_IT(rx_cmd_buffer, 1, UartRxCallback)`.
- **Prevenção de Travamento em ISR:** Ao receber um comando do Host (`0xA1` ou `0xA0`), a rotina `UartRxCallback` apenas armazena o byte e levanta a flag atômica `pending_ack = 1`. A transmissão do byte de resposta `0x06` (ACK) é executada fora da interrupção pelo loop da aplicação em `ProcessUartHandshake()`.
- **Envio com Espera de ACK:** Ao transmitir comandos para o Host (`StopScan` `0xB0` e `ResumeScan` `0xB1`), o microcontrolador comuta internamente para `UART_HANDLER_STATE_WAITING_ACK` e aguarda o ACK (`0x06`) por até 50 ms antes de retornar `HAL_OK` ou `HAL_TIMEOUT`.

### Loop Principal e Debounce Unificado (`bsp.c`)
A execução da aplicação ocorre no laço da função `BSP_InitAPP()`:
1. Coloca `foundObject` (`PD7`) em nível alto (`1` / sem objeto).
2. Dispara `BSP_HostRequestScan()` transmitindo `0xB1` para avisar ao Host que o micro inicializou.
3. Entra no laço infinito executando sequencialmente:
   - `ProcessMotorEnable()`: Leitura do pino de habilitação do motor (`PD0`).
   - `ProcessMotorDirection()`: Leitura do sentido de giro do motor (`PD5`).
   - `ProcessMotorSpeed()`: Varredura dos pinos de velocidade (`PD1` a `PD4`).
   - `ProcessServoEnable()`: Leitura do sinal de comando do servo (`PD6`).
   - `ProcessObjectDetection()`: Atualização física do pino `PD7` conforme a flag serial.
   - `ProcessScanControl()`: Máquina de transição que envia `StopScan` ou `ResumeScan`.
   - `ProcessUartHandshake()`: Despacho de confirmações ACK pendentes.
   - `HAL_Delay(DEBOUNCE_TIME_MS)`: **Debounce único centralizado de 10 ms** que filtra repiques mecânicos de todas as entradas simultaneamente sem busy-waiting desnecessário.

### Lógica do Pino de Objeto e Controle de Varredura
- **`ProcessObjectDetection()`:** Quando a flag `object_detected_flag` muda sob comando da serial:
  - Se `1` (Host enviou `0xA1`): seta `foundObject_Pin` (`PD7`) para `GPIO_PIN_RESET` (`0`).
  - Se `0` (Host enviou `0xA0`): seta `foundObject_Pin` (`PD7`) para `GPIO_PIN_SET` (`1`).
  - *Regra estrita:* Esse pino **só** é comutado por comando do Host.
- **`ProcessScanControl()`:** Detecta transições coordenadas entre os estados do motor e do servo:
  - Quando o motor para e o servo é ativado (`!motor_is_active && servo_is_active`): dispara `BSP_HostRequestStopScan()` (`0xB0`).
  - Quando o servo desliga e o motor é religado (`motor_is_active && !servo_is_active`): dispara `BSP_HostRequestResumeScan()` (`0xB1`).

---

## 3. Mapeamento Completo de Pinos, Tags e Configurações

Configurações geradas via STM32CubeMX e declaradas em `Core/Inc/main.h`:

| Tag / Macro no Código | Pino Físico | Modo GPIO | Periférico | Nível Lógico | Ação / Comportamento no Sistema |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **`enableMotorIn_Pin`** | **`PD0`** | Entrada (Pull-Down) | GPIO | **`0` (RESET)** | **Motor Ativo:** Dispara `BSP_StartMotor()`, `motor_is_active = 1`. |
| | | | | **`1` (SET)** | **Motor Parado:** Dispara `BSP_StopMotor()`, `motor_is_active = 0`. |
| **`speed1_Pin`** | **`PD1`** | Entrada (Pull-Down) | GPIO | **`1` (SET)** | Seleciona **Velocidade 1** (`SPEED1`) via `BSP_SetMotorSpeed(SPEED1)`. |
| **`speed2_Pin`** | **`PD2`** | Entrada (Pull-Down) | GPIO | **`1` (SET)** | Seleciona **Velocidade 2** (`SPEED2`) via `BSP_SetMotorSpeed(SPEED2)`. |
| **`speed3_Pin`** | **`PD3`** | Entrada (Pull-Down) | GPIO | **`1` (SET)** | Seleciona **Velocidade 3** (`SPEED3`) via `BSP_SetMotorSpeed(SPEED3)`. |
| **`speed4_Pin`** | **`PD4`** | Entrada (Pull-Down) | GPIO | **`1` (SET)** | Seleciona **Velocidade 4** (`SPEED4`) via `BSP_SetMotorSpeed(SPEED4)`. |
| **`directionIn_Pin`** | **`PD5`** | Entrada (Pull-Down) | GPIO | **`0` (RESET)** | Sentido de rotação para a **Esquerda** (`BSP_SetMotorDir(LEFT)`). |
| | | | | **`1` (SET)** | Sentido de rotação para a **Direita** (`BSP_SetMotorDir(RIGHT)`). |
| **`servoControl_Pin`** | **`PD6`** | Entrada (Pull-Down) | GPIO | **`0` (RESET)** | **Servo Inativo:** Dispara `BSP_StopServo()`, `servo_is_active = 0`. |
| | | | | **`1` (SET)** | **Servo Ativo:** Dispara `BSP_StartServo()`, `servo_is_active = 1`. |
| **`foundObject_Pin`** | **`PD7`** | Saída Digital | GPIO | **`0` (RESET)** | **Objeto Encontrado:** Ativado via `0xA1` do Host. Avisa à controladora. |
| | | | | **`1` (SET)** | **Sem Objeto / Repouso:** Estado inicial e retornado via `0xA0` do Host. |
| **`enableMotorOut_Pin`** | **`PD8`** | Saída Digital | GPIO | Digital | Habilita/Desabilita fisicamente o driver do motor de passo/DC. |
| **`directionMotorOut_Pin`**| **`PD9`** | Saída Digital | GPIO | Digital | Sinal de sentido físico no driver do motor. |
| **`MotorStepPin_Pin`** | **`PA6`** | Alternate Function (PWM) | **TIM3_CH1** | Pulsos | Geração de pulsos de passo com frequência proporcional à velocidade. |
| **`ServoPwmPin_Pin`** | **`PD12`**| Alternate Function (PWM) | **TIM4_CH1** | Pulso 50 Hz | Sinal PWM (1 ms a 2 ms) para controle angular do servomotor. |
| **`USART1_TX`** | **`PA9`** | Alternate Function | **USART1** | Serial TX | Transmissão de comandos e ACKs ao Host PC. |
| **`USART1_RX`** | **`PA10`**| Alternate Function | **USART1** | Serial RX | Recepção assíncrona de comandos e ACKs do Host PC. |

> **Nota sobre velocidade:** Os pinos `speed1` a `speed4` são mutuamente exclusivos (apenas um ativo em nível alto por vez).

---

## 4. Funções Pendentes de Implementação (Drivers de Atuadores)

Em `firmware/Core/Src/bsp.c`, a lógica de controle e transições está pronta, restando implementar as rotinas de baixo nível nos esqueletos das funções abaixo:

### 1. `void BSP_InitMotor(void)`
- **Objetivo:** Inicialização física do subsistema do motor.
- **O que deve fazer:**
  - Garantir os pinos digitais `enableMotorOut_Pin` (`PD8`) e `directionMotorOut_Pin` (`PD9`) em estado seguro inicial (desligado).
  - Inicializar o Timer 3 (`htim3`), configurando o canal 1 em modo PWM no pino `PA6` (`MotorStepPin`).

### 2. `void BSP_SetMotorSpeed(motorSpeed_e speed)`
- **Objetivo:** Ajustar a frequência de clock dos passos do motor conforme a velocidade selecionada (`SPEED1` a `SPEED4`).
- **O que deve fazer:**
  - Definir a frequência desejada para cada um dos 4 níveis alterando o registrador `htim3.Instance->ARR` (Auto-Reload Register) e ajustando o Prescaler se necessário.
  - Ajustar o `htim3.Instance->CCR1` para 50% de duty cycle (`ARR / 2`) para produzir pulsos de passo perfeitamente quadrados.

### 3. `void BSP_SetMotorDir(motorDir_e dir)`
- **Objetivo:** Comutar o pino de sentido de giro do driver do motor.
- **O que deve fazer:**
  - Se `dir == LEFT`: chamar `HAL_GPIO_WritePin(directionMotorOut_GPIO_Port, directionMotorOut_Pin, GPIO_PIN_RESET)`.
  - Se `dir == RIGHT`: chamar `HAL_GPIO_WritePin(directionMotorOut_GPIO_Port, directionMotorOut_Pin, GPIO_PIN_SET)`.

### 4. `void BSP_StartMotor(void)`
- **Objetivo:** Colocar a esteira/motor em movimento.
- **O que deve fazer:**
  - Ativar o pino de habilitação do driver: `HAL_GPIO_WritePin(enableMotorOut_GPIO_Port, enableMotorOut_Pin, GPIO_PIN_RESET)` (ou `SET`, conforme o driver).
  - Iniciar o envio de pulsos PWM via canal de temporização: `HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1)`.

### 5. `void BSP_StopMotor(void)`
- **Objetivo:** Parar o motor imediatamente.
- **O que deve fazer:**
  - Interromper a geração de pulsos de passo: `HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1)`.
  - Desabilitar a ponte de potência: `HAL_GPIO_WritePin(enableMotorOut_GPIO_Port, enableMotorOut_Pin, GPIO_PIN_SET)`.

### Servomotor (implementado)

O TIM4 conta com tick de 1 µs (84 MHz / Prescaler 84) e período de 20 ms (50 Hz). Por isso,
os pulsos em `bsp.c` são escritos diretamente em microssegundos:

| Constante | Pulso | Quando |
| :--- | :---: | :--- |
| `SERVO_PULSE_REST_US` | **1650 µs** | Posição inicial (boot) e repouso: `BSP_InitServo()` / `BSP_StopServo()` (PD6 = 0) |
| `SERVO_PULSE_WORK_US` | **600 µs** | Atuação na peça: `BSP_StartServo()` (PD6 = 1) |

- `BSP_InitServo()`: carrega o pulso de repouso e inicia o PWM no `PD12` (TIM4_CH1).
- `BSP_StartServo()`: move o servo de 1650 µs para 600 µs.
- `BSP_StopServo()`: retorna o servo para 1650 µs. O PWM continua ativo para manter o torque na posição.
