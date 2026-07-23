#ifndef __SYSTEM_UTILS_H
#define __SYSTEM_UTILS_H

#include "stm32f4xx_hal.h"

void DWT_Init(void);
void delay_us(uint32_t us);

#endif