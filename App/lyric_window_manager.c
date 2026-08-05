#include "lyric_window_manager.h"
#include "canvas_renderer.h"
#include "flash_font.h"
#include "led_driver.h"
#include "lyric_service.h"
#include "mem_pool.h"
#include "stdio.h"
#include "window_manager.h"
#include <stdint.h>
#include <string.h>

// #define DEBUG_PRT

extern volatile uint8_t need_commit;

/** 每帧一次平滑时间更新后，所有消费者直接读这个全局值 */
static uint32_t s_curr_play_time_ms = 0;

LyricWinConfig l_win_cfg[MAX_LYRIC_LINES];
uint16_t g_lyric_progress = 0;
uint16_t used_lyric_lines = 2;
uint16_t lyric_total_height = 0;

void LyricWM_Init(uint8_t line_count) {
  if (line_count > MAX_LYRIC_LINES)
    line_count = MAX_LYRIC_LINES;
  used_lyric_lines = line_count;

  uint16_t acc_y = 0;
  for (int i = 0; i < used_lyric_lines; i++) {
    l_win_cfg[i].win_idx = i; // 对应 LED_Window 窗口列表的索引
    l_win_cfg[i].bound_area = NULL;
    l_win_cfg[i].last_line_index = 0xFFFF;
    l_win_cfg[i].last_cmd = 0;

    // 默认配色方案
    l_win_cfg[i].color_base = i % 2 ? C_RED : C_GREEN;

    l_win_cfg[i].color_high = C_YELLOW;

    l_win_cfg[i].offset_x = 0;
    l_win_cfg[i].offset_y = acc_y; // 累加排布
    acc_y += window_list[i].h;
  }
  lyric_total_height = acc_y;

  g_lyric_progress = 0;
}

void LyricWM_Reset(void) {
  for (int i = 0; i < used_lyric_lines; i++) {
    // 清除当前 write_idx 的 canvas 内容
    Window_FillText(i, " ", C_YELLOW, CANVAS_Y, VALIGN_MIDDLE);

    // 清除双缓冲中此窗口物理区域的残留像素（含偏移叠加）
    LED_Window *win = &window_list[i];
    LED_ClearAreaAllBuffers(win->x + (int16_t)l_win_cfg[i].offset_x,
                            win->y + (int16_t)l_win_cfg[i].offset_y, win->w,
                            win->h);

    l_win_cfg[i].bound_area = NULL;        // 断开指针绑定
    l_win_cfg[i].last_line_index = 0xFFFF; // 重置行号记录

    win->x_offset = 0;
  }
  g_lyric_progress = 0;
}

/**
 * @brief 130FPS 纯整数 PID 歌词时间轴 Smoothing 算法
 * 每帧在 LyricWM_RenderMgr 入口调用一次，更新平滑时间 纯整数微秒级 PID
 * 速度微调 Smoothing 运行环境：STM32F401 @ 130FPS (dt ≈ 7.69ms)
 *
 * @note  【核心算法说明与开发者血泪警告】
 * 1. 本算法专为 1.0x 正常人类听歌速度极速优化，精度锁定在 1 微秒 (0.001ms)。
 * 2. 限制 PID 速度微调区间为 [0.900x, 1.100x] (900 ~
 * 1100)，旨在抹平蓝牙/串口抖动。
 *
 * @warning
 *        🚨 警告：严禁使用 0.1x 鬼畜慢放 或 4.0x 极速狂飙模式听歌！
 *        开发者已亲身试毒，听完脑袋直接爆炸！🤯
 *        若强行开启非人道倍速，由于 PID 限幅保护，时间轴会发生频繁 Seek
 * 强制跳变（即屏幕抽搐）。 这不是
 * Bug，这是硬件对奇葩听歌习惯发出的【物理抗议】！请受着！🤪
 */
static void _UpdateSmoothTime(void) {
  static uint32_t s_smooth_ms = 0;
  static uint32_t s_smooth_sub_ms = 0; // 毫秒的小数部分 (范围 0~999，即微秒)
  static uint32_t s_last_tick = 0;

  uint32_t now_tick = HAL_GetTick();
  if (s_last_tick == 0) {
    s_last_tick = now_tick;
  }
  uint32_t dt = now_tick - s_last_tick; // 130FPS 下 dt 通常为 7ms 或 8ms
  s_last_tick = now_tick;

  // 1. 边界拦截
  if (g_sys.local_record_tick == 0) {
    s_curr_play_time_ms = 0;
    return;
  }
  if (!g_sys.is_playing) {
    s_smooth_ms = g_sys.remote_time_ms;
    s_smooth_sub_ms = 0;
    s_curr_play_time_ms = g_sys.remote_time_ms;
    return;
  }

  // 2. 计算原始推算时间
  uint32_t raw = g_sys.remote_time_ms + (now_tick - g_sys.local_record_tick);

  if (s_smooth_ms == 0) {
    s_smooth_ms = raw;
    s_smooth_sub_ms = 0;
    s_curr_play_time_ms = raw;
    return;
  }

#define SEEK_THRESHOLD_MS 500

  // 3. 计算时间差 error
  int32_t error = (int32_t)raw - (int32_t)s_smooth_ms;

  // 4. 大跨步拖动进度条 (Seek)
  if (error > SEEK_THRESHOLD_MS || error < -SEEK_THRESHOLD_MS) {
    s_smooth_ms = raw;
    s_smooth_sub_ms = 0;
  } else {
    // 5. 纯整数 PID 速度微调
    // 基准速度 ratio = 1000 (代表 1.000x)
    // Kp = 2，即每落后 1ms，速度提升 2/1000 = 0.2%
    int32_t speed_ratio = 1000 + (error * 2);

    // 限制微调上限为 0.900x ~ 1.100x (900 ~ 1100)
    // 130FPS 下肉眼对 ±10% 的速度微调绝对无法感知，但 1 秒内能轻松抹平 100ms
    // 的抖动！
    if (speed_ratio > 1100)
      speed_ratio = 1100;
    if (speed_ratio < 900)
      speed_ratio = 900;

    // 6. 高精度整数累加
    // 实际增加的微毫秒 = dt * speed_ratio
    // 比如 dt=8ms, speed_ratio=1020, 增加 8160 (即 8.16ms)
    uint32_t total_sub = s_smooth_sub_ms + (dt * (uint32_t)speed_ratio);

    s_smooth_ms += total_sub / 1000;    // 进位到整数毫秒
    s_smooth_sub_ms = total_sub % 1000; // 留存小数微秒
  }

#undef SEEK_THRESHOLD_MS

  s_curr_play_time_ms = s_smooth_ms;
}

/**
 * @brief 读取当前播放时间 (ms)
 *
 * 注意：此函数已是轻量读全局变量，每帧可多次调用。
 *       实际平滑时间更新由 _UpdateSmoothTime 每帧仅执行一次。
 */
uint32_t Get_Current_PlayTime(void) { return s_curr_play_time_ms; }

/**
 * @brief 优化版进度计算
 */
static uint16_t _Calculate_Mapped_Progress_Enhanced(LyricArea *area,
                                                    uint32_t elapsed) {
  if (!area || area->duration == 0)
    return 0;

  // --- 情况 A: 逐字歌词 (0x14) ---
  if (area->cmd == 0x14 && area->word_count > 0) {
    const int SKIP_WORDS = 1;
    const int FINISH_EARLY_WORDS = 1;

    uint32_t start_offset = area->time_offsets[SKIP_WORDS];
    int end_idx = area->word_count - FINISH_EARLY_WORDS - 1;
    if (end_idx < 0)
      end_idx = 0;
    uint32_t end_offset = area->time_offsets[end_idx];

    if (end_offset > start_offset) {
      if (elapsed <= start_offset)
        return 0;
      if (elapsed >= end_offset)
        return 10000;

      return (uint16_t)(((elapsed - start_offset) * 10000) /
                        (end_offset - start_offset));
    }
  }

  // --- 情况 B: 普通歌词或保底 ---
  uint32_t head_time = (area->duration * HEAD_RATE) / 10000;
  uint32_t tail_start_time =
      area->duration - (area->duration * TAIL_RATE) / 10000;

  if (elapsed <= head_time)
    return 0;
  if (elapsed >= tail_start_time)
    return 10000;

  uint32_t effective_dur = tail_start_time - head_time;
  return (uint16_t)(((elapsed - head_time) * 10000) / effective_dur);
}

/**
 * @brief 计算高亮像素边界 (三板斧插值)
 */
uint16_t Lyric_GetHighlightPx(LyricArea *area, uint32_t current_ms) {
  if (current_ms < area->start_time_ms)
    return 0;
  uint32_t elapsed = current_ms - area->start_time_ms;

  uint16_t acc_px = 0;
  for (int i = 0; i < area->word_count; i++) {
    uint32_t w_start = area->time_offsets[i];
    uint32_t w_end =
        (i < area->word_count - 1) ? area->time_offsets[i + 1] : area->duration;

    if (elapsed >= w_start && elapsed < w_end) {
      uint32_t w_dur = w_end - w_start;
      uint32_t w_elapsed = elapsed - w_start;
      uint16_t w_px = (w_elapsed * area->word_widths[i]) / w_dur;
      return acc_px + w_px;
    }
    acc_px += area->word_widths[i];
  }
  return acc_px;
}

/**
 * @brief 虚拟测量：严格基于 word_lens 进行分段宽度统计
 */
void _Lyric_MeasureVerbatim(LyricArea *area) {
  if (area->cmd != 0x14)
    return;

  for (int i = 0; i < area->word_count; i++) {
    DrawContext ctx = {0};
    Size2D size = {0};
    ctx.padding = 0;

    const char *p = area->word_ptrs[i];
    uint8_t rem = area->word_lens[i];

    while (rem > 0) {
      GlyphInfo g;
      if ((uint8_t)*p < 0x80) { // ASCII
        Font_Flash_ASC_Adapter((uint8_t)*p, &g, 0);
        p++;
        rem--;
      } else { // GBK (双字节)
        if (rem >= 2) {
          uint16_t gbk = ((uint16_t)(uint8_t)*p << 8) | (uint8_t)*(p + 1);
          Font_Flash_GBK_Adapter(gbk, &g, 0);
          p += 2;
          rem -= 2;
        } else {
          p++;
          rem--;
        }
      }
      Canvas_MeasureStep(&ctx, &g, &size);
    }
    area->word_widths[i] = (uint8_t)size.w;
  }
}

/**
 * @brief 歌词着色渲染器
 */
static void _Lyric_RenderWithShader(LyricWinConfig *cfg, uint16_t h_px) {
  LED_Window *win = &window_list[cfg->win_idx];
  if (win->canvas.handle == -1)
    return;

  Window_BlitToScreen(win, h_px, cfg->color_high, cfg->color_base,
                      cfg->offset_x, cfg->offset_y);
}

static int _RecycleWindow(uint32_t now) {
  int target_w = -1;
  uint32_t max_overdue = 0;
  for (int w = 0; w < used_lyric_lines; w++) {
    if (l_win_cfg[w].is_occupied)
      continue;
    if (l_win_cfg[w].bound_area == NULL)
      return w;

    uint32_t end = l_win_cfg[w].bound_area->start_time_ms +
                   l_win_cfg[w].bound_area->duration;
    uint32_t overdue = (now > end) ? (now - end) : 0;
    if (overdue >= max_overdue) {
      max_overdue = overdue;
      target_w = w;
    }
  }
  return target_w;
}

/**
 * @brief 对活跃窗口按歌词时间排序，从老到新累加 offset_y
 *
 * 实现方法：
 *   1. 收集所有 [is_occupied && bound_area != NULL] 的窗口
 *   2. 按它们在 sorted_lyrics 中的位置排序（老→新）
 *   3. 从 0 开始累加物理窗口高度，写入 offset_y
 */
static void _RecalcOffsetYByTimeOrder(void) {
  // 收集活跃窗口的索引与排序键
  uint8_t active_idx[MAX_LYRIC_LINES];
  uint32_t sort_key[MAX_LYRIC_LINES];
  uint8_t active_count = 0;

  for (int w = 0; w < used_lyric_lines; w++) {
    LyricWinConfig *cfg = &l_win_cfg[w];
    if (!cfg->is_occupied || cfg->bound_area == NULL)
      continue;

    // 在 sorted_lyrics 中查找该窗口绑定的 LyricArea 的位置
    // 直接用 sorted_lyrics 下标作为排序键，继承 _Lyric_UpdateSortedArray
    // 的完整顺序
    uint32_t pos = 0xFFFFFFFF;
    for (uint32_t i = 0; i < g_sys.active_count; i++) {
      if (g_sys.sorted_lyrics[i] == cfg->bound_area) {
        pos = i;
        break;
      }
    }
    sort_key[active_count] = pos;
    active_idx[active_count] = w;
    active_count++;
  }

  // 简单冒泡排序（窗口数 <= 4，足够快）
  for (uint8_t i = 0; i < active_count; i++) {
    for (uint8_t j = i + 1; j < active_count; j++) {
      if (sort_key[j] < sort_key[i]) {
        uint32_t tmp_k = sort_key[i];
        sort_key[i] = sort_key[j];
        sort_key[j] = tmp_k;
        uint8_t tmp_i = active_idx[i];
        active_idx[i] = active_idx[j];
        active_idx[j] = tmp_i;
      }
    }
  }

  // 累加重写 offset_y
  uint16_t acc_y = 0;
  for (uint8_t i = 0; i < active_count; i++) {
    LyricWinConfig *cfg = &l_win_cfg[active_idx[i]];
    cfg->offset_y = acc_y;
    acc_y += window_list[cfg->win_idx].h;
  }
}

/** @brief 寻找显示起始点，每帧全量遍历计算 */
static int _FindDisplayStartIdx(uint32_t now) {
  for (int i = 0; i < g_sys.active_count; i++) {
    LyricArea *a = g_sys.sorted_lyrics[i];

    // case 1: 当前正在播放的行
    if (now >= a->start_time_ms && now <= (a->start_time_ms + a->duration))
      return i;

    // case 2: 尚未到达的行 → 退 1 行重叠
    if (now < a->start_time_ms)
      return (i > 0) ? (i - 1) : 0;

    // case 3: 当前行已过期，检查下一行是否需要提前切换
    if ((i + 1) < g_sys.active_count) {
      LyricArea *b = g_sys.sorted_lyrics[i + 1];
      if (b->cmd == 0x13 && i + 2 < g_sys.active_count)
        b = g_sys.sorted_lyrics[i + 2];
      if (now + 1500 >= b->start_time_ms &&
          b->start_time_ms > (a->start_time_ms + a->duration))
        return i + 1;
    }
  }
  return 0;
}

/** @brief 寻找当前播放区间内的歌词行作为进度基准 */
static LyricArea *_FindActiveMain(uint32_t now) {
  for (int i = 0; i < g_sys.active_count; i++) {
    LyricArea *a = g_sys.sorted_lyrics[i];
    if (now >= a->start_time_ms && now < (a->start_time_ms + a->duration))
      return a;
  }
  return NULL;
}

void LyricWM_RenderMgr(void) {
  if (need_commit)
    return;

  /* === 每帧仅在此处更新一次平滑时间 === */
  _UpdateSmoothTime();

  for (uint16_t i = used_lyric_lines; i < MAX_WINDOWS; i++) {
    WindowManager_Single_Process(i);
  }

  LyricWM_Process();

  need_commit = 1;
}

void LyricWM_Process(void) {
  uint32_t now = Get_Current_PlayTime();

  // --- 0. 准备工作 ---
  for (int w = 0; w < used_lyric_lines; w++)
    l_win_cfg[w].is_occupied = 0;

  // --- 1. 寻找显示起始点 ---
  int display_start_idx = _FindDisplayStartIdx(now);
  if (display_start_idx == -1)
    return;

  // --- 2. 寻找全局进度基准 ---
  LyricArea *active_main = _FindActiveMain(now);
  g_lyric_progress = active_main
                         ? _Calculate_Mapped_Progress_Enhanced(
                               active_main, now - active_main->start_time_ms)
                         : 0;

  // --- 3. 第一阶段：【老兵登记】 ---
  for (int i = 0; i < used_lyric_lines; i++) {
    int lyric_idx = display_start_idx + i;
    if (lyric_idx >= g_sys.active_count)
      break;
    LyricArea *target_area = g_sys.sorted_lyrics[lyric_idx];

    for (int w = 0; w < used_lyric_lines; w++) {
      if (l_win_cfg[w].bound_area == target_area &&
          l_win_cfg[w].last_line_index == target_area->line_index) {
        l_win_cfg[w].is_occupied = 1;
        break;
      }
    }
  }

  // --- 4. 第二阶段：【新兵入伍/清除旧内容 + 填充新内容】 ---
  for (int i = 0; i < used_lyric_lines; i++) {
    int lyric_idx = display_start_idx + i;
    if (lyric_idx >= g_sys.active_count)
      break;
    LyricArea *target_area = g_sys.sorted_lyrics[lyric_idx];

    uint8_t already_active = 0;
    for (int w = 0; w < used_lyric_lines; w++) {
      if (l_win_cfg[w].bound_area == target_area && l_win_cfg[w].is_occupied) {
        already_active = 1;
        break;
      }
    }

    if (!already_active) {
      int target_w = _RecycleWindow(now);
      if (target_w != -1) {
        LyricWinConfig *cfg = &l_win_cfg[target_w];
        CanvasMode cm = ColorToCanvasMode(cfg->color_base);

        // 第一步：清除旧内容（清空画布，模式与底色保持一致）
        Window_FillText(cfg->win_idx, " ", cfg->color_base, cm, VALIGN_MIDDLE);

        // 清除双缓冲中此窗口物理区域的残留像素（含旧偏移偏移，_RecalcOffsetYByTimeOrder
        // 尚未更新）
        LED_Window *win = &window_list[cfg->win_idx];
        LED_ClearAreaAllBuffers(win->x + (int16_t)cfg->offset_x,
                                win->y + (int16_t)cfg->offset_y, win->w,
                                win->h);

        // 第二步：（移动窗口——在 _RecalcOffsetYByTimeOrder 中统一处理）

        // 第三步：填充新歌词内容（模式与底色保持一致）
        if (target_area->cmd == 0x14)
          _Lyric_MeasureVerbatim(target_area);

        Window_FillText(cfg->win_idx, target_area->text, cfg->color_base, cm,
                        VALIGN_MIDDLE);

        cfg->bound_area = target_area;
        cfg->last_line_index = target_area->line_index;
        cfg->is_occupied = 1;
      }
    }
  }

  // --- 4.5 第三步：【按时间顺序重算竖直位置】 ---
  _RecalcOffsetYByTimeOrder();

  // --- 5. 第四阶段：【渲染提交】 ---
  for (int w = 0; w < used_lyric_lines; w++) {
    LyricWinConfig *cfg = &l_win_cfg[w];
    if (cfg->bound_area == NULL)
      continue;

    if (!cfg->is_occupied) {
      cfg->bound_area = NULL;
      continue;
    }

    LyricArea *area = cfg->bound_area;
    LED_Window *win = &window_list[cfg->win_idx];

    if (win->canvas.width > win->w) {
      int16_t max_s = win->canvas.width - win->w;
      if (area == active_main ||
          (active_main && area->line_index == active_main->line_index)) {
        win->x_offset = -(int16_t)((g_lyric_progress * max_s) / 10000);
      } else if (now >= area->start_time_ms + area->duration) {
        win->x_offset = -max_s;
      } else {
        win->x_offset = 0;
      }
    } else {
      Window_SetAlignment(cfg->win_idx, ALIGN_CENTER);
    }

    uint16_t hp = (area->cmd == 0x14) ? Lyric_GetHighlightPx(area, now) : 0;
    _Lyric_RenderWithShader(cfg, hp);
  }
}