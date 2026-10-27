/**
 * @file    bsp.h
 * @author  iona50
 * @date    21 de out. de 2026
 * @brief   Definições de tipos, enumerações e protótipos da BSP (Motor, Servo e App).
 *
 * Este cabeçalho declara as interfaces de controle de hardware da placa
 * STM32F407VET6, desacoplando a lógica de atuadores das rotinas de comunicação UART
 * (agora isoladas no módulo UartHandler).
 */

#ifndef INC_BSP_H_
#define INC_BSP_H_

#include "main.h"
#include "UartHandler.h"

/**
 * @brief Enumeração para os níveis de velocidade do motor.
 * @note  Adcionar valores de frequência.
 */
typedef enum {
    SPEED1 = 0, /**< Nível 1 de velocidade */
    SPEED2 = 1, /**< Nível 2 de velocidade */
    SPEED3 = 2, /**< Nível 3 de velocidade */
    SPEED4 = 3  /**< Nível 4 de velocidade */
} motorSpeed_e;

/**
 * @brief Enumeração para o sentido de rotação do motor.
 */
typedef enum {
    LEFT,  /**< Rotação para a esquerda */
    RIGHT  /**< Rotação para a direita */
} motorDir_e;

/* ==============================================================================
 * PROTÓTIPOS DAS FUNÇÕES DA BSP
 * ============================================================================== */

/**
 * @brief  Inicializa os módulos e subsistemas gerais da aplicação na BSP.
 * @retval None
 */
void BSP_InitAPP(void);

/**
 * @brief  Inicializa os pinos GPIO e periféricos de temporização associados ao motor.
 * @retval None
 * @note Usar timer4 para o servo
 */
void BSP_InitMotor(void);

/**
 * @brief  Configura o nível de velocidade do motor.
 * @param  speed Nível de velocidade desejado (SPEED1, SPEED2, SPEED3, SPEED4).
 * @note ajustar valores do timer 3
 * @retval None
 */
void BSP_SetMotorSpeed(motorSpeed_e speed);

/**
 * @brief  Configura o sentido de rotação do motor.
 * @param  dir Sentido de direção (LEFT para esquerda ou RIGHT para direita).
 * @note usar directionMotorOut_Pin
 * @retval None
 */

void BSP_SetMotorDir(motorDir_e dir);

/**
 * @brief  Interrompe o funcionamento do motor (parada / desacionamento).
 * @retval None
 * @note usar enableMotorOut_Pin
 */
void BSP_StopMotor(void);

/**
 * @brief  Inicia ou retoma o acionamento do motor conforme as configurações atuais.
 * @retval None
 * @note usar enableMotorOut_Pin
 */
void BSP_StartMotor(void);

/**
 * @brief  Inicializa o periférico de controle PWM e pino de sinal do servomotor.
 * @retval None
 * @note Usar timer4 para o servo
 */
void BSP_InitServo(void);

/**
 * @brief  Inicia ou retoma o funcionamento do servomotor (servo ativo).
 * @retval None
 */
void BSP_StartServo(void);

/**
 * @brief  Interrompe o funcionamento do servomotor (servo inativo).
 * @retval None
 */
void BSP_StopServo(void);

/* ==============================================================================
 * COMUNICAÇÃO COM O HOST (COMANDOS E CONTROLE)
 * ============================================================================== */

/* Comandos Recebidos do Host */
#define CMD_OBJETO_ENCONTRADO   0xA1U /**< Byte recebido do Host: Objeto detectado */
#define CMD_SEM_OBJETO          0xA0U /**< Byte recebido do Host: Sem objeto */

/* Comandos Enviados para o Host */
#define CMD_HOST_STOP_SCAN      0xB0U /**< Byte de comando enviado ao Host: Parar busca de objeto */
#define CMD_HOST_RESUME_SCAN    0xB1U /**< Byte de comando enviado ao Host: Retomar varredura */
#define CMD_HOST_REQUEST_SCAN   CMD_HOST_RESUME_SCAN /**< Byte de comando: Solicitação inicial de varredura (boot) */
#define HOST_ACK_TIMEOUT_MS     50U   /**< Tempo limite de espera para o ACK do Host (50 ms) */
#define HOST_EXPECTED_ACK       0x06U /**< Byte de confirmação (ACK) esperado do Host */

/**
 * @brief  Retorna o estado atual da flag de detecção de objeto recebida do Host.
 * @retval 1 se objeto foi detectado, 0 se sem objeto.
 */
uint8_t BSP_IsObjectDetected(void);

/**
 * @brief  Envia comando solicitando ao Host parar a busca de objetos e aguarda ACK (0x06).
 * @retval HAL_StatusTypeDef HAL_OK se o Host respondeu dentro do timeout de 50 ms.
 */
HAL_StatusTypeDef BSP_HostRequestStopScan(void);

/**
 * @brief  Envia comando solicitando ao Host retomar a varredura e aguarda ACK (0x06).
 * @retval HAL_StatusTypeDef HAL_OK se o Host respondeu dentro do timeout de 50 ms.
 */
HAL_StatusTypeDef BSP_HostRequestResumeScan(void);

/**
 * @brief  Alias para solicitação inicial de varredura no boot (mesmo opcode de ResumeScan).
 */
#define BSP_HostRequestScan     BSP_HostRequestResumeScan

#endif /* INC_BSP_H_ */
