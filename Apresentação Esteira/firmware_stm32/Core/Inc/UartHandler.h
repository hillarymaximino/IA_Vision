/**
 * @file    UartHandler.h
 * @author  iona50
 * @date    21 de out. de 2026
 * @brief   Cabeçalho do módulo UartHandler para comunicação UART (STM32F407VET6 - USART1).
 *
 * Define os tipos, estruturas de estado e protótipos de funções necessários
 * para a comunicação serial UART, suportando modos bloqueantes (Polling)
 * e assíncronos (Interrupção), além de controle de fluxo via ACK.
 */

#ifndef INC_UARTHANDLER_H_
#define INC_UARTHANDLER_H_

#include "main.h"

/**
 * @brief Referência externa para a estrutura de controle da USART1.
 *
 * Variável instanciada pelo STM32CubeMX que contém a configuração de
 * hardware do periférico USART1 (PA9=TX, PA10=RX no STM32F407VET6).
 */
extern UART_HandleTypeDef huart1;

/**
 * @brief Tipo de ponteiro de função para o callback de recepção assíncrona.
 *
 * @param size Quantidade de bytes efetivamente recebidos e prontos no buffer.
 */
typedef void (* UART_Handler_Callback_t)(uint16_t size);

/**
 * @brief Enumeração dos estados operacionais da interface UART no UartHandler.
 *
 * Utilizada pela máquina de estados interna para gerenciar o fluxo de
 * dados e evitar colisões entre acessos imperativos e assíncronos ao hardware.
 */
typedef enum {
    UART_HANDLER_STATE_IDLE,                 /**< Nenhuma operação de recepção ativa no momento. */
    UART_HANDLER_STATE_RECEIVING_IT,         /**< Recepção assíncrona (background) por interrupção em andamento. */
    UART_HANDLER_STATE_RECEIVING_POLLING,    /**< Recepção síncrona bloqueante ativa, CPU dedicada à UART. */
    UART_HANDLER_STATE_WAITING_ACK           /**< Aguardando um byte específico de Acknowledge (ACK). */
} UART_Handler_State_t;

/* --- Protótipos das Funções de Recepção --- */
HAL_StatusTypeDef UART_Handler_StartReceive_IT(uint8_t *pData, uint16_t Size, UART_Handler_Callback_t callback);
HAL_StatusTypeDef UART_Handler_Receive(uint8_t *pData, uint16_t Size, uint32_t Timeout);
void UART_Handler_StopReceive_IT(void);

/* --- Protótipos das Funções de Envio --- */
HAL_StatusTypeDef UART_Handler_Send(uint8_t *pData, uint16_t Size, uint32_t Timeout);
HAL_StatusTypeDef UART_Handler_Send_WaitACK(uint8_t *pData, uint16_t Size, uint8_t ExpectedAck, uint32_t Timeout);

/* Alias de compatibilidade para correção de grafia */
#define UART_Handler_StartRecieve_IT UART_Handler_StartReceive_IT

#endif /* INC_UARTHANDLER_H_ */
