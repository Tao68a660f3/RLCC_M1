#include "lyric_window_manager.h"
#include "canvas_renderer.h"
#include "flash_font.h"
#include "lyric_service.h"
#include "stdio.h"
#include "window_manager.h"
#include <stdint.h>
#include <string.h>

// #define DEBUG_PRT

/**
 * @brief 根据颜色选择对应的画布内存模式
 *        CANVAS_R:  红色像素写入 r_ptr (形状掩码从 r_ptr 读)
 *        CANVAS_G:  绿色像素写入 g_ptr (形状掩码从 g_ptr 读)
 *        CANVAS_Y:  红绿共用同一平面 r_ptr==g_ptr
 */
static CanvasMode _ColorToCanvasMode(CanvasColor color) {
  switch (color) {
  case C_RED:
    return CANVAS_R;
  case C_GREEN:
    return CANVAS_G;
  case C_YELLOW:
    return CANVAS_Y;
  default:
    return CANVAS_R;
  }
}

extern volatile uint8_t need_commit;

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
    l_win_cfg[i].bound_area = NULL;        // 断开指针绑定
    l_win_cfg[i].last_line_index = 0xFFFF; // 重置行号记录

    // 顺便把物理窗口清空（黄底色 → CANVAS_Y）
    Window_FillText(i, " ", C_YELLOW, CANVAS_Y, VALIGN_MIDDLE);
    LED_Window *win = &window_list[l_win_cfg[i].win_idx];
    win->x_offset = 0;
  }
  g_lyric_progress = 0;
}

/**
 * @brief 从系统状态结构体中计算当前播放的绝对时间 (ms)
 *
 * 平滑策略 (EWMA 低通滤波):
 *   - raw = remote_time_ms + (HAL_GetTick() - local_record_tick)
 *   - 当 raw > smooth 时: step = (raw - smooth) / 8，最少 1ms
 *   - 当 raw <= smooth 时: 不后退，仅前进 1ms
 *
 * 效果:
 *   - 正常播放时几乎无延迟跟随
 *   - 200ms 的 sync 跳变约 8 帧 (~250ms) 平滑吸收
 *   - 时间永不后退 (单调递增)
 */
uint32_t Get_Current_PlayTime(void) {
  static uint32_t s_smooth = 0;

  // 如果从未收到过同步包，或者 local_record_tick 尚未初始化
  if (g_sys.local_record_tick == 0) {
    return 0;
  }

  // 如果当前处于暂停状态，时间不再随系统 Tick 累加
  if (!g_sys.is_playing) {
    s_smooth = g_sys.remote_time_ms;
    return g_sys.remote_time_ms;
  }

  // 原始计算 (PC 推算)
  uint32_t raw =
      g_sys.remote_time_ms + (HAL_GetTick() - g_sys.local_record_tick);

  // 首次调用：直接初始化
  if (s_smooth == 0) {
    s_smooth = raw;
    return raw;
  }

// seek 检测阈值 (ms)：超过此值视为用户拖拽进度，立即跟随
#define SEEK_THRESHOLD_MS 300

  // EWMA 一阶低通
  if (raw > s_smooth) {
    uint32_t delta = raw - s_smooth;
    if (delta > SEEK_THRESHOLD_MS) {
      // 用户向前拖拽进度，直接跳转
      s_smooth = raw;
    } else {
      // 微小漂移/抖动：EWMA 平滑跟随
      uint32_t step = (delta + 4) / 8; // delta/8 向上取整
      if (step < 1)
        step = 1;
      if (step > delta)
        step = delta; // 防止过冲
      s_smooth += step;
    }
  } else {
    uint32_t delta = s_smooth - raw;
    if (delta > SEEK_THRESHOLD_MS) {
      // 用户向后拖拽进度，直接跳转
      s_smooth = raw;
    } else {
      // 微小倒退：视为时钟噪声，以 1ms/帧 缓慢前进（保持单调）
      s_smooth += 1;
    }
  }

#undef SEEK_THRESHOLD_MS

  return s_smooth;
}

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

void LyricWM_RenderMgr(void) {
  if (need_commit)
    return;

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
  int display_start_idx = 0;
  for (int i = 0; i < g_sys.active_count; i++) {
    LyricArea *a = g_sys.sorted_lyrics[i];
    if (now > a->start_time_ms && now < (a->start_time_ms + a->duration)) {
      display_start_idx = i;
      break;
    }
    if (now < a->start_time_ms) {
      display_start_idx = (i > 0) ? (i - 1) : 0;
      break;
    }
  }

  // --- 2. 寻找全局进度基准 (active_main) ---
  LyricArea *active_main = NULL;
  for (int i = 0; i < g_sys.active_count; i++) {
    LyricArea *a = g_sys.sorted_lyrics[i];
    if (now >= a->start_time_ms && now < (a->start_time_ms + a->duration)) {
      active_main = a;
      break;
    }
  }
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
        CanvasMode cm = _ColorToCanvasMode(cfg->color_base);

        // 第一步：清除旧内容（清空画布，模式与底色保持一致）
        Window_FillText(cfg->win_idx, " ", cfg->color_base, cm, VALIGN_MIDDLE);

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