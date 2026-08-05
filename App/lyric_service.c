#include "lyric_service.h"
#include "lyric_window_manager.h"
#include "stdbool.h"
#include <stdint.h>
#include <string.h>

SystemStatus g_sys;
uint8_t media_updated = 0;

void LyricService_Init(void) { memset(&g_sys, 0, sizeof(g_sys)); }

void LyricService_ClearPool(void) {
  // 清空歌词池
  memset(g_sys.lyric_pool, 0, sizeof(g_sys.lyric_pool));
  memset(g_sys.sorted_lyrics, 0, sizeof(g_sys.sorted_lyrics));
  g_sys.active_count = 0;

  // 重置时间轴
  g_sys.remote_time_ms = 0;
  g_sys.local_record_tick = HAL_GetTick();

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

// 0x11: 时间同步
void Lyric_OnSyncReceived(uint8_t *payload, uint16_t len) {
  g_sys.is_playing = payload[0];
  g_sys.remote_time_ms = *(uint32_t *)&payload[1];
  g_sys.total_ms = *(uint32_t *)&payload[5]; // 如果需要也可以存
  g_sys.local_record_tick = HAL_GetTick();
}

/**
 * @brief 0x12/13/14: 歌词内容处理 (核心池管理)
 * 修正点：
 * 1. 偏移量对齐：Index(2B)@0, Time(4B)@2, WordCount/Text@6
 * 2. 查重逻辑：Index + Cmd 联合判定，防止原文翻译互踢
 * 3. 排序逻辑：Index升序，同Index时 原文(12/14) > 翻译(13)
 */
void Lyric_OnContentReceived(uint8_t cmd, uint8_t *payload, uint16_t len) {
  // --- 1. 偏移量解析 (基于 payload[0] 是 Index) ---
  uint16_t index = *(uint16_t *)&payload[0];
  uint32_t start_time = *(uint32_t *)&payload[2];
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
      slot->time_offsets[i] = *(uint16_t *)p_data;
      // 先存为结束时间戳偏移,这样最后一次得到duration.
      slot->duration = *(uint16_t *)p_data;
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
    uint32_t end_time = *(uint32_t *)&payload[6];
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

  } else if (cmd == 0x16) { // 纯文本: 填默认值
    slot->line_index = 0;
    slot->start_time_ms = g_sys.remote_time_ms;
    slot->duration = 5000;
    slot->word_count = 0;

    uint32_t txt_len = (len > 0) ? len : 0;
    if (txt_len >= LYRIC_TEXT_SIZE)
      txt_len = LYRIC_TEXT_SIZE - 1;

    memcpy(slot->text, &payload[0], txt_len);
    slot->text[txt_len] = '\0';

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

  // --- 6. 测量宽度并激活 ---
  // _Lyric_MeasureVerbatim(slot);
  slot->is_valid = 1;

  // --- 7. 更新有序指针数组 ---
  _Lyric_UpdateSortedArray();
}
