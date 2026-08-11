/**
 * @file lyric_window_manager.c
 * @brief 歌词窗口管理：播放时间轴平滑外推 + 歌词渲染调度
 *
 * 职责分层：
 *  - lyric_service.c   : 协议解析与全局时间轴锚点 (g_sys) 维护；
 *  - 本文件：把锚点外推为平滑播放时间 (s_pi_time)，并驱动歌词窗口的
 *    填充 / 换行 / 高亮 / 滚动渲染。
 *
 * 播放时间轴是全局系统状态：
 *  - s_pi_time / s_pi_last_tick 的生命周期由 _UpdateSmoothTime 的边界
 *    条件管理（未同步 / 暂停 / 变速 / Seek），UI 模式切换只清屏，
 *    不得清零，否则重进歌词界面会从 0 追赶当前进度。
 */

#include "lyric_window_manager.h"
#include "canvas_renderer.h"
#include "flash_font.h"
#include "led_driver.h"
#include "lyric_service.h"
#include "mem_pool.h"
#include "window_manager.h"
#include <stdint.h>
#include <string.h>

extern volatile uint8_t need_commit;

/** 每帧一次平滑时间更新后，所有消费者直接读这个全局值 */
static uint32_t s_curr_play_time_ms = 0;

/** PI 平滑后的播放时间 (ms) —— 全局时间轴，UI 切换不清零 */
static uint32_t s_pi_time = 0;
/** 上次 PI 计算的墙钟 (ms) */
static uint32_t s_pi_last_tick = 0;

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
  // 注意：不得重置 PI 平滑状态 s_pi_time / s_pi_last_tick——
  // 它们是全局时间轴，生命周期由 _UpdateSmoothTime 边界条件管理：
  //   - 切歌：ClearPool → clock_synced=0 → 下帧自动清零并重建；
  //   - 暂停：is_playing=0 → 冻结在最近已知进度；
  //   - 变速/Seek：playback_speed_changed=1 → 硬跳对齐新锚点。
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

/**
 * @brief 130FPS 时间轴算法：speed 感知外推 + PI 平滑控推进速率
 * 每帧由 LyricWM_UpdatePlayTime()（UI_Manager_Tick Step1.6）调用一次，
 * 与渲染路径（LyricWM_RenderMgr / WindowMgr）解耦。
 *
 * 原理：
 *   0x11 同步包在 Lyric_OnSyncReceived 中已建立锚点
 *   (base_remote_time / local_send_tick / playback_speed)，此后推算
 *   阶段完全不依赖 UDP 接收时刻 → 免疫网络延迟抖动。
 *
 *   目标值 target = base_remote_time + 墙钟流逝 × speed
 *   (含变速感知，与 C# 播放器速率同频)。
 *
 *   渲染端用 PI 平滑跟踪 target，分三种行为：
 *   - 正常播放：s_pi_time 以 speed 速率推进，误差用 P 项低通吸收
 *   - 变速确认：speed 更新 + 锚点重建 → target 拉开差距，
 *     P 项逐帧收敛（歌词平滑追赶，不瞬移）
 *   - Seek/暂停恢复：硬跳重建锚点 + playback_speed_changed 事件
 *     → s_pi_time 直接对齐 base_remote_time（瞬移接管）
 *
 * 边界：
 *   - 未完成首次同步 → 返回 0
 *   - 暂停 → 冻结在最近已知进度
 *   - 卡帧/异常间隔 → elapsed 钳制，防瞬移
 */
// ===== 渲染端 PI 平滑调参 =====
// P 项系数 (Q16)：调大→变速追赶快(输出偏抖)；调小→稳(追赶滞后)
#define PI_KP_Q16 3200
// 单帧 P 项补偿上限 (ms)：防异常误差(如一次大 Seek)导致歌词瞬移
#define PI_COMP_MAX_MS 40
// 单帧墙钟上限 (ms)：防长时间卡帧后恢复时的瞬移
#define PI_FRAME_MAX_MS 100

static void _UpdateSmoothTime(void) {
  uint32_t now_tick = HAL_GetTick();

  // 1. 边界拦截：未同步（切歌后 ClearPool 清 clock_synced，自动回到此路径）
  if (!g_sys.clock_synced) {
    s_curr_play_time_ms = 0;
    s_pi_time = 0;
    s_pi_last_tick = now_tick;
    return;
  }

  // 2. 暂停：冻结在最近已知进度
  if (!g_sys.is_playing) {
    s_curr_play_time_ms = g_sys.base_remote_time;
    s_pi_time = g_sys.base_remote_time;
    s_pi_last_tick = now_tick;
    return;
  }

  // 3. 变速/Seek 事件：直接对齐新锚点（硬跳接管），重置 PI
  if (g_sys.playback_speed_changed) {
    g_sys.playback_speed_changed = 0;
    s_pi_time = g_sys.base_remote_time;
    s_pi_last_tick = now_tick;
  }

  // 4. 目标 = 锚点 + 墙钟流逝 × speed（Q8，含变速感知）
  uint32_t elapsed = now_tick - s_pi_last_tick;
  if (elapsed > PI_FRAME_MAX_MS) {
    elapsed = PI_FRAME_MAX_MS;
    s_pi_last_tick = now_tick - elapsed; // 平移，避免累积误差
  }
  uint32_t target =
      (uint32_t)((int64_t)g_sys.base_remote_time +
                 (int64_t)((int32_t)now_tick - (int32_t)g_sys.local_send_tick) *
                     (int32_t)g_sys.playback_speed / (int32_t)256);

  // 5. 推进速率 = elapsed × speed（speed 感知，与锚点同步增长）
  int32_t frame_step =
      (int32_t)((int64_t)elapsed * g_sys.playback_speed / (int32_t)256);
  s_pi_time = (uint32_t)((int32_t)s_pi_time + frame_step);

  // 6. P 项误差补偿：低通吸收 target 与 s_pi_time 的偏差
  int32_t err = (int32_t)target - (int32_t)s_pi_time;
  int32_t comp = (int32_t)((int64_t)err * PI_KP_Q16 / 65536);
  if (comp > PI_COMP_MAX_MS)
    comp = PI_COMP_MAX_MS;
  if (comp < -PI_COMP_MAX_MS)
    comp = -PI_COMP_MAX_MS;
  s_pi_time = (uint32_t)((int32_t)s_pi_time + comp);
  s_pi_last_tick = now_tick;

  // 7. 封顶总时长
  if (g_sys.total_ms > 0 && s_pi_time > g_sys.total_ms)
    s_curr_play_time_ms = g_sys.total_ms;
  else
    s_curr_play_time_ms = s_pi_time;
}

/**
 * @brief 读取当前播放时间 (ms)
 *
 * 注意：此函数已是轻量读全局变量，每帧可多次调用。
 *       实际平滑时间更新由 _UpdateSmoothTime 每帧仅执行一次。
 */
uint32_t Get_Current_PlayTime(void) { return s_curr_play_time_ms; }

/**
 * @brief 每帧推进全局播放时间轴（薄封装）
 *
 * 由 UI_Manager_Tick 每帧统一调用一次，与渲染路径解耦：
 *   - need_commit 短路 / 协议内文本子模式 / 息屏 均不影响时间轴推进
 *   - 渲染端（LyricWM_RenderMgr / WindowMgr）读 Get_Current_PlayTime() 即可
 */
void LyricWM_UpdatePlayTime(void) { _UpdateSmoothTime(); }

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
   * LyricWM_UpdatePlayTime() 统一推进，本处不再调用 _UpdateSmoothTime。
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