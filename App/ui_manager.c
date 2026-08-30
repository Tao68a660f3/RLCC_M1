#include "ui_manager.h"
#include "canvas_renderer.h"
#include "com_manager.h"
#include "env_manager.h"
#include "font_config.h"
#include "led_driver.h"
#include "lyric_service.h"
#include "lyric_window_manager.h"
#include "mem_pool.h"
#include "window_manager.h"
#include <stdio.h>
#include <string.h>

// ========== 全局状态 ==========
UI_Mode_t g_curr_ui_mode = UI_MODE_2_Line;
Sys_Mode_t g_curr_sys_mode = SYS_MODE_PROTOCOL_MODE;

/** 息屏标志：1=熄灭，0=亮屏 */
static uint8_t s_screen_off = 0;

// ====== 协议内文本模式（0x16 驱动，保持在 SYS_MODE_PROTOCOL_MODE 内）======
/** 1 = 处于协议内文本模式 */
static uint8_t s_proto_text_active = 0;
/** 双行文本渲染交替起点（原 _Render_2Line 内 static 提升为文件级以便重置） */
static uint8_t s_next_textmode_win = 0;
/** 1 = 正在 EnterProtocolTextMode 转场（其内部 SetMode 触发的
 *  _UI_Cleanup_Current 应跳过清标志，防止进入子模式被自杀式中断） */
static uint8_t s_entering_proto_text = 0;

// 每个模式默认的字体配置（渲染时使用，模式内部可临时切换）
typedef struct {
  uint8_t font_asc; // ASCII 字体序号
  uint8_t font_gbk; // GBK 字体序号
} ModeFonts;

static const ModeFonts s_mode_fonts[] = {
    [UI_MODE_2_Line] = {FONT_ASC_1608, FONT_GBK_1616S},
    [UI_MODE_MUSIC_INFO] = {FONT_ASC_1608, FONT_GBK_1616S},
    [UI_MODE_HOME_LIFE] = {FONT_ASC_2412, FONT_GBK_2432S},
    [UI_MODE_1_LINE_MID_FONT] = {FONT_ASC_2010, FONT_GBK_1624Y},
    [UI_MODE_1_LINE_BIG_FONT] = {FONT_ASC_2412, FONT_GBK_2432S},
};

// ========== 内部工具函数 ==========

/** 应用当前模式的默认字体 */
static void _ApplyDefaultFont(void) {
  const ModeFonts *mf = &s_mode_fonts[g_curr_ui_mode];
  Font_Select_ASC(mf->font_asc);
  Font_Select_GBK(mf->font_gbk);
}

/** 清理上一个模式的残余状态（任意模式切换的先决步骤） */
static void _UI_Cleanup_Current(void) {
  // 模式切换 = 退出协议内文本子模式（与 SetSysMode 约定一致）。
  // 唯一例外是 EnterProtocolTextMode 转场：其内部 SetMode 触发的
  // cleanup 由 s_entering_proto_text 保护，不得清掉刚置位的标志，
  // 否则进入子模式会被自杀式中断（子模式布局也会失效）。
  if (!s_entering_proto_text)
    s_proto_text_active = 0;

  LyricWM_Reset();
  WindowManager_Init();
  LED_Clear(); // 清除整个 frame_buffer，防止旧模式区域残留
}

/**
 * @brief 通用滚动配置：内容超出窗口宽度时左滚动，否则居中
 */
static void _SetScrollIfNeeded(uint8_t win_idx) {
  LED_Window *win = &window_list[win_idx];
  if (win->canvas.width > win->w) {
    win->scroll_divider = 2;
    win->scroll_step = -1;
    Window_SetAlignment(win_idx, ALIGN_LEFT);
  } else {
    win->scroll_divider = 0;
    Window_SetAlignment(win_idx, ALIGN_CENTER);
  }
}

// ========== 文本行渲染函数（各模式私有实现）==========

/**
 * @brief 双行模式：交替渲染到窗口0/1，交换Y位置使新行在下
 */
static void _Render_2Line(const char *text) {
  if (text == NULL || strlen(text) == 0)
    return;

  uint8_t win_idx = s_next_textmode_win;

  Window_FillText(win_idx, text, (win_idx == 0) ? C_RED : C_GREEN,
                  (win_idx == 0) ? CANVAS_R : CANVAS_G, VALIGN_MIDDLE);
  _SetScrollIfNeeded(win_idx);

  // 交换窗口 Y 位置（让新行在下部，旧行在上部）
  int16_t ty = window_list[0].y;
  window_list[0].y = window_list[1].y;
  window_list[1].y = ty;

  window_list[0].x_offset = 0;
  window_list[1].x_offset = 0;

  s_next_textmode_win = (win_idx == 0) ? 1 : 0;
}

/**
 * @brief 单行模式：渲染到0号窗口
 */
static void _Render_1Line(const char *text, CanvasColor color) {
  if (text == NULL || strlen(text) == 0)
    return;

  Window_FillText(0, text, color, ColorToCanvasMode(color), VALIGN_MIDDLE);
  _SetScrollIfNeeded(0);
}

// ========== HomeLife 脏更新渲染（6窗口，多字体）==========

static char s_cache_time[9] = "";
static char s_cache_time_s[7] = "";
static char s_cache_date[11] = "";
static char s_cache_ampm[3] = "";
static char s_cache_week[4] = "";
static char s_cache_temp[7] = "";
static char s_cache_humi[5] = "";

/** 舒适度等级 → CanvasColor */
#define COMFORT_COLOR(lvl)                                                     \
  ((CanvasColor)((lvl) == COMFORT_GREEN    ? C_GREEN                           \
                 : (lvl) == COMFORT_YELLOW ? C_YELLOW                          \
                                           : C_RED))

/** 脏检测 + 更新的宏：当缓存与当前值不同时，填充窗口并更新缓存 */
#define HOME_LIFE_UPDATE(win, cache, str, asc_font, color)                     \
  do {                                                                         \
    if (strcmp(cache, str)) {                                                  \
      Font_Select_ASC(asc_font);                                               \
      Window_FillText(win, str, color, ColorToCanvasMode(color),               \
                      VALIGN_MIDDLE);                                          \
      Window_SetAlignment(win, ALIGN_CENTER);                                  \
      _SetScrollIfNeeded(win);                                                 \
      strcpy(cache, str);                                                      \
    }                                                                          \
  } while (0)

/** 脏检测 + 清窗版本（用于 AM/PM 可能为空的情况） */
#define HOME_LIFE_UPDATE_AMPM(cache, str, asc_font, color)                     \
  do {                                                                         \
    if (strcmp(cache, str)) {                                                  \
      Font_Select_ASC(asc_font);                                               \
      Window_FillText(2, (str[0] == '\0') ? " " : str, color,                  \
                      ColorToCanvasMode(color), VALIGN_MIDDLE);                \
      Window_SetAlignment(2, ALIGN_CENTER);                                    \
      strcpy(cache, str);                                                      \
    }                                                                          \
  } while (0)

/** 1LineMid 脏更新 Tick：win1 = hh:mm，win2 = 温度，结束后恢复模式默认字体 */
static void _Render_1LineMid_Tick(void) {
  HOME_LIFE_UPDATE(1, s_cache_time_s, g_str_time_s, FONT_ASC_0805, C_YELLOW);
  HOME_LIFE_UPDATE(2, s_cache_temp, g_str_temp, FONT_ASC_0805,
                   COMFORT_COLOR(g_temp_comfort));
  _ApplyDefaultFont();
}

static void _Render_HomeLife_Tick(void) {
  HOME_LIFE_UPDATE(0, s_cache_time, g_str_time, FONT_ASCV_20, C_YELLOW);
  HOME_LIFE_UPDATE(1, s_cache_date, g_str_date_std, FONT_ASC_0805, C_GREEN);
  HOME_LIFE_UPDATE_AMPM(s_cache_ampm, g_str_ampm, FONT_ASC_0805, C_YELLOW);
  HOME_LIFE_UPDATE(3, s_cache_week, g_str_week_en, FONT_ASC_0805, C_GREEN);
  HOME_LIFE_UPDATE(4, s_cache_temp, g_str_temp, FONT_ASC_0805,
                   COMFORT_COLOR(g_temp_comfort));
  HOME_LIFE_UPDATE(5, s_cache_humi, g_str_humi, FONT_ASC_0805,
                   COMFORT_COLOR(g_humi_comfort));
}

#undef HOME_LIFE_UPDATE
#undef HOME_LIFE_UPDATE_AMPM

/** 置空所有 HomeLife 脏更新缓存，强制下次全刷新 */
static void _Reset_HomeLife_Cache(void) {
  s_cache_time[0] = '\0';
  s_cache_time_s[0] = '\0';
  s_cache_date[0] = '\0';
  s_cache_ampm[0] = '\0';
  s_cache_week[0] = '\0';
  s_cache_temp[0] = '\0';
  s_cache_humi[0] = '\0';
}

// ========== MusicInfo 脏更新渲染 ==========

/** 纯算术组装 mm:ss，避开 snprintf */
static void _Fast_Time2Str(uint32_t total_sec, char *out) {
  uint32_t m = total_sec / 60;
  uint32_t s = total_sec % 60;
  out[0] = (m / 10) + '0';
  out[1] = (m % 10) + '0';
  out[2] = ':';
  out[3] = (s / 10) + '0';
  out[4] = (s % 10) + '0';
  out[5] = '\0';
}

/** 上次渲染的秒数（数字比较，避免 strcmp） */
static uint32_t s_last_progress_sec = 0;

static void _Render_Progress(void) {
  uint32_t ms = Get_Current_PlayTime();
  uint32_t sec = ms / 1000;
  if (sec == s_last_progress_sec)
    return;
  s_last_progress_sec = sec;

  char buf[6];
  _Fast_Time2Str(sec, buf);
  Font_Select_ASC(FONT_ASC_1608);
  Window_FillText(1, buf, C_YELLOW, CANVAS_Y, VALIGN_MIDDLE);
  Window_SetAlignment(1, ALIGN_CENTER);
}

// ========== 元数据轮换状态机 ==========

typedef struct {
  const char *text;  // 指向源字符串（零拷贝）
  CanvasColor color; // 条目颜色
} MetaItem;

static struct {
  MetaItem items[4];                      // 条目指针数组
  char sub_buf[2 * MAX_METADATA_STR_LEN]; // 组合串 "artist - album" 的本地存储
  uint8_t pool_count;                     // 有效条目数
  uint8_t curr_idx;                       // 当前显示第几条
  uint32_t switch_start_tick;             // 切到此条目时 HAL_GetTick()
} s_meta;

/** 将元数据第 idx 条渲染到窗口 2（渲染后根据实际宽度决定滚动策略） */
static void _Meta_RenderToWin(uint8_t idx) {
  CanvasColor color = s_meta.items[idx].color;
  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616S);
  Window_FillText(2, s_meta.items[idx].text, color, ColorToCanvasMode(color),
                  VALIGN_MIDDLE);

  LED_Window *win = &window_list[2];
  uint8_t needs_scroll = (win->canvas.width > win->w);
  if (needs_scroll) {
    win->x_offset = win->w;
  } else {
    Window_SetAlignment(2, ALIGN_CENTER);
  }
  win->scroll_divider = needs_scroll ? 2 : 0;
  win->scroll_step = needs_scroll ? -1 : 0;
  win->scroll_counter = 0;
}

/** 当媒体元数据变化时重建 pool */
static void _Meta_RebuildPool(void) {
  s_meta.pool_count = 0;
  s_meta.curr_idx = 0;
  s_meta.switch_start_tick = HAL_GetTick();

  // item[0] = title — 指针直接指向源字符串，零拷贝，红色
  const char *title = (strlen(g_sys.title) > 0) ? g_sys.title : "Unknown music";
  s_meta.items[0].text = title;
  s_meta.items[0].color = C_RED;
  s_meta.pool_count = 1;

  // 构建组合串到 sub_buf
  s_meta.sub_buf[0] = '\0';
  uint8_t has_artist = (strlen(g_sys.artist) > 0);
  uint8_t has_album = (strlen(g_sys.album) > 0);

  if (has_artist && has_album) {
    snprintf(s_meta.sub_buf, sizeof(s_meta.sub_buf), "%s - %s", g_sys.artist,
             g_sys.album);
  } else if (has_artist) {
    snprintf(s_meta.sub_buf, sizeof(s_meta.sub_buf), "%s - %s", g_sys.artist,
             title);
  } else if (has_album) {
    snprintf(s_meta.sub_buf, sizeof(s_meta.sub_buf), "%s - %s", title,
             g_sys.album);
  }
  // 都没有 → sub_buf 为空，不添加副信息条目

  if (strlen(s_meta.sub_buf) > 0) {
    // 副信息颜色：同时有 artist+album 绿色，否则黄色
    CanvasColor sub_color = (has_artist && has_album) ? C_GREEN : C_YELLOW;
    s_meta.items[s_meta.pool_count].text = s_meta.sub_buf;
    s_meta.items[s_meta.pool_count].color = sub_color;
    s_meta.pool_count++;
  }
}

/** 元数据轮换 Tick：根据当前策略滚动/切换 */
static void _Render_MetaRotation(void) {
  if (s_meta.pool_count == 0)
    return;

  const char *curr_text = s_meta.items[s_meta.curr_idx].text;
  if (strlen(curr_text) == 0) {
    static char s_meta_cleared = 0;
    if (!s_meta_cleared) {
      Window_FillText(2, " ", C_GREEN, CANVAS_G, VALIGN_MIDDLE);
      s_meta_cleared = 1;
    }
    return;
  }

  LED_Window *win = &window_list[2];
  if (win->canvas.handle == -1)
    return;

  uint32_t now = HAL_GetTick();
  uint32_t elapsed = now - s_meta.switch_start_tick;
  uint8_t do_switch = 0;

  // 根据 _Meta_RenderToWin 设定的 scroll_divider 判断滚动策略
  if (win->scroll_divider == 0) {
    do_switch = (elapsed >= 10000); // 居中不滚动 → 10 秒超时切换
  } else {
    do_switch = (win->scroll_counter >= 3); // 滚动 → 3 轮后切换
  }

  if (do_switch) {
    s_meta.curr_idx = (s_meta.curr_idx + 1) % s_meta.pool_count;
    s_meta.switch_start_tick = HAL_GetTick();
    _Meta_RenderToWin(s_meta.curr_idx);
  }
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
  s_next_textmode_win = 0; // 重置双行文本交替起点

  // 双行窗口的 y 坐标根据系统模式/协议内文本子模式分配：
  //   - protocol_mode（歌词）: LyricWM 接管 y 偏移计算
  //     （_RecalcOffsetYByTimeOrder），所有歌词窗口 y 基值必须相同
  //   - text_mode / 协议内文本子模式: _Render_2Line 手动交换 y 坐标
  //     来实现新行在下，窗口 y 基值不相同
  uint8_t use_text_layout =
      (g_curr_sys_mode == SYS_MODE_TXT_MODE || s_proto_text_active);
  if (use_text_layout) {
    Window_Config(0, 0, 0, 192, 16);
    Window_Config(1, 0, 16, 192, 16);
    LyricWM_Init(0); // 停用歌词（子模式下本就不渲染，纯防御）
  } else {
    Window_Config(0, 0, 0, 192, 16);
    Window_Config(1, 0, 0, 192, 16);
    LyricWM_Init(2);
  }
}

void UI_Manager_SetMode_1LineMid(void) {
  _UI_Cleanup_Current();
  g_curr_ui_mode = UI_MODE_1_LINE_MID_FONT;
  _ApplyDefaultFont();
  Window_Config(0, 0, 0, 160, 32); // 歌词窗口
  Window_Config(1, 160, 0, 32, 8); // s_cache_time_s: hh:mm
  Window_Config(2, 160, 8, 32, 8); // 温度
  _Reset_HomeLife_Cache();         // 清空缓存，强制 Tick 刷新
  LyricWM_Init(1);
}

void UI_Manager_SetMode_1LineBig(void) {
  _UI_Cleanup_Current();
  g_curr_ui_mode = UI_MODE_1_LINE_BIG_FONT;
  _ApplyDefaultFont();
  Window_Config(0, 0, 0, 192, 32);
  LyricWM_Init(1);
}

void UI_Manager_SetMode_MusicInfo(void) {
  _UI_Cleanup_Current();

  g_curr_ui_mode = UI_MODE_MUSIC_INFO;
  _ApplyDefaultFont();

  // 窗口顺序：win0=歌词，win1=mm:ss进度，win2=元数据轮换
  Window_Config(0, 0, 16, 192, 16); // [0] 歌词（LyricWM 用）
  Window_Config(1, 0, 0, 48, 16);   // [1] mm:ss 进度
  Window_Config(2, 48, 0, 144, 16); // [2] title/artist/album 轮换

  // 初始化缓存
  s_last_progress_sec = 0;

  // 重建并预渲染元数据第 0 条（子模式下也照常，方便退出后继续使用）
  _Meta_RebuildPool();
  _Meta_RenderToWin(0);

  // 仅按标志决定歌词行渲染是否启用：常规 LyricWM_Init(1)，子模式停用
  LyricWM_Init(s_proto_text_active ? 0 : 1);
}

void UI_Manager_SetMode_HomeLife(void) {
  // 本函数不总是经过 _UI_Cleanup_Current（同在 HomeLife 时走
  // 12/24H toggle 快捷分支直接返回），因此这里补统一退出子模式。
  if (!s_entering_proto_text)
    s_proto_text_active = 0;

  // 12/24H toggle：如果当前已在 HomeLife 模式，切换格式后直接返回
  if (g_curr_ui_mode == UI_MODE_HOME_LIFE) {
    Env_Manager_SetFormat((g_time_format_config == TIME_FORMAT_24H)
                              ? TIME_FORMAT_12H
                              : TIME_FORMAT_24H);
    _Reset_HomeLife_Cache(); // 强制下次 Tick 刷新所有窗口
    return;
  }

  _UI_Cleanup_Current();

  g_curr_ui_mode = UI_MODE_HOME_LIFE;
  _ApplyDefaultFont();

  Window_Config(0, 0, 0, 152, 24);  // hh:mm:ss 大字时间
  Window_Config(1, 0, 24, 152, 8);  // yyyy-mm-dd 日期
  Window_Config(2, 152, 0, 40, 8);  // AM/PM
  Window_Config(3, 152, 8, 40, 8);  // 星期
  Window_Config(4, 152, 16, 40, 8); // 温度
  Window_Config(5, 152, 24, 40, 8); // 湿度

  _Reset_HomeLife_Cache();
  LyricWM_Init(0); // used_lyric_lines=0，LyricWM_RenderMgr 跳过歌词
}

// ========== 协议内文本模式（0x16 驱动）==========

uint8_t UI_Manager_IsProtocolTextMode(void) { return s_proto_text_active; }

void UI_Manager_EnterProtocolTextMode(void) {
  if (s_proto_text_active)
    return; // 幂等：已处于子模式则仅刷新文本
  s_proto_text_active = 1;

  // 转场保护：以下 SetMode 内部的 _UI_Cleanup_Current 若清掉
  // s_proto_text_active，会中断"进入子模式"本身（标志被清零），
  // 故转场期间跳过清理，结束后恢复。
  s_entering_proto_text = 1;
  switch (g_curr_ui_mode) {
  case UI_MODE_2_Line:
    UI_Manager_SetMode_2Line(); // 标志已置 → 文本 y0/16 布局 + LyricWM_Init(0)
    break;
  case UI_MODE_MUSIC_INFO:
    UI_Manager_SetMode_MusicInfo(); // 正常初始化，仅末尾 LyricWM_Init(0)
    break;
  case UI_MODE_1_LINE_MID_FONT:
  case UI_MODE_1_LINE_BIG_FONT:
  case UI_MODE_HOME_LIFE:
    // 布局与 sys_mode 无关，子模式下 LyricWM_RenderMgr 不执行 → 无需重置
    break;
  default:
    UI_Manager_SetMode_2Line();
    break;
  }
  s_entering_proto_text = 0;
}

void UI_Manager_ExitProtocolTextMode(void) {
  if (!s_proto_text_active)
    return;
  s_proto_text_active = 0;
  // 子模式生命周期内 UI 模式不变（任何模式切换都会先经 _UI_Cleanup_Current
  // 清除子模式标志即退出子模式），故无需保存进入前模式，直接用当前模式恢复。
  switch (g_curr_ui_mode) {
  case UI_MODE_2_Line:
    UI_Manager_SetMode_2Line(); // 标志已清 → 协议 y0/0 + LyricWM_Init(2)
    break;
  case UI_MODE_MUSIC_INFO:
    UI_Manager_SetMode_MusicInfo(); // 标志已清 → LyricWM_Init(1)，歌词恢复
    break;
  case UI_MODE_1_LINE_MID_FONT:
    // 子模式下 win0 被 _Render_1Line 画过文本，画布需重绘；
    // SetMode_1LineMid 内部 _Reset_HomeLife_Cache 强制刷新 win1/win2
    UI_Manager_SetMode_1LineMid();
    break;
  case UI_MODE_1_LINE_BIG_FONT:
    UI_Manager_SetMode_1LineBig(); // 重绘大字体歌词窗口
    break;
  default:
    break; // HomeLife 未动过（脏更新缓存仍在），无需恢复
  }
}

void UI_Manager_OnProtocolTextReceived(const uint8_t *payload, uint16_t len) {
  UI_Manager_EnterProtocolTextMode();

  static char s_buf[LYRIC_TEXT_SIZE];
  uint16_t n = (len < LYRIC_TEXT_SIZE - 1) ? len : LYRIC_TEXT_SIZE - 1;
  memcpy(s_buf, payload, n);
  s_buf[n] = '\0';

  // 子模式生命周期内 UI 模式不变（切换模式即退出子模式），
  // 用 g_curr_ui_mode 直接表达"渲染跟随当前实际布局"。
  if (g_curr_ui_mode == UI_MODE_MUSIC_INFO) {
    _ApplyDefaultFont();
    _Render_1Line(s_buf, C_RED); // MUSIC_INFO 子模式 → 单行渲染到 win0
  } else {
    UI_Manager_OnTextLineReceived(s_buf);
  }
}

void UI_Manager_NextUIMode(void) {
  UI_Mode_t next = g_curr_ui_mode + 1;
  if (next > UI_MODE_HOME_LIFE)
    next = 0;

  switch (next) {
  case UI_MODE_2_Line:
    UI_Manager_SetMode_2Line();
    break;
  case UI_MODE_MUSIC_INFO:
    if (g_curr_sys_mode == SYS_MODE_PROTOCOL_MODE) {
      UI_Manager_SetMode_MusicInfo();
      break;
    }
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
  // 手动切换系统模式 → 退出协议内文本子模式（含恢复进入前布局）
  UI_Manager_ExitProtocolTextMode();
  g_curr_sys_mode = mode;

  if (g_curr_ui_mode == UI_MODE_2_Line) {
    UI_Manager_SetMode_2Line(); // 重建 2Line 布局以适配新 sys_mode
  } else if (g_curr_ui_mode == UI_MODE_MUSIC_INFO) {
    UI_Manager_SetMode_HomeLife(); // MusicInfo 仅存在于 protocol 模式
  } else {
    LyricWM_Reset();
  }
}

void UI_Manager_ToggleSysMode(void) {
  Sys_Mode_t next = (g_curr_sys_mode == SYS_MODE_TXT_MODE)
                        ? SYS_MODE_PROTOCOL_MODE
                        : SYS_MODE_TXT_MODE;
  UI_Manager_SetSysMode(next);
}

// ========== Tick：主循环唯一入口 ==========

void UI_Manager_OnMusicChanged(void) {
  // 注：歌词池（lyric_pool）以及时间轴已在
  // Lyric_OnMetadataReceived() → LyricService_ClearPool() 中清空，
  // 此处只做 UI 层面的重置
  LyricWM_Reset();

  if (g_curr_ui_mode == UI_MODE_MUSIC_INFO) {
    _Meta_RebuildPool();
    _Meta_RenderToWin(0);
  }
}

void UI_Manager_ToggleScreen(void) {
  s_screen_off = !s_screen_off;
  LED_SetScreenEnable(!s_screen_off);
}

uint8_t UI_Manager_IsScreenOff(void) { return s_screen_off; }

void UI_Manager_WakeScreen(void) {
  if (s_screen_off) {
    s_screen_off = 0;
    LED_SetScreenEnable(1);
  }
}

void UI_Manager_Tick(void) {
  // Step 1: COM 数据消费（息屏时照常运行，不丢数据）
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

  // Step 1.5: 公共媒体切换——切歌时清空歌词池、重置 LyricWM、刷新元数据
  if (media_updated) {
    media_updated = 0;
    UI_Manager_OnMusicChanged();
  }

  // Step 1.6: 全局播放时间轴推进（每帧一次，与渲染路径解耦）
  //   LyricWM_RenderMgr 不再负责推进时间轴；need_commit 短路 /
  //   协议内文本子模式 / 息屏 均不影响时间轴，防止歌词与进度冻结
  LyricWM_UpdatePlayTime();

  // 息屏：跳过渲染和提交
  if (s_screen_off)
    return;

  // Step 2: 模式专属渲染 Tick
  switch (g_curr_ui_mode) {
  case UI_MODE_1_LINE_MID_FONT:
    _Render_1LineMid_Tick();
    break;
  case UI_MODE_HOME_LIFE:
    _Render_HomeLife_Tick();
    break;
  case UI_MODE_MUSIC_INFO:
    _Render_Progress();
    _Render_MetaRotation();
    break;
  default:
    break;
  }

  // Step 3: 窗口/歌词渲染提交
  //   协议内文本子模式下，与 TXT_MODE 一样走 WindowManager_Process()
  if (g_curr_sys_mode == SYS_MODE_TXT_MODE || s_proto_text_active) {
    WindowManager_Process();
  } else {
    LyricWM_RenderMgr();
  }
}
