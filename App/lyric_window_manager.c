#include "lyric_window_manager.h"
#include "canvas_renderer.h"
#include "flash_font.h"
#include "lyric_service.h"
#include "stdio.h"
#include "window_manager.h"
#include <stdint.h>
#include <string.h>

// #define DEBUG_PRT

extern volatile uint8_t need_commit;

LyricWinConfig l_win_cfg[MAX_LYRIC_LINES];
uint16_t g_lyric_progress = 0;
uint16_t used_lyric_lines = 2;

void LyricWM_Init(void) {
  for (int i = 0; i < MAX_LYRIC_LINES; i++) {
    l_win_cfg[i].win_idx = i;              // 对应 LED_Window 窗口列表的索引
    l_win_cfg[i].bound_area = NULL;        // 关键：初始没绑定任何数据
    l_win_cfg[i].last_line_index = 0xFFFF; // 初始行号
    l_win_cfg[i].last_cmd = 0;

    // 默认配色方案
    l_win_cfg[i].color_base = i % 2 ? C_RED : C_GREEN;
    l_win_cfg[i].color_high = C_YELLOW;

    // 设置位置
    l_win_cfg[i].offset_x = 0;
    l_win_cfg[i].offset_y = i * 16;
  }

  g_lyric_progress = 0;
}

void LyricWM_Reset(void) {
  for (int i = 0; i < MAX_LYRIC_LINES; i++) {
    l_win_cfg[i].bound_area = NULL;        // 断开指针绑定
    l_win_cfg[i].last_line_index = 0xFFFF; // 重置行号记录

    // 顺便把物理窗口清空
    Window_FillText(i, " ", C_YELLOW, CANVAS_R, VALIGN_TOP);
    LED_Window *win = &window_list[l_win_cfg[i].win_idx];
    win->x_offset = 0;
  }
  g_lyric_progress = 0;
}

/**
 * @brief 从系统状态结构体中计算当前播放的绝对时间 (ms)
 */
uint32_t Get_Current_PlayTime(void) {
  // 如果从未收到过同步包，或者 local_record_tick 尚未初始化
  if (g_sys.local_record_tick == 0) {
    return 0;
  }

  // 如果当前处于暂停状态，时间不再随系统 Tick 累加
  if (!g_sys.is_playing) {
    return g_sys.remote_time_ms;
  }

  // 计算逻辑：PC最后一次同步的时间 + (当前运行时间 - 同步时的运行时间)
  uint32_t tick_diff = HAL_GetTick() - g_sys.local_record_tick;
  return g_sys.remote_time_ms + tick_diff;
}

/**
 * @brief 优化版进度计算
 */
static uint16_t _Calculate_Mapped_Progress_Enhanced(LyricArea *area,
                                                    uint32_t elapsed) {
  if (!area || area->duration == 0)
    return 0;

  // --- 情况 A: 逐字歌词 (0x14) ---
  // 注意：此处假设 area->time_offsets[i] 是相对于行开始的时间戳
  // 数组有效长度为 area->word_count + 1 (最后一个是全行结束时间)
  if (area->cmd == 0x14 && area->word_count > 0) {
    const int SKIP_WORDS = 1;         // 跳过前1个字开始滚
    const int FINISH_EARLY_WORDS = 1; // 提前1个字滚完

    // 直接从 time_offsets 数组获取起始和结束时间点
    // start_offset: 第 SKIP_WORDS 个词开始的时间
    uint32_t start_offset = area->time_offsets[SKIP_WORDS];

    // end_offset: 倒数第 FINISH_EARLY_WORDS 个词开始的时间
    int end_idx = area->word_count - FINISH_EARLY_WORDS - 1;
    if (end_idx < 0)
      end_idx = 0;
    uint32_t end_offset = area->time_offsets[end_idx];

    // 安全检查：防止偏移量计算逻辑导致除以0或倒流
    if (end_offset > start_offset) {
      if (elapsed <= start_offset)
        return 0;
      if (elapsed >= end_offset)
        return 10000;

      return (uint16_t)(((elapsed - start_offset) * 10000) /
                        (end_offset - start_offset));
    }
  }

  // --- 情况 B: 普通歌词或保底 (HEAD_RATE/TAIL_RATE) ---
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

  // 找到当前时间落在哪一个词的区间内
  uint16_t acc_px = 0;
  for (int i = 0; i < area->word_count; i++) {
    uint32_t w_start = area->time_offsets[i];
    uint32_t w_end =
        (i < area->word_count - 1) ? area->time_offsets[i + 1] : area->duration;

    if (elapsed >= w_start && elapsed < w_end) {
      // 词内线性插值：当前词已走过的像素
      uint32_t w_dur = w_end - w_start;
      uint32_t w_elapsed = elapsed - w_start;
      uint16_t w_px = (w_elapsed * area->word_widths[i]) / w_dur;
      return acc_px + w_px;
    }
    acc_px += area->word_widths[i];
  }
  return acc_px; // 唱完了
}

/**
 * @brief 虚拟测量：严格基于 word_lens 进行分段宽度统计
 * 使用 Flash 字库适配器替代旧的 font_engine
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
          rem--; // 容错处理
        }
      }
      Canvas_MeasureStep(&ctx, &g, &size);
    }
    area->word_widths[i] = (uint8_t)size.w;
  }
}

/**
 * @brief 歌词着色渲染器 (优化版：采用块搬运)
 *
 * 替代原逐点 _Lyric_RenderWithShader，改为调用 Window_BlitToScreen。
 * Canvas 作为形状掩码 (CANVAS_R 单色画布)，h_px 决定高亮/底色分界。
 * screen_off_x/y 携带 cfg->offset 偏移，歌词着色在搬运阶段即时完成。
 */
static void _Lyric_RenderWithShader(LyricWinConfig *cfg, uint16_t h_px) {
  LED_Window *win = &window_list[cfg->win_idx];
  if (win->canvas.handle == -1)
    return;

  // 调用统一块搬运函数，h_px 传给 Window_BlitToScreen 触发歌词着色模式
  // off_x/off_y 传 cfg->offset_x/y 实现歌词窗口的屏幕偏移
  Window_BlitToScreen(win, h_px, cfg->color_high, cfg->color_base,
                      cfg->offset_x, cfg->offset_y);
}

static int _RecycleWindow(uint32_t now) {
  int target_w = -1;
  uint32_t max_overdue = 0;
  for (int w = 0; w < MAX_LYRIC_LINES; w++) {
    if (l_win_cfg[w].is_occupied)
      continue;
    if (l_win_cfg[w].bound_area == NULL)
      return w; // 优先返回空位

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

void LyricWM_RenderMgr(void) {
  if (need_commit)
    return;

  for (uint16_t i = MAX_LYRIC_LINES; i < MAX_WINDOWS; i++) {
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
        l_win_cfg[w].offset_y = i * 16;
        break;
      }
    }
  }

  // --- 4. 第二阶段：【新兵入伍/强行重绘】 ---
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
        if (target_area->cmd == 0x14)
          _Lyric_MeasureVerbatim(target_area);

        // 歌词 Canvas 仅作为形状掩码，CANVAS_R 省 50% 内存
        Window_FillText(cfg->win_idx, target_area->text, cfg->color_base,
                        CANVAS_R, VALIGN_TOP);

        cfg->bound_area = target_area;
        cfg->last_line_index =
            target_area->line_index; // 记录行号，下次跳转时靠它识别刷新
        cfg->is_occupied = 1;
        cfg->offset_y = i * 16;
      }
    }
  }

  // --- 5. 第三阶段：【渲染提交】 ---
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