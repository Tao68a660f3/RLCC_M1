#ifndef __WINDOW_MANAGER_H
#define __WINDOW_MANAGER_H

#include "canvas_renderer.h"
#include "led_driver.h"
#include <stdint.h>

#define MAX_WINDOWS 16

typedef struct {
  // 物理属性 (屏幕坐标系)
  int16_t x, y;  // 窗口在屏幕上的起始位置
  uint16_t w, h; // 窗口的裁剪框大小

  // 内容引用
  CanvasHandle canvas;

  // 视口属性 (内容相对于窗口左上角的偏移)
  int16_t x_offset;
  int16_t y_offset;

  // 动画属性 (基于帧刷新的分频逻辑)
  uint16_t scroll_divider; // 0=静止, 1=每帧移, N=每N帧移一次
  uint16_t tick_counter;   // 内部帧计数器
  int8_t scroll_step;      // 移动方向与步长 (例如 -1)
  uint16_t scroll_counter; // 滚动次数计数器

  uint8_t is_active;
} LED_Window;

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } WinAlign;

extern LED_Window window_list[MAX_WINDOWS];

/* 窗口管理 API */
void WindowManager_Init(void);

// 创建/配置窗口
void Window_Config(uint8_t idx, int16_t x, int16_t y, uint16_t w, uint16_t h);

// 设置对齐方式 (内部自动计算 x_offset)
void Window_SetAlignment(uint8_t idx, WinAlign align);

// 核心渲染与逻辑更新 (在 while 循环调用)
void WindowManager_Single_Process(uint8_t i);
void WindowManager_Process(void);

// 块搬运渲染函数
// h_px==0xFFFF: 静态文本模式; 否则歌词着色模式
// off_x/off_y: 额外偏移叠加在 win->x/y 上（歌词场景传 cfg->offset_x/y，其他传
// 0）
void Window_BlitToScreen(LED_Window *win, uint16_t h_px, uint8_t color_high,
                         uint8_t color_base, int16_t off_x, int16_t off_y);

// 填充文本到窗口画布（Flash 字库适配器版）
// 参数含义同 UI_UpdateTextEx，流程：测量→分配→绘制→原子切换
void Window_FillText(uint8_t win_idx, const char *str, CanvasColor color,
                     CanvasMode mode);

#endif
