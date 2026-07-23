#ifndef __IR_REMOTE_H
#define __IR_REMOTE_H

#include "stm32f4xx_hal.h"

// 结构体封装，显得很高级
typedef struct {
    uint32_t raw_code;
    uint8_t  cmd;
    uint8_t  ready;
} IR_Data_t;

extern IR_Data_t My_IR; // 声明全局变量

void IR_Init(void);
void IR_Process_Callback(uint16_t GPIO_Pin);

#endif