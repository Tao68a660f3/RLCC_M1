#ifndef __LED_DISPLAY_H
#define __LED_DISPLAY_H

#include "led_driver.h"

// 基础绘图
void Display_Clear(void);
void Display_DrawPixel(int16_t x, int16_t y, LED_Color color);

// 文字显示
void Display_DrawChar(int16_t x, int16_t y, char c, LED_Color color);
void Display_ShowString(int16_t x, int16_t y, char *str, LED_Color color);

// 进阶：带窗口限制的滚动文字（为你的 128x32 量身定制）
// void Display_ShowScrollString(int16_t x, int16_t y, char* str, LED_Color
// color);

#endif
