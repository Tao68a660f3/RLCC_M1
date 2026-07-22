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

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED_R1_Pin GPIO_PIN_0
#define LED_R1_GPIO_Port GPIOA
#define LED_G1_Pin GPIO_PIN_1
#define LED_G1_GPIO_Port GPIOA
#define LED_R2_Pin GPIO_PIN_2
#define LED_R2_GPIO_Port GPIOA
#define LED_G2_Pin GPIO_PIN_3
#define LED_G2_GPIO_Port GPIOA
#define LED_R3_Pin GPIO_PIN_4
#define LED_R3_GPIO_Port GPIOA
#define LED_G3_Pin GPIO_PIN_5
#define LED_G3_GPIO_Port GPIOA
#define LED_R4_Pin GPIO_PIN_6
#define LED_R4_GPIO_Port GPIOA
#define LED_G4_Pin GPIO_PIN_7
#define LED_G4_GPIO_Port GPIOA
#define LED_CLK_Pin GPIO_PIN_0
#define LED_CLK_GPIO_Port GPIOB
#define LED_LAT_Pin GPIO_PIN_1
#define LED_LAT_GPIO_Port GPIOB
#define LED_EN_Pin GPIO_PIN_2
#define LED_EN_GPIO_Port GPIOB
#define SD_CS_Pin GPIO_PIN_12
#define SD_CS_GPIO_Port GPIOB
#define LED_LA_Pin GPIO_PIN_8
#define LED_LA_GPIO_Port GPIOA
#define LED_LB_Pin GPIO_PIN_11
#define LED_LB_GPIO_Port GPIOA
#define LED_LC_Pin GPIO_PIN_12
#define LED_LC_GPIO_Port GPIOA
#define LED_LD_Pin GPIO_PIN_15
#define LED_LD_GPIO_Port GPIOA
#define IR_Pin GPIO_PIN_8
#define IR_GPIO_Port GPIOB
#define IR_EXTI_IRQn EXTI9_5_IRQn
#define W25QXX_CS_Pin GPIO_PIN_9
#define W25QXX_CS_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
