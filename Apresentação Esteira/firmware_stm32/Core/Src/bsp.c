/**
 * @file    bsp.c
 * @author  iona50
 * @date    21 de out. de 2026
 * @brief   Implementação dos protótipos e esqueletos de controle da BSP (Motor, Servo e App).
 *
 * Este arquivo contém os esqueletos das funções de controle de atuadores
 * (motor DC/passo e servomotor) e inicialização da aplicação, servindo como base
 * para a implementação dos drivers de hardware da placa STM32F407VET6.
 */

#include "bsp.h"

extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim4;

#define DEBOUNCE_TIME_MS    10U

#define SCAN_RETRY_MS        200U   /* Reenvio do estado de varredura sem ACK do Host */
#define SCAN_SYNC_PERIOD_MS  1000U  /* Reenvio periodico do estado ja confirmado      */

/* Largura de pulso do servo em us (TIM4: tick de 1 us, periodo de 20 ms / 50 Hz) */
#define SERVO_PULSE_REST_US     1650U   //Posição inicial / repouso (servo inativo, PD6 = 0)
#define SERVO_PULSE_WORK_US     600U    //Posição de atuação na peça (servo ativo, PD6 = 1)

/* --- Variáveis Estáticas da Comunicação UART com o Host --- */
static uint8_t rx_cmd_buffer[1];
static volatile uint8_t object_detected_flag = 0;
static volatile uint8_t pending_ack = 0;

/* --- Variáveis de Estado dos Atuadores para Coordenação com o Host --- */
static uint8_t motor_is_active = 0;
static uint8_t servo_is_active = 0;

/* --- Protótipos das Funções Estáticas Internas --- */
static void ProcessMotorEnable(void);
static void ProcessMotorDirection(void);
static void ProcessMotorSpeed(void);
static void ProcessServoEnable(void);
static void ProcessObjectDetection(void);
static void ProcessScanControl(void);
static void UartRxCallback(uint16_t size);
static void ProcessUartHandshake(void);

/* ==============================================================================
 * FUNÇÕES ESTÁTICAS DE PROCESSAMENTO DE ENTRADAS
 * ============================================================================== */

/**
 * @brief  Monitora e processa o pino de habilitação do motor (PD0 - enableMotorIn_Pin).
 * @note   Nível lógico 0 (RESET) chama BSP_StartMotor().
 *         Nível lógico 1 (SET) chama BSP_StopMotor().
 *         Disparo executado apenas na detecção de transição (borda).
 * @retval None
 */
static void ProcessMotorEnable(void)
{
	static uint8_t last_enable_state = 0xFF;

	uint8_t current_enable = (uint8_t)HAL_GPIO_ReadPin(enableMotorIn_GPIO_Port, enableMotorIn_Pin);
	if (current_enable != last_enable_state)
	{
		last_enable_state = current_enable;
		if (current_enable == GPIO_PIN_SET)
		{
			motor_is_active = 1;
			BSP_StartMotor();
		}
		else
		{
			motor_is_active = 0;
			BSP_StopMotor();
		}
	}
}

/**
 * @brief  Monitora e processa o pino de sentido de rotação do motor (PD5 - directionIn_Pin).
 * @note   Nível lógico 0 (RESET) chama BSP_SetMotorDir(LEFT).
 *         Nível lógico 1 (SET) chama BSP_SetMotorDir(RIGHT).
 *         Disparo executado apenas na detecção de transição (borda).
 * @retval None
 */
static void ProcessMotorDirection(void)
{
	static uint8_t last_dir_state = 0xFF;

	uint8_t current_dir = (uint8_t)HAL_GPIO_ReadPin(directionIn_GPIO_Port, directionIn_Pin);
	if (current_dir != last_dir_state)
	{
		last_dir_state = current_dir;
		if (current_dir == GPIO_PIN_SET)
		{
			BSP_SetMotorDir(LEFT);
		}
		else
		{
			BSP_SetMotorDir(RIGHT);
		}
	}
}

/**
 * @brief  Monitora e processa os pinos exclusivos de velocidade do motor (PD1 a PD4).
 * @note   Varre os pinos speed1 a speed4 e dispara BSP_SetMotorSpeed() com o enum
 *         correspondente (SPEED1..SPEED4) apenas quando houver mudança do índice ativo.
 * @retval None
 */
static void ProcessMotorSpeed(void)
{
	static int8_t last_speed_state = -1;
	int8_t current_speed = SPEED1;

	if (HAL_GPIO_ReadPin(speed1_GPIO_Port, speed1_Pin) == GPIO_PIN_SET)
	{
		current_speed = (int8_t)SPEED1;
	}
	else if (HAL_GPIO_ReadPin(speed2_GPIO_Port, speed2_Pin) == GPIO_PIN_SET)
	{
		current_speed = (int8_t)SPEED2;
	}
	else if (HAL_GPIO_ReadPin(speed3_GPIO_Port, speed3_Pin) == GPIO_PIN_SET)
	{
		current_speed = (int8_t)SPEED3;
	}
	else if (HAL_GPIO_ReadPin(speed4_GPIO_Port, speed4_Pin) == GPIO_PIN_SET)
	{
		current_speed = (int8_t)SPEED4;
	}

	if (current_speed != -1 && current_speed != last_speed_state)
	{
		last_speed_state = current_speed;
		BSP_SetMotorSpeed((motorSpeed_e)current_speed);
	}
}

/**
 * @brief  Monitora e processa o pino de controle do servo (PD6 - servoControl_Pin).
 * @note   Nível lógico 1 (SET) chama BSP_StartServo() (Ativo).
 *         Nível lógico 0 (RESET) chama BSP_StopServo() (Inativo).
 *         Disparo executado apenas na detecção de transição (borda).
 * @retval None
 */
static void ProcessServoEnable(void)
{
	static uint8_t last_servo_state = 0xFF;

	uint8_t current_servo = (uint8_t)HAL_GPIO_ReadPin(servoControl_GPIO_Port, servoControl_Pin);
	if (current_servo != last_servo_state)
	{
		last_servo_state = current_servo;
		if (current_servo == GPIO_PIN_SET)
		{
			servo_is_active = 1;
			BSP_StartServo();
		}
		else
		{
			servo_is_active = 0;
			BSP_StopServo();
		}
	}
}

/**
 * @brief  Atualiza o pino de indicação de objeto (PD7 - foundObject_Pin) conforme
 *         a flag object_detected_flag recebida do Host via UART.
 * @note   Nível 0 (RESET): Objeto encontrado / detectado.
 *         Nível 1 (SET): Sem objeto / varredura normal.
 *         A alteração ocorre exclusivamente sob comando explícito do Host.
 * @retval None
 */
static void ProcessObjectDetection(void)
{
	static uint8_t last_detection_state = 0xFF;

	if (object_detected_flag != last_detection_state)
	{
		last_detection_state = object_detected_flag;
		if (object_detected_flag == 1U)
		{
			HAL_GPIO_WritePin(foundObject_GPIO_Port, foundObject_Pin, GPIO_PIN_RESET);
		}
		else
		{
			HAL_GPIO_WritePin(foundObject_GPIO_Port, foundObject_Pin, GPIO_PIN_SET);
		}
	}
}

/**
 * @brief  Mantém o Host sincronizado com a permissão de varredura da visão.
 * @note   Regra de segurança: a visão só pode detectar com a esteira em movimento,
 *         ou seja, motor ativo E servo inativo. Qualquer outra combinação (esteira
 *         desligada, servo atuando, boot sem motor) envia StopScan (0xB0).
 *         - Mudança de estado: envia imediatamente.
 *         - Sem ACK do Host: reenvia a cada SCAN_RETRY_MS.
 *         - Estado confirmado: reenvia a cada SCAN_SYNC_PERIOD_MS, para que um Host
 *           conectado depois do boot (ou que perdeu um byte) nunca fique dessincronizado.
 * @retval None
 */
static void ProcessScanControl(void)
{
	static int8_t   last_sent  = -1;   /* -1: nada enviado ainda (boot) */
	static int8_t   last_acked = -1;
	static uint32_t last_send_tick = 0;

	const int8_t scan_allowed = (motor_is_active && !servo_is_active) ? 1 : 0;
	const uint32_t elapsed = HAL_GetTick() - last_send_tick;

	const uint8_t changed   = (scan_allowed != last_sent);
	const uint8_t retry     = (scan_allowed != last_acked) && (elapsed >= SCAN_RETRY_MS);
	const uint8_t heartbeat = (elapsed >= SCAN_SYNC_PERIOD_MS);

	if (changed || retry || heartbeat)
	{
		HAL_StatusTypeDef status = scan_allowed ? BSP_HostRequestResumeScan()
		                                        : BSP_HostRequestStopScan();
		last_sent = scan_allowed;
		last_acked = (status == HAL_OK) ? scan_allowed : -1;
		last_send_tick = HAL_GetTick();
	}
}

/**
 * @brief  Callback acionado pela interrupção da UART ao receber comando de 1 byte do Host.
 * @note   Executa em contexto de interrupção (ISR). Não atua diretamente no pino, apenas
 *         gerencia a flag de objeto e a flag de handshake para a BSP_InitAPP.
 * @param  size Quantidade de bytes recebidos pelo UartHandler (esperado: 1).
 * @retval None
 */
static void UartRxCallback(uint16_t size)
{
	if (size >= 1U)
	{
		uint8_t cmd = rx_cmd_buffer[0];

		if (cmd == CMD_OBJETO_ENCONTRADO || cmd == '1')
		{
			object_detected_flag = 1;
			pending_ack = 1;
		}
		else if (cmd == CMD_SEM_OBJETO || cmd == '0')
		{
			object_detected_flag = 0;
			pending_ack = 1;
		}
	}
}

/**
 * @brief  Processa o envio de confirmação mútua (Handshake ACK) para o Host fora da ISR.
 * @retval None
 */
static void ProcessUartHandshake(void)
{
	if (pending_ack)
	{
		pending_ack = 0;
		uint8_t ack_val = HOST_EXPECTED_ACK;
		UART_Handler_Send(&ack_val, 1U, HOST_ACK_TIMEOUT_MS);
	}
}

/* ==============================================================================
 * INICIALIZAÇÃO GERAL DA APLICAÇÃO
 * ============================================================================== */

/**
 * @brief  Retorna o estado atual da flag de detecção de objeto recebida do Host.
 * @retval 1 se objeto detectado, 0 se sem objeto.
 */
uint8_t BSP_IsObjectDetected(void)
{
	return object_detected_flag;
}

/**
 * @brief  Inicializa os módulos da BSP, inicia a UART via interrupção e roda o loop de polling.
 * @note   Aplica um debounce único de 10 ms para todo o grupo de entradas a cada ciclo.
 * @retval None
 */
void BSP_InitAPP(void)
{
	BSP_InitMotor();
	BSP_InitServo();

	HAL_GPIO_WritePin(foundObject_GPIO_Port, foundObject_Pin, GPIO_PIN_SET);

	UART_Handler_StartReceive_IT(rx_cmd_buffer, 1U, UartRxCallback);

	/* Sem ResumeScan incondicional no boot: o primeiro ProcessScanControl() envia o
	 * estado real (StopScan se a esteira estiver parada). */

	while(1)
	{
		ProcessMotorEnable();
		ProcessMotorDirection();
		ProcessMotorSpeed();
		ProcessServoEnable();
		ProcessObjectDetection();
		ProcessScanControl();
		ProcessUartHandshake();

		/* Debounce único de 10 ms para todo o conjunto de entradas */
		HAL_Delay(DEBOUNCE_TIME_MS);
	}
}

/* ==============================================================================
 * CONTROLE DE MOTOR
 * ============================================================================== */

/**
 * @brief  Inicializa os pinos GPIO e periféricos de temporização associados ao motor.
 * @retval None
 */
void BSP_InitMotor(void)
{
    BSP_SetMotorDir(LEFT);
    BSP_SetMotorSpeed(SPEED1);
    BSP_StopMotor();
}

/**
 * @brief  Configura o nível de velocidade do motor.
 * @param  speed Nível de velocidade desejado (SPEED1, SPEED2, SPEED3, SPEED4).
 * @retval None
 */
void BSP_SetMotorSpeed(motorSpeed_e speed)
{
    static const uint16_t arr_table[]   = {1999, 1499, 1199, 799};
    static const uint16_t pulse_table[] = {1000, 750,  600,  400};
//    static const uint16_t arr_table[]   = {2999, 1999, 999, 499};
//    static const uint16_t pulse_table[] = {1500, 1000,  500,  250};
    if (speed <= SPEED4)
    {
        __HAL_TIM_SET_AUTORELOAD(&htim3, arr_table[speed]);
        __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, pulse_table[speed]);
    }
}

/**
 * @brief  Configura o sentido de rotação do motor.
 * @param  dir Sentido de direção (LEFT para esquerda ou RIGHT para direita).
 * @retval None
 */
void BSP_SetMotorDir(motorDir_e dir)
{
	GPIO_PinState pin_state = (dir == RIGHT) ? GPIO_PIN_RESET : GPIO_PIN_SET;
	HAL_GPIO_WritePin(directionMotorOut_GPIO_Port, directionMotorOut_Pin, pin_state);
}

/**
 * @brief  Interrompe o funcionamento do motor (parada / desacionamento).
 * @retval None
 */
void BSP_StopMotor(void)
{
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1);
    HAL_GPIO_WritePin(enableMotorOut_GPIO_Port, enableMotorOut_Pin, GPIO_PIN_SET);}

/**
 * @brief  Inicia ou retoma o acionamento do motor conforme as configurações atuais.
 * @retval None
 */
void BSP_StartMotor(void)
{
    HAL_GPIO_WritePin(enableMotorOut_GPIO_Port, enableMotorOut_Pin, GPIO_PIN_RESET);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);}

/* ==============================================================================
 * CONTROLE DE SERVOMOTOR
 * ============================================================================== */

/**
 * @brief  Inicializa o periférico de controle PWM e pino de sinal do servomotor.
 * @retval None
 */
void BSP_InitServo(void)
{
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, SERVO_PULSE_REST_US);
    HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1);
}

/**
 * @brief  Move o servomotor para a posição de atuação (ativo, 600 us).
 * @retval None
 */
void BSP_StartServo(void)
{
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, SERVO_PULSE_WORK_US);
}

/**
 * @brief  Retorna o servomotor para a posição de repouso (inativo, 1650 us).
 * @retval None
 */
void BSP_StopServo(void)
{
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, SERVO_PULSE_REST_US);
}

/* ==============================================================================
 * COMUNICAÇÃO COM O HOST (CONTROLE DE VARREDURA)
 * ============================================================================== */

/**
 * @brief  Envia comando solicitando ao Host parar a busca de objetos e aguarda ACK (0x06).
 * @retval HAL_StatusTypeDef HAL_OK se o Host confirmou com ACK (0x06) em até 50 ms.
 */
HAL_StatusTypeDef BSP_HostRequestStopScan(void)
{
	uint8_t cmd = CMD_HOST_STOP_SCAN;
	return UART_Handler_Send_WaitACK(&cmd, 1U, HOST_EXPECTED_ACK, HOST_ACK_TIMEOUT_MS);
}

/**
 * @brief  Envia comando solicitando ao Host retomar a varredura e aguarda ACK (0x06).
 * @retval HAL_StatusTypeDef HAL_OK se o Host confirmou com ACK (0x06) em até 50 ms.
 */
HAL_StatusTypeDef BSP_HostRequestResumeScan(void)
{
	uint8_t cmd = CMD_HOST_RESUME_SCAN;
	return UART_Handler_Send_WaitACK(&cmd, 1U, HOST_EXPECTED_ACK, HOST_ACK_TIMEOUT_MS);
}

