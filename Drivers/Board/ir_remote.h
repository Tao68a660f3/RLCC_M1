#ifndef __IR_REMOTE_H
#define __IR_REMOTE_H

#include "stm32f4xx_hal.h"

// 结构体封装，显得很高级
typedef struct {
  uint32_t raw_code;
  uint8_t cmd;
  uint8_t ready;
} IR_Data_t;

extern IR_Data_t My_IR; // 声明全局变量

/* IR 按键码定义 */
#define KEY_1 0x45
#define KEY_2 0x46
#define KEY_3 0x47
#define KEY_4 0x44
#define KEY_5 0x40
#define KEY_6 0x43
#define KEY_7 0x07
#define KEY_8 0x15
#define KEY_9 0x09
#define KEY_STAR 0x16 /* * */
#define KEY_0 0x19
#define KEY_POUND 0x0d /* # */
#define KEY_UP 0x18
#define KEY_DOWN 0x52
#define KEY_LEFT 0x08
#define KEY_OK 0x1c
#define KEY_RIGHT 0x5a

void IR_Init(void);
void IR_Process_Callback(uint16_t GPIO_Pin);

#endif