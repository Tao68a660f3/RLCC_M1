#include "ui_manager.h"
#include "canvas_renderer.h"
#include "com_manager.h"
#include "font_config.h"
#include "lyric_service.h"
#include "lyric_window_manager.h"
#include "mem_pool.h"
#include "window_manager.h"
#include <string.h>

// ========== 全局状态 ==========
UI_Mode_t g_curr_ui_mode = UI_MODE_2_Line;
Sys_Mode_t g_curr_sys_mode = SYS_MODE_PROTOCOL_MODE;

// 每个模式默认的字体配置（渲染时使用，模式内部可临时切换）
typedef struct {
  uint8_t font_asc; // ASCII 字体序号
  uint8_t font_gbk; // GBK 字体序号
} ModeFonts;

static const ModeFonts s_mode_fonts[] = {
    [UI_MODE_2_Line] = {FONT_ASC_1608, FONT_GBK_1616S},
    [UI_MODE_MUSIC_INFO] = {FONT_ASC_1608, FONT_GBK_1616S},
    [UI_MODE_HOME_LIFE] = {FONT_ASC_2412, FONT_GBK_2432S},
    [UI_MODE_1_LINE_MID_FONT] = {FONT_ASC_2010, FONT_GBK_1624M},
    [UI_MODE_1_LINE_BIG_FONT] = {FONT_ASC_2412, FONT_GBK_2432S},
};

// ========== 内部工具函数 ==========

/** 应用当前模式的默认字体 */
static void _ApplyDefaultFont(void) {
  const ModeFonts *mf = &s_mode_fonts[g_curr_ui_mode];
  Font_Select_ASC(mf->font_asc);
  Font_Select_GBK(mf->font_gbk);
}

/** 清理上一个模式的残余状态 */
static void _UI_Cleanup_Current(void) {
  LyricWM_Reset();
  // 如有需要可在此清空窗口内容
}

// ========== 文本行渲染函数（各模式私有实现）==========

/**
 * @brief 双行模式：交替渲染到窗口0/1，交换Y位置使新行在下
 */
static void _Render_2Line(const char *text) {
  static uint8_t next_textmode_win = 0;

  if (text == NULL || strlen(text) == 0)
    return;

  uint8_t win_idx = next_textmode_win;

  // 1. 填充文本
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
}

/**
 * @brief 单行模式：渲染到0号窗口
 */
static void _Render_1Line(const char *text, CanvasColor color) {
  if (text == NULL || strlen(text) == 0)
    return;

  Window_FillText(0, text, color, ColorToCanvasMode(color), VALIGN_MIDDLE);

  LED_Window *win = &window_list[0];
  if (win->canvas.width > win->w) {
    win->scroll_divider = 1;
    win->scroll_step = -1;
    Window_SetAlignment(0, ALIGN_LEFT);
  } else {
    win->scroll_divider = 0;
    Window_SetAlignment(0, ALIGN_CENTER);
  }
}

/**
 * @brief 家居模式：暂用单行大字渲染（后续脏更新方式刷新温湿度/时间）
 */
static void _Render_HomeLife(const char *text) {
  // TODO: 脏更新逻辑，多字体混合显示
  _Render_1Line(text, C_GREEN);
}

// ========== 对外的文本行 API ==========

void UI_Manager_OnLineReceived_Double(const char *text) {
  _ApplyDefaultFont();
  _Render_2Line(text);
}

void UI_Manager_OnLineReceived_Single(const char *text, CanvasColor color) {
  _ApplyDefaultFont();
  _Render_1Line(text, color);
}

/**
 * @brief 通用文本行入口：COM 层拆包后调用，按当前 UI 模式自动分发
 */
void UI_Manager_OnTextLineReceived(const char *text) {
  _ApplyDefaultFont();

  switch (g_curr_ui_mode) {
  case UI_MODE_2_Line:
    _Render_2Line(text);
    break;
  case UI_MODE_1_LINE_MID_FONT:
    _Render_1Line(text, C_RED);
    break;
  case UI_MODE_1_LINE_BIG_FONT:
    _Render_1Line(text, C_RED);
    break;
  default:
    break;
  }
}

// ========== UI 模式切换 ==========

void UI_Manager_SetMode_2Line(void) {
  _UI_Cleanup_Current();
  g_curr_ui_mode = UI_MODE_2_Line;
  _ApplyDefaultFont();

  // 手动配置窗口布局：双行，各占一半高度
  Window_Config(0, 0, 0, 192,
                16); // SYS_MODE_PROTOCOL_MODE 时给 lyric_window 用
  Window_Config(1, 0, 16, 192,
                16); // SYS_MODE_PROTOCOL_MODE 时给 lyric_window 用
}

void UI_Manager_SetMode_1LineMid(void) {
  _UI_Cleanup_Current();
  g_curr_ui_mode = UI_MODE_1_LINE_MID_FONT;
  _ApplyDefaultFont();

  Window_Config(0, 0, 0, 192,
                32); // SYS_MODE_PROTOCOL_MODE 时给 lyric_window 用
}

void UI_Manager_SetMode_1LineBig(void) {
  _UI_Cleanup_Current();
  g_curr_ui_mode = UI_MODE_1_LINE_BIG_FONT;
  _ApplyDefaultFont();

  Window_Config(0, 0, 0, 192,
                32); // SYS_MODE_PROTOCOL_MODE 时给 lyric_window 用
}

void UI_Manager_SetMode_MusicInfo(void) {
  _UI_Cleanup_Current();
  g_curr_ui_mode = UI_MODE_MUSIC_INFO;
  _ApplyDefaultFont();

  Window_Config(0, 0, 0, 48, 16); // mm:ss格式播放进度
  Window_Config(
      1, 48, 0, 144,
      16); // 当title、artit、album均有时 ["title", "artist - album"]轮换显示；
           // 缺少artist或者album时，显示"title - album"或"artist - title"；
  // 只有title时，显示"title"。什么都没有时，不显示。
  // 显示方式：每种内容可显示得下时，居中显示10秒，显示不下时，滚动显示2次。如需切换，则切换内容，如不需切换，则保持当前显示状态。
  Window_Config(2, 0, 16, 192, 16); // 给 lyric_window 用
}

void UI_Manager_SetMode_HomeLife(void) {
  _UI_Cleanup_Current();
  g_curr_ui_mode = UI_MODE_HOME_LIFE;
  _ApplyDefaultFont();

  // TODO: 大字时钟 +
  // 温湿度区域，内容来自env_manager的全局影子数据，只有实际内容变了才更新屏幕上的文字，关于如何调整12/24小时制：如果当前在HomeLife模式，还收到进入HomeLife模式的指令，则toggle12/24小时显示。
  Window_Config(0, 0, 0, 128, 24);  // hh:mm:ss格式显示12/24小时制时间
  Window_Config(1, 0, 24, 128, 8);  // yyyy-mm-dd格式显示当前日期
  Window_Config(2, 128, 0, 64, 8);  // 12小时制时，显示AM/PM，24小时制时留空
  Window_Config(3, 128, 8, 64, 8);  // 显示星期
  Window_Config(4, 128, 16, 64, 8); // 显示温度
  Window_Config(5, 128, 24, 64, 8); // 显示湿度
}

void UI_Manager_NextUIMode(void) {
  UI_Mode_t next = g_curr_ui_mode + 1;
  if (next > UI_MODE_1_LINE_BIG_FONT)
    next = 0;

  switch (next) {
  case UI_MODE_2_Line:
    UI_Manager_SetMode_2Line();
    break;
  case UI_MODE_MUSIC_INFO:
    UI_Manager_SetMode_MusicInfo();
    break;
  case UI_MODE_HOME_LIFE:
    UI_Manager_SetMode_HomeLife();
    break;
  case UI_MODE_1_LINE_MID_FONT:
    UI_Manager_SetMode_1LineMid();
    break;
  case UI_MODE_1_LINE_BIG_FONT:
    UI_Manager_SetMode_1LineBig();
    break;
  }
}

// ========== 系统模式切换 ==========

void UI_Manager_SetSysMode(Sys_Mode_t mode) {
  if (mode == g_curr_sys_mode)
    return;
  g_curr_sys_mode = mode;
  LyricWM_Reset();
}

void UI_Manager_ToggleSysMode(void) {
  Sys_Mode_t next = (g_curr_sys_mode == SYS_MODE_TXT_MODE)
                        ? SYS_MODE_PROTOCOL_MODE
                        : SYS_MODE_TXT_MODE;
  UI_Manager_SetSysMode(next);
}

// ========== Tick：主循环唯一入口 ==========

void UI_Manager_OnMusicChanged(void) { LyricWM_Reset(); }

void UI_Manager_Tick(void) {
  // Step 1: COM 数据消费
  switch (g_curr_sys_mode) {
  case SYS_MODE_TXT_MODE:
    COM_Process_TextMode(); // 内部调 UI_Manager_OnTextLineReceived
    break;
  case SYS_MODE_PROTOCOL_MODE:
    COM_Process_ProtocolMode();
    break;
  default:
    COM_Process_TextMode();
    break;
  }

  // Step 2: 窗口动画（滚动/刷新 LED）
  WindowManager_Process();
}