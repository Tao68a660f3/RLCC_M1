/**
 * @file lyric_service.c
 * @brief 歌词服务：协议解析 + 时间轴输入转发 + 歌词池管理
 *
 * 职责（分层）：
 *  - 本文件：解析 C# 端歌词协议包（0x10~0x15），把 0x11 的四个字段原样
 *    转交 sync_algorithm（Sync_OnPacketAt）。本文件不再维护任何锚点 /
 *    变速 / 抖动 / 漂移状态 —— 时间轴的全部状态都在 sync_algorithm 内部；
 *  - sync_algorithm.c：时间轴唯一权威（锚点+斜率模型、钟差与倍速学习、
 *    暂停冻结、Seek/切歌硬复位、看门狗自愈）；
 *  - lyric_window_manager.c：每帧采样 Sync_GetTime() 并渲染歌词。
 *
 * 协议约定（与上位机 MediaMonitor / sync_algorithm readme 一致）：
 *  - current_ms 是"上位机估计的、本机收到该包时的时间轴位置"，
 *    上位机自行补偿链路延迟（即用「同步偏移(ms)」填延迟测试所得的 Base）；
 *  - 暂停 (is_playing=0) 时携带的是已冻结的权威位置；
 *  - upstream_tick_ms 是上位机自己的单调毫秒 tick。
 *
 * 非对齐安全说明：协议 payload 为串口 DMA 字节流，偏移 1/2/5/6/9 等
 * 均非 4 对齐，任何 *(uint32_t*) / *(uint16_t*) 强转解引用都会触发
 * M4 UNALIGNED HardFault，必须用 memcpy 逐字段拷贝。
 */

#include "lyric_service.h"
#include "lyric_window_manager.h"
#include "sync_algorithm.h"
#include "stdbool.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// =====================================================================
// 时间轴调参全部集中在 sync_algorithm.h
// =====================================================================
// 本文件已不再维护任何锚点 / 变速 / 抖动 / 漂移参数，相关宏改到：
//   - 硬复位门限：SYNC_HARD_RESET_MS / SYNC_HARD_RESET_FAST_MS
//   - 相位斜坡  ：SYNC_PHASE_SLEW_MIN_MS(误差≤20ms 不动相位，需 A 端补偿) /
//                 SYNC_SLEW_MS_PER_S / SYNC_SLEW_DIV
//   - 钟差与倍速：SYNC_DRIFT_* / SYNC_RATE_* / SYNC_PB_* / SYNC_RATE_PPM_LIMIT
//   - 过期观测  ：SYNC_EXCESS_*
//   - 看门狗    ：SYNC_RESYNC_AFTER_MS
// 改这些宏前请先读 fake_stm32_sync_simulator/readme.md 的对应章节。

SystemStatus g_sys;
uint8_t media_updated = 0;

void LyricService_Init(void) {
  memset(&g_sys, 0, sizeof(g_sys));
  Sync_Init(); // 时间轴模块：唯一的一次初始化（切歌不复位，见 ClearPool）
}

void LyricService_ClearPool(void) {
  // 清空歌词池
  memset(g_sys.lyric_pool, 0, sizeof(g_sys.lyric_pool));
  memset(g_sys.sorted_lyrics, 0, sizeof(g_sys.sorted_lyrics));
  g_sys.active_count = 0;

  // 时间轴输入镜像清零（纯诊断字段；时间轴状态在 sync_algorithm 内部）
  g_sys.remote_time_ms = 0;
  g_sys.total_ms = 0;
  g_sys.upstream_tick_ms = 0;

  // 注意：切歌时绝不复位 sync_algorithm（不要在这里加 Sync_Init()）：
  //   1) 上位机切歌是「0x10 元数据」紧接「0x11 同步包」，新时间轴首包误差
  //      远超 SYNC_HARD_RESET_FAST_MS，算法会单包内硬复位直接接管；
  //   2) Sync_Init() 会清空 epoch 基线 / 钟差 / 倍速等已建立的学习状态，
  //      复位反而让算法从零重学（秒级~分钟级的精度损失）。
  // 未收到任何同步包时 Sync_GetTime() 返回 0，与旧的"未同步→0"等价。

  media_updated = 1;
}

// 排序优先级：0x15(增强原文) -> 0x12(原文) -> 0x13(翻译) -> 其他
static uint8_t _CmdRank(uint8_t cmd) {
  switch (cmd) {
  case 0x15:
    return 0;
  case 0x12:
    return 1;
  case 0x13:
    return 2;
  default:
    return 3;
  }
}

static void _Lyric_UpdateSortedArray(void) {
  g_sys.active_count = 0;

  // 1. 收集所有有效指针
  for (int i = 0; i < LYRIC_POOL_SIZE; i++) {
    if (g_sys.lyric_pool[i].is_valid) {
      g_sys.sorted_lyrics[g_sys.active_count++] = &g_sys.lyric_pool[i];
    }
  }

  // 2. 基础排序：Index 升序为主；同 Index 时按级别升序
  //    (0x15 与 0x12 同属原文，0x15 在前；0x13 翻译排最后)
  for (int i = 0; i < g_sys.active_count; i++) {
    for (int j = i + 1; j < g_sys.active_count; j++) {
      LyricArea *a = g_sys.sorted_lyrics[i];
      LyricArea *b = g_sys.sorted_lyrics[j];
      bool swap = false;
      if (a->line_index > b->line_index)
        swap = true;
      else if (a->line_index == b->line_index &&
               _CmdRank(a->cmd) > _CmdRank(b->cmd))
        swap = true;

      if (swap) {
        LyricArea *tmp = g_sys.sorted_lyrics[i];
        g_sys.sorted_lyrics[i] = g_sys.sorted_lyrics[j];
        g_sys.sorted_lyrics[j] = tmp;
      }
    }
  }

  // 3. --- 动态时长预测 (核心增强) ---
  for (int i = 0; i < g_sys.active_count; i++) {
    LyricArea *curr = g_sys.sorted_lyrics[i];

    // 0x14/0x15 已自带精准结束时间，跳过；其他 cmd 跳过
    if (curr->cmd != 0x12 && curr->cmd != 0x13)
      continue;

    bool found = false;

    // --- 0x13 翻译：优先继承同 Index 的 0x15 精准时长 ---
    // (0x15 解析时已存 duration = end_time - start_time)
    if (curr->cmd == 0x13) {
      for (int j = 0; j < g_sys.active_count; j++) {
        LyricArea *enh = g_sys.sorted_lyrics[j];
        if (enh->cmd == 0x15 && enh->line_index == curr->line_index) {
          curr->duration = enh->duration;
          found = true;
          break;
        }
      }
      if (found)
        continue;
    }

    // --- 0x12 原文：向下找下一个原文类 (0x12 或 0x15) 计算时长 ---
    for (int j = i + 1; j < g_sys.active_count; j++) {
      LyricArea *next = g_sys.sorted_lyrics[j];
      if (next->cmd == 0x12 || next->cmd == 0x15) {
        // 时长 = 下一行开始时间 - 本行开始时间
        if (next->start_time_ms > curr->start_time_ms) {
          curr->duration = next->start_time_ms - curr->start_time_ms;
          found = true;
        }
        break;
      }
    }

    // --- 0x13 翻译：找不到 0x15 时，向下找下一个 0x13 预测 ---
    if (!found && curr->cmd == 0x13) {
      for (int j = i + 1; j < g_sys.active_count; j++) {
        LyricArea *next = g_sys.sorted_lyrics[j];
        if (next->cmd == 0x13) {
          if (next->start_time_ms > curr->start_time_ms) {
            curr->duration = next->start_time_ms - curr->start_time_ms;
            found = true;
          }
          break;
        }
      }
    }

    // 如果后面没数据了，根据歌曲总时长 total_ms 预测
    if (!found && g_sys.total_ms > curr->start_time_ms) {
      uint32_t remaining = g_sys.total_ms - curr->start_time_ms;
      // 最后一两句通常不会持续到死，限制在 5s 内
      curr->duration = (remaining > 5000) ? 5000 : remaining;
    }
  }
}

// 0x10: 元数据解析 (TLV格式: Len + String)
void Lyric_OnMetadataReceived(uint8_t *payload, uint16_t len) {
  uint8_t p = 0;

  // Title
  uint8_t t_len = payload[p++];
  if (t_len >= MAX_METADATA_STR_LEN)
    t_len = MAX_METADATA_STR_LEN - 1;
  memcpy(g_sys.title, &payload[p], t_len);
  g_sys.title[t_len] = '\0';
  p += t_len;

  // Artist
  uint8_t r_len = payload[p++];
  if (r_len >= MAX_METADATA_STR_LEN)
    r_len = MAX_METADATA_STR_LEN - 1;
  memcpy(g_sys.artist, &payload[p], r_len);
  g_sys.artist[r_len] = '\0';
  p += r_len;

  // Album
  if (p < len) {
    uint8_t a_len = payload[p++];
    if (a_len >= MAX_METADATA_STR_LEN)
      a_len = MAX_METADATA_STR_LEN - 1;
    memcpy(g_sys.album, &payload[p], a_len);
    g_sys.album[a_len] = '\0';
  }

  LyricService_ClearPool();
}

/**
 * @brief 0x11: 时间同步（时间轴输入，转交 sync_algorithm）
 *
 * 包格式: [0]=is_playing [1..4]=remote_time_ms [5..8]=total_ms
 *         [9..12]=upstream_tick_ms (C# Environment.TickCount)
 *
 * 本函数只做三件事：长度校验 → 非对齐安全解包 → 原样转交算法。
 * 所有时间轴决策（首次锁相、暂停冻结、Seek/切歌硬复位、钟差与倍速学习、
 * 过期观测丢弃、看门狗自愈）都在 sync_algorithm.c 内部完成。
 */
void Lyric_OnSyncReceived(uint8_t *payload, uint16_t len) {
  if (len < 13)
    return;

  uint8_t is_playing = payload[0];
  uint32_t remote_time_ms;
  uint32_t total_ms;
  uint32_t upstream_tick_ms;
  // 非对齐安全解包：payload 为 DMA 字节流，偏移 1/5/9 非 4 对齐，
  // 直接强转 *(uint32_t*) 会触发 M4 UNALIGNED HardFault，必须 memcpy。
  memcpy(&remote_time_ms, &payload[1], 4);
  memcpy(&total_ms, &payload[5], 4);
  memcpy(&upstream_tick_ms, &payload[9], 4); // C# 单调毫秒 tick

  // 原始量镜像（诊断 / 歌词池时长预测用；时间轴权威在 sync_algorithm）
  g_sys.is_playing = is_playing;
  g_sys.total_ms = total_ms;
  g_sys.remote_time_ms = remote_time_ms;
  g_sys.upstream_tick_ms = upstream_tick_ms;

  // local_recv_tick 取"报文处理时刻"：协议分发在 UI_Manager_Tick 的 COM 消费里，
  // 与 Sync_GetTime() 同为主循环上下文 → 无 64 位锚点撕裂风险。
  Sync_OnPacketAt(is_playing, remote_time_ms, total_ms, upstream_tick_ms,
                  HAL_GetTick());
}

/**
 * @brief 0x12/13/14/15/16: 歌词内容处理 (核心池管理)
 *
 * 偏移量对齐：
 *  - 0x12/0x13 原文/翻译: [Index:2B][StartTime:4B][Text@6...]
 *  - 0x14 逐字歌词:       [Index:2B][StartTime:4B][WordCount@6][逐字@7...]
 *  - 0x15 增强原文:       [Index:2B][StartTime:4B][EndTime@6][Text@10...]
 *  - 0x16 纯文本:         [Text@0...]
 *
 * 池管理：
 *  1. 查重：Index + Cmd 联合判定，防止原文翻译互踢；
 *  2. 排序：Index 升序，同 Index 时 原文(12/14/15) > 翻译(13)；
 *  3. LRU：无空位时按 update_tick 淘汰最旧。
 */
void Lyric_OnContentReceived(uint8_t cmd, uint8_t *payload, uint16_t len) {
  // --- 1. 偏移量解析 (基于 payload[0] 是 Index) ---
  // 非对齐安全解包：payload[2] 非 4 对齐，强转解引用会触发 UNALIGNED。
  uint16_t index;
  uint32_t start_time;
  memcpy(&index, &payload[0], 2);
  memcpy(&start_time, &payload[2], 4);
  LyricArea *slot = NULL;

  // --- 2. 查找 Slot (查重) ---
  // 必须 line_index 和 cmd 均匹配才视为同一条，进行覆盖
  for (int i = 0; i < LYRIC_POOL_SIZE; i++) {
    if (g_sys.lyric_pool[i].is_valid &&
        g_sys.lyric_pool[i].line_index == index &&
        g_sys.lyric_pool[i].cmd == cmd) {
      slot = &g_sys.lyric_pool[i];
      break;
    }
  }

  // --- 3. 找空位或 LRU 替换 ---
  if (!slot) {
    uint32_t min_tick = 0xFFFFFFFF;
    int oldest_idx = 0;
    for (int i = 0; i < LYRIC_POOL_SIZE; i++) {
      if (!g_sys.lyric_pool[i].is_valid) {
        slot = &g_sys.lyric_pool[i];
        break;
      }
      if (g_sys.lyric_pool[i].update_tick < min_tick) {
        min_tick = g_sys.lyric_pool[i].update_tick;
        oldest_idx = i;
      }
    }
    if (!slot)
      slot = &g_sys.lyric_pool[oldest_idx];
  }

  // --- 4. 填充基础数据 ---
  slot->is_valid = 0; // 写入时锁定
  slot->cmd = cmd;
  slot->line_index = index;
  slot->start_time_ms = start_time;
  slot->update_tick = HAL_GetTick();
  uint32_t txt_pos = 0;

  // --- 5. 内容解析 (修正字符偏移) ---
  if (cmd == 0x14) {               // 逐字模式
    slot->word_count = payload[6]; // 对应 C# data[9]
    uint8_t *p_data = &payload[7]; // 对应 C# data[10]

    for (int i = 0; i < slot->word_count && i < MAX_WORDS_PER_LINE; i++) {
      // 非对齐安全解包：p_data 随逐字扫描前进，可能落在任意奇地址。
      uint16_t ts_end;
      memcpy(&ts_end, p_data, 2);
      slot->time_offsets[i] = ts_end;
      // 先存为结束时间戳偏移,这样最后一次得到duration.
      slot->duration = ts_end;
      uint8_t w_len = p_data[2];
      p_data += 3;

      slot->word_ptrs[i] = &slot->text[txt_pos];
      slot->word_lens[i] = w_len;

      if (txt_pos + w_len < LYRIC_TEXT_SIZE) {
        memcpy(&slot->text[txt_pos], p_data, w_len);
        txt_pos += w_len;
      }
      p_data += w_len;
    }

    if (txt_pos < LYRIC_TEXT_SIZE)
      slot->text[txt_pos] = '\0';

  } else if (cmd == 0x15) { // 增强原文行:
                            // [Index:2B][StartTime:4B][EndTime:4B][Text...]
    uint32_t end_time;
    memcpy(&end_time, &payload[6], 4); // payload[6] 非 4 对齐，必须 memcpy
    if (end_time > start_time)
      slot->duration = end_time - start_time;
    else
      slot->duration = 4000; // 兜底

    uint8_t txt_offset = 10;
    uint32_t txt_len = (len > txt_offset) ? (len - txt_offset) : 0;

    if (txt_len >= LYRIC_TEXT_SIZE)
      txt_len = LYRIC_TEXT_SIZE - 1;

    memcpy(slot->text, &payload[txt_offset], txt_len);
    slot->text[txt_len] = '\0';
    slot->word_count = 0;

  } else { // 0x12 (原文) 或 0x13 (翻译)
    // 修正：C# data[9] 开始是文本，对应 payload[6]
    uint8_t txt_offset = 6;
    uint32_t txt_len = (len > txt_offset) ? (len - txt_offset) : 0;

    if (txt_len >= LYRIC_TEXT_SIZE)
      txt_len = LYRIC_TEXT_SIZE - 1;

    memcpy(slot->text, &payload[txt_offset], txt_len);
    slot->text[txt_len] = '\0';

    // 普通行暂定 4s 持续时间
    slot->duration = 4000;
    slot->word_count = 0;
  }

  // --- 6. 激活 ---
  slot->is_valid = 1;

  // --- 7. 更新有序指针数组 ---
  _Lyric_UpdateSortedArray();
}