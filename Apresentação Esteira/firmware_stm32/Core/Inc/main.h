/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define MotorStepPin_Pin GPIO_PIN_6
#define MotorStepPin_GPIO_Port GPIOA
#define enableMotorOut_Pin GPIO_PIN_8
#define enableMotorOut_GPIO_Port GPIOD
#define directionMotorOut_Pin GPIO_PIN_9
#define directionMotorOut_GPIO_Port GPIOD
#define ServoPwmPin_Pin GPIO_PIN_12
#define ServoPwmPin_GPIO_Port GPIOD
#define enableMotorIn_Pin GPIO_PIN_0
#define enableMotorIn_GPIO_Port GPIOD
#define speed1_Pin GPIO_PIN_1
#define speed1_GPIO_Port GPIOD
#define speed3_Pin GPIO_PIN_3
#define speed3_GPIO_Port GPIOD
#define speed4_Pin GPIO_PIN_4
#define speed4_GPIO_Port GPIOD
#define directionIn_Pin GPIO_PIN_5
#define directionIn_GPIO_Port GPIOD
#define servoControl_Pin GPIO_PIN_6
#define servoControl_GPIO_Port GPIOD
#define foundObject_Pin GPIO_PIN_7
#define foundObject_GPIO_Port GPIOD
#define speed2_Pin GPIO_PIN_5
#define speed2_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
