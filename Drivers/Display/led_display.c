#include "led_display.h"
#include "inner_font.h" // 确保你的 Chars[] 数组在这里能找到

// 1. 简单的画点封装
void Display_DrawPixel(int16_t x, int16_t y, LED_Color color) {
  // 这里的函数其实就是调用驱动层的 SetPixel
  // 可以在这里加一层“旋转”或者“镜像”逻辑，如果需要的话
  LED_SetPixel(x, y, color);
}

// 2. 解析你的 5x7/5x8 字库
void Display_DrawChar(int16_t x, int16_t y, char c, LED_Color color) {
  if (c < char_offset || c > char_offset + char_numsum)
    c = ' ';

  uint16_t font_ptr = (c - char_offset) * 5; // 你的字库每个字符 5 字节

  for (uint8_t col = 0; col < 5; col++) { // 遍历 5 列
    uint8_t columnData = Chars[font_ptr + col];

    for (uint8_t row = 0; row < 8; row++) { // 遍历 8 行
      if (columnData & (0x80 >> row)) {
        Display_DrawPixel(x + col, y + row, color);
      }
    }
  }
}

// 3. 静态字符串显示
void Display_ShowString(int16_t x, int16_t y, char *str, LED_Color color) {
  while (*str) {
    // 如果字符超出了右边界，直接停掉，节省 CPU
    if (x > WIDTH)
      break;

    // 只有在屏幕范围内的字符才画
    if (x + 5 > 0) {
      Display_DrawChar(x, y, *str, color);
    }

    x += 6; // 5像素宽 + 1像素间距
    str++;
  }
}
