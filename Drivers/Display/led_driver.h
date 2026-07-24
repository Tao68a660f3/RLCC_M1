#ifndef __LED_DRIVER_H
#define __LED_DRIVER_H

#include "main.h"

// 屏幕规格 (64 行: 两个 32 行 HUB08 级联)
#define WIDTH 128
#define HEIGHT 64
#define SCAN_ROWS 16
#define DRIVER_WIDTH (WIDTH + 1) // Hardware Abstraction Layer (HAL) Offset

// 显存全局符号 (定义在 led_driver.c，给 window_manager 块搬运直接访问)
extern uint8_t frame_buffer[2][SCAN_ROWS][DRIVER_WIDTH];
extern volatile uint8_t write_idx;

// 颜色枚举
typedef enum {
  COLOR_BLACK = 0,
  COLOR_RED = 1,
  COLOR_GREEN = 2,
  COLOR_YELLOW = 3
} LED_Color;

// 引脚宏定义
#define LED_PORT_DATA                                                          \
  GPIOA // bit0~3: HUB08#1(R1,G1,R2,G2), bit4~7: HUB08#2(R1',G1',R2',G2')

// 函数声明
void LED_Init(TIM_HandleTypeDef *htim);
void LED_SetPixel(int16_t x, int16_t y, LED_Color color);
void LED_Clear(void);
void LED_Commit(void);

#endif
