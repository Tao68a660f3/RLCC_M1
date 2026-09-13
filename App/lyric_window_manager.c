/**
 * @file lyric_window_manager.c
 * @brief 歌词窗口管理：播放时间轴采样 + 歌词渲染调度
 *
 * 职责分层：
 *  - sync_algorithm.c  : 时间轴唯一权威（锚点+斜率模型、钟差/倍速学习、
 *    暂停冻结、Seek/切歌硬复位、看门狗自愈）；
 *  - lyric_service.c   : 只把 0x11 同步包转交算法，不持有任何时间轴状态；
 *  - 本文件：每帧采样一次 Sync_GetTime() 作为播放时间 (s_curr_play_time_ms)，
 *    并驱动歌词窗口的填充 / 换行 / 高亮 / 滚动渲染。
 *
 * 播放时间轴是全局系统状态：
 *  - s_curr_play_time_ms 的取值完全由算法决定（未同步→0 / 暂停→权威冻结值 /
 *    |误差|≤20ms 不动相位 / 大跳单包硬复位），UI 模式切换只清屏，
 *    不参与时间轴推进。
 */

#include "lyric_window_manager.h"
#include "canvas_renderer.h"
#include "flash_font.h"
#include "led_driver.h"
#include "lyric_service.h"
#include "mem_pool.h"
#include "sync_algorithm.h"
#include "window_manager.h"
#include <stdint.h>
#include <string.h>

extern volatile uint8_t need_commit;

/** 每帧一次采样后，所有消费者直接读这个全局值（来源：Sync_GetTime()） */
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
    l_win_cfg[i].color_base = (i % 2) ? C_RED : C_GREEN;
    l_win_cfg[i].color_high = C_YELLOW;

    l_win_cfg[i].offset_x = 0;
    l_win_cfg[i].offset_y = acc_y; // 累加排布
    acc_y += window_list[i].h;
  }
  lyric_total_height = acc_y;

  g_lyric_progress = 0;
}

void LyricWM_Reset(void) {
  // 仅清屏与解绑（UI 模式切换 / 切歌共用）。
  //
  // 注意：不得重置 s_curr_play_time_ms —— 它是全局播放时间轴快照，
  // 取值完全由 sync_algorithm (Sync_GetTime) 决定：
  //   - 切歌：ClearPool 清空歌词池，新时间轴首包由算法硬复位接管；
  //   - 暂停：算法冻结在权威暂停点；
  //   - Seek/变速：算法自行相位斜坡 / 硬复位。
  // 若在此清零，切换 UI 模式（如 HomeLife → 歌词界面）时画面会
  // 从 0 开始追赶当前进度。

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

/** 每帧一次采样后的播放时间 (ms) —— 全局时间轴，来源 Sync_GetTime()。
 *  由 UI_Manager_Tick Step1.6 每帧调用 LyricWM_UpdatePlayTime() 更新一次，
 *  与渲染路径 (LyricWM_RenderMgr / WindowMgr) 解耦。 */
uint32_t Get_Current_PlayTime(void) { return s_curr_play_time_ms; }

/**
 * @brief 每帧采样一次全局播放时间轴（唯一来源：sync_algorithm）
 *
 * 由 UI_Manager_Tick Step1.6 每帧统一调用一次，与渲染路径解耦：
 *   - need_commit 短路 / 协议内文本子模式 / 息屏 均不影响采样
 *   - 渲染端（LyricWM_RenderMgr / WindowMgr）读 Get_Current_PlayTime() 即可
 *
 * Sync_GetTime() 的语义由算法保证：
 *   - 未收到任何同步包 → 0（等价于旧的"未同步 → 0"）
 *   - 暂停 → 权威冻结位置（不再增长）
 *   - 误差 ≤ SYNC_PHASE_SLEW_MIN_MS → 不动相位；大跳 → 单包硬复位
 *   - 预测位置永不超过上位机给的 total_ms
 */
void LyricWM_UpdatePlayTime(void) {
  s_curr_play_time_ms = Sync_GetTime();
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

/**
 * @brief 回收最"过期"的窗口（已结束最久 / 空绑定）
 * @return 目标窗口索引；无可用窗口返回 -1
 */
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

  /* 注：全局播放时间轴已由 UI_Manager_Tick 每帧调用
   * LyricWM_UpdatePlayTime() 统一采样（Sync_GetTime()），
   * need_commit 短路只影响渲染，不影响时间轴。 */

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

        // 清除双缓冲中此窗口物理区域的残留像素（含旧偏移，_RecalcOffsetYByTimeOrder
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