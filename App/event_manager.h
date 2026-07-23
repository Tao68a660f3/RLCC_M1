#ifndef __EVENT_MANAGER_H
#define __EVENT_MANAGER_H

#include "stm32f4xx_hal.h"

// void Event_Init(void);
void Event_Dispatch_IR(uint8_t cmd); // 分发红外按键信号

#endif