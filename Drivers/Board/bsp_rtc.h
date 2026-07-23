#ifndef __BSP_RTC_H
#define __BSP_RTC_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

/* 时间结构体定义 */
typedef struct {
  uint16_t year;   // 2026
  uint8_t month;   // 1-12
  uint8_t day;     // 1-31
  uint8_t weekday; // 1-7 (1=周一, 7=周日)
  uint8_t hours;   // 0-23
  uint8_t minutes; // 0-59
  uint8_t seconds; // 0-59
} RTC_DateTimeTypeDef;

/* API 声明 */
uint8_t RTC_App_Init(RTC_HandleTypeDef *hrtc);
uint8_t RTC_SetDateTime(RTC_HandleTypeDef *hrtc, RTC_DateTimeTypeDef *dt);
uint8_t RTC_GetDateTime(RTC_HandleTypeDef *hrtc, RTC_DateTimeTypeDef *dt);

#endif /* __BSP_RTC_H */