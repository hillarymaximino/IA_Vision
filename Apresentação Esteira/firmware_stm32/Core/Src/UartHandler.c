/**
 * @file    UartHandler.c
 * @author  iona50
 * @date    21 de out. de 2026
 * @brief   Implementação do módulo UartHandler para comunicação UART (STM32F407VET6 - USART1).
 *
 * Contém a lógica de envio, recepção e a máquina de estados que processa
 * os dados recebidos via interrupção de hardware (USART1), adaptando-se
 * dinamicamente ao contexto de execução da aplicação.
 */

#include "UartHandler.h"

/* --- Variáveis Estáticas da Máquina de Estados --- */

/** @brief Ponteiro para a função de callback registrada pela aplicação. */
static UART_Handler_Callback_t p_user_rx_callback = NULL;

/** @brief Estado atual da interface UART gerenciada pelo UartHandler. */
static volatile UART_Handler_State_t uart_state = UART_HANDLER_STATE_IDLE;

/** @brief Buffer circular interno de 1 byte para gerenciar a recepção da HAL via IT. */
static uint8_t rx_byte_buffer;

/** @brief Ponteiro para o buffer de destino fornecido pela aplicação em modo IT. */
static uint8_t *p_user_buffer = NULL;

/** @brief Quantidade total de bytes que a aplicação espera receber no pacote atual. */
static uint16_t user_expected_size = 0;

/** @brief Índice atual de preenchimento do buffer da aplicação (modo IT). */
static uint16_t user_current_index = 0;

/** @brief Flag de sinalização assíncrona indicando a chegada do byte de ACK. */
static volatile uint8_t ack_received_flag = 0;

/** @brief Valor do byte de Acknowledge (ACK) que o sistema está aguardando. */
static uint8_t expected_ack_val = 0;

/* ==============================================================================
 * FUNÇÕES DE CONTROLE DE RECEPÇÃO
 * ============================================================================== */

/**
 * @brief  Para a recepção assíncrona (Interrupção) de forma forçada.
 *
 * Função pública que desativa a interrupção no NVIC, aborta operações assíncronas
 * da HAL na USART1 e reinicia as variáveis da máquina de estados para IDLE.
 * Utilizada para garantir transições seguras entre contextos de execução.
 */
void UART_Handler_StopReceive_IT(void)
{
    if (uart_state != UART_HANDLER_STATE_IDLE)
    {
        HAL_NVIC_DisableIRQ(USART1_IRQn);
        HAL_UART_AbortReceive(&huart1);

        uart_state = UART_HANDLER_STATE_IDLE;
        p_user_rx_callback = NULL;
        user_current_index = 0;
        user_expected_size = 0;
        p_user_buffer = NULL;
    }
}

/**
 * @brief  Inicia a recepção contínua (1 byte por vez) via interrupção (Background).
 *
 * Limpa configurações anteriores, configura o buffer de destino e aciona a
 * escuta da USART1 no hardware. O fluxo de dados será gerenciado pela
 * máquina de estados interna dentro do callback do IRQ.
 *
 * @param  pData Buffer onde o pacote de dados será montado.
 * @param  Size Quantidade total de bytes do pacote esperado.
 * @param  callback Função a ser chamada quando o pacote estiver completo.
 * @retval HAL_StatusTypeDef Status de retorno da chamada HAL subjacente.
 */
HAL_StatusTypeDef UART_Handler_StartReceive_IT(uint8_t *pData, uint16_t Size, UART_Handler_Callback_t callback)
{
    UART_Handler_StopReceive_IT(); /* Previne instabilidade desativando configurações antigas */

    p_user_rx_callback = callback;
    p_user_buffer = pData;
    user_expected_size = Size;
    user_current_index = 0;
    uart_state = UART_HANDLER_STATE_RECEIVING_IT;

    HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    return HAL_UART_Receive_IT(&huart1, &rx_byte_buffer, 1);
}

/**
 * @brief  Recepção de dados padrão bloqueante (Polling).
 *
 * Aguarda a chegada dos dados prendendo a execução da CPU na função até que
 * todos os bytes sejam recebidos ou o tempo limite expire. Altera dinamicamente
 * o estado do driver para refletir a ocupação do hardware.
 *
 * @param  pData Ponteiro para o buffer de destino.
 * @param  Size Quantidade de bytes a receber.
 * @param  Timeout Tempo máximo de espera em milissegundos.
 * @retval HAL_StatusTypeDef Status de retorno da operação (HAL_OK, HAL_TIMEOUT, etc).
 */
HAL_StatusTypeDef UART_Handler_Receive(uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    HAL_StatusTypeDef status;

    UART_Handler_StopReceive_IT(); /* Garante que não haja concorrência de hardware */

    uart_state = UART_HANDLER_STATE_RECEIVING_POLLING;
    status = HAL_UART_Receive(&huart1, pData, Size, Timeout);
    uart_state = UART_HANDLER_STATE_IDLE; /* Libera o estado ao finalizar */

    return status;
}

/* ==============================================================================
 * FUNÇÕES DE ENVIO DE DADOS
 * ============================================================================== */

/**
 * @brief  Envia um pacote de dados via USART1 (Modo Bloqueante Simples).
 *
 * Transmite os dados sequencialmente, sem controle de fluxo, mantendo
 * a CPU retida até a conclusão do envio ou estouro do timeout.
 *
 * @param  pData Ponteiro para o buffer contendo os dados a enviar.
 * @param  Size Quantidade de bytes do pacote.
 * @param  Timeout Tempo máximo de espera para o término da transmissão (em ms).
 * @retval HAL_StatusTypeDef Status de retorno da transmissão.
 */
HAL_StatusTypeDef UART_Handler_Send(uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    return HAL_UART_Transmit(&huart1, pData, Size, Timeout);
}

/**
 * @brief  Envia dados e aguarda o ACK, adaptando-se ao estado atual da UART.
 *
 * Se o sistema estiver operando com recepção por interrupção (IT ativa),
 * o pacote é enviado e a função captura o ACK via máquina de estados, sem
 * perder dados assíncronos. Se o sistema estiver ocioso (IDLE), utiliza a
 * recepção bloqueante padrão da HAL para a resposta.
 *
 * @param  pData Ponteiro para o buffer contendo os dados a enviar.
 * @param  Size Quantidade de bytes do pacote.
 * @param  ExpectedAck Valor numérico do byte esperado como confirmação (ex: 0x06).
 * @param  Timeout Tempo máximo global (em ms) para envio e recebimento da resposta.
 * @retval HAL_StatusTypeDef HAL_OK se o ACK for validado, erro ou timeout caso contrário.
 */
HAL_StatusTypeDef UART_Handler_Send_WaitACK(uint8_t *pData, uint16_t Size, uint8_t ExpectedAck, uint32_t Timeout)
{
    HAL_StatusTypeDef status;
    uint32_t start_tick = HAL_GetTick();

    if (uart_state == UART_HANDLER_STATE_RECEIVING_IT)
    {
        /* --- MODO ASSÍNCRONO: IT ATIVO --- */
        expected_ack_val = ExpectedAck;
        ack_received_flag = 0;

        UART_Handler_State_t previous_state = uart_state;

        /* Muda o estado antes da transmissão para evitar condição de corrida
           caso o periférico remoto responda instantaneamente. */
        uart_state = UART_HANDLER_STATE_WAITING_ACK;

        status = HAL_UART_Transmit(&huart1, pData, Size, Timeout);
        if (status != HAL_OK)
        {
            uart_state = previous_state;
            return status;
        }

        /* Aguarda o preenchimento da flag pela interrupção (em background) */
        while (!ack_received_flag)
        {
            if ((HAL_GetTick() - start_tick) > Timeout)
            {
                uart_state = previous_state;
                return HAL_TIMEOUT;
            }
        }

        uart_state = previous_state;
        return HAL_OK;
    }
    else
    {
        /* --- MODO BLOQUEANTE: IDLE / OUTROS --- */
        status = HAL_UART_Transmit(&huart1, pData, Size, Timeout);
        if (status != HAL_OK)
        {
            return status;
        }

        /* Calcula o tempo decorrido no TX para não estourar o limite global estipulado */
        uint32_t elapsed = HAL_GetTick() - start_tick;
        if (elapsed >= Timeout) return HAL_TIMEOUT;

        uint8_t rx_byte = 0;
        status = HAL_UART_Receive(&huart1, &rx_byte, 1, (Timeout - elapsed));
        if (status != HAL_OK)
        {
            return status;
        }

        if (rx_byte == ExpectedAck)
        {
            return HAL_OK;
        }

        return HAL_ERROR; /* Dado recebido divergiu do ACK esperado */
    }
}

/* ==============================================================================
 * TRATADORES DE INTERRUPÇÃO E CALLBACKS DA HAL
 * ============================================================================== */

/**
 * @brief  Tratador de Interrupção de Hardware (IRQ Handler) da USART1.
 *
 * Função acionada pelo NVIC do Cortex-M4 sempre que ocorre um evento na USART1.
 * Direciona o processamento para a máquina genérica de flags da HAL.
 */
void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart1);
}

/**
 * @brief  Callback de Recepção Concluída da HAL (Núcleo da Máquina de Estados).
 *
 * Invocada pela HAL_UART_IRQHandler após cada byte recebido (Size = 1).
 * Processa o byte de acordo com o estado atual: valida ACKs para liberar funções
 * de envio bloqueadas ou acumula o payload para disparar o callback da aplicação.
 *
 * @param  huart Ponteiro para a estrutura do periférico que gerou o evento.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        switch (uart_state)
        {
            case UART_HANDLER_STATE_WAITING_ACK:
                /* Valida se o byte recém-chegado corresponde ao esperado */
                if (rx_byte_buffer == expected_ack_val)
                {
                    ack_received_flag = 1;
                }
                break;

            case UART_HANDLER_STATE_RECEIVING_IT:
                /* Acumula o dado no buffer mapeado da aplicação */
                if (p_user_buffer != NULL && user_current_index < user_expected_size)
                {
                    p_user_buffer[user_current_index] = rx_byte_buffer;
                    user_current_index++;

                    /* Se atingiu a cota solicitada, aciona o callback de nível superior */
                    if (user_current_index >= user_expected_size)
                    {
                        if (p_user_rx_callback != NULL)
                        {
                            p_user_rx_callback(user_current_index);
                        }
                        user_current_index = 0; /* Prepara para o próximo pacote contínuo */
                    }
                }
                break;

            case UART_HANDLER_STATE_IDLE:
            case UART_HANDLER_STATE_RECEIVING_POLLING:
            default:
                break;
        }

        /* Rearma a recepção assíncrona para capturar o próximo byte */
        HAL_UART_Receive_IT(&huart1, &rx_byte_buffer, 1);
    }
}
