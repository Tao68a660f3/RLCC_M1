#include "ui_manager.h"
#include "canvas_renderer.h"
#include "com_manager.h"
#include "lyric_service.h"
#include "lyric_window_manager.h"
#include "mem_pool.h"
#include "window_manager.h"
#include <string.h>

// ========== 全局状态 ==========
UI_Mode_t g_curr_ui_mode = UI_MODE_2_Line;
Sys_Mode_t g_curr_sys_mode = SYS_MODE_PROTOCOL_MODE;

/**
 * @brief 文本模式消费者回调：将一行文本渲染到双窗口的交替位置
 *
 * 原 _Execute_Stolen_Line 逻辑，用新 API 改写。
 * - 根据当前窗口索引选择颜色（窗口0=红色，窗口1=绿色）
 * - 动态调整滚动属性（超宽则滚动，否则居中）
 * - 渲染后交换两个窗口的 Y 坐标，使新行显示在下部
 */
void UI_Manager_OnLineReceived(const char *text) {
  static uint8_t next_textmode_win = 0;

  if (text == NULL || strlen(text) == 0)
    return;

  uint8_t win_idx = next_textmode_win;

  // 1. 用新 API 填充文本（替代旧 UI_UpdateText）
  Window_FillText(win_idx, text, (win_idx == 0) ? C_RED : C_GREEN,
                  (win_idx == 0) ? CANVAS_R : CANVAS_G, VALIGN_MIDDLE);

  // 2. 动态调整滚动属性
  LED_Window *win = &window_list[win_idx];
  if (win->canvas.width > win->w) {
    win->scroll_divider = 1;
    win->scroll_step = -1;
    Window_SetAlignment(win_idx, ALIGN_LEFT);
  } else {
    win->scroll_divider = 0;
    Window_SetAlignment(win_idx, ALIGN_CENTER);
  }

  // 3. 交换窗口 Y 位置（让新行在下部，旧行在上部）
  int16_t ty = window_list[0].y;
  window_list[0].y = window_list[1].y;
  window_list[1].y = ty;

  // 4. 指向下一个窗口
  next_textmode_win = (win_idx == 0) ? 1 : 0;

  win = &window_list[next_textmode_win];
  if (win->canvas.width > win->w) {
    Window_SetAlignment(next_textmode_win, ALIGN_LEFT);
  }
}

// ========== 以下为占位桩函数，后续可按需实现 ==========

void UI_Manager_OnMusicChanged(void) { LyricWM_Reset(); }

void UI_Manager_Tick(void) {
  switch (g_curr_sys_mode) {
  case SYS_MODE_TXT_MODE:
    COM_Process_TextMode();
    break;
  case SYS_MODE_PROTOCOL_MODE:
    COM_Process_ProtocolMode();
    break;
  default:
    COM_Process_TextMode();
    break;
  }
}

void UI_Layout_Apply(UI_Mode_t mode) {}

void UI_Manager_Mode_1(void) {}

void UI_Manager_Mode_2(void) {}

void UI_Manager_Mode_3(void) {}

void UI_Manager_NextMode(void) {}

void UI_Manager_SysMode1(void) { g_curr_sys_mode = SYS_MODE_TXT_MODE; }

void UI_Manager_SysMode2(void) { g_curr_sys_mode = SYS_MODE_PROTOCOL_MODE; }