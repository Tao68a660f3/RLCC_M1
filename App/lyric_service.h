#ifndef __LYRIC_SERVICE_H
#define __LYRIC_SERVICE_H

#include "main.h"
#include <stdint.h>

#define MAX_METADATA_STR_LEN 128
#define LYRIC_POOL_SIZE 4     // 环形歌词池大小
#define MAX_WORDS_PER_LINE 64 // 逐字词数上限
#define LYRIC_TEXT_SIZE 256   // 单行文本缓冲区

// 逐字歌词结构
typedef struct {
  uint8_t cmd;            // 保存原始指令号 (0x12, 0x13, 0x14)
  uint16_t line_index;    // 协议中的 Index，用于排序和匹配
  uint32_t start_time_ms; // 开始时间
  uint32_t duration;      // 持续时间

  char text[LYRIC_TEXT_SIZE];
  char *word_ptrs[MAX_WORDS_PER_LINE];
  uint16_t time_offsets[MAX_WORDS_PER_LINE];
  uint8_t word_lens[MAX_WORDS_PER_LINE];   // 字符串层面的逐字区段长度
  uint8_t word_widths[MAX_WORDS_PER_LINE]; // 渲染后的图形层面的逐字区段长度
  uint8_t word_count;

  uint32_t update_tick; // 用于池管理 (LRU)
  uint8_t is_valid;     // 是否有效
} LyricArea;

// 系统全局状态
typedef struct {
  // 元数据
  char title[MAX_METADATA_STR_LEN];
  char artist[MAX_METADATA_STR_LEN];
  char album[MAX_METADATA_STR_LEN];

  // 时间轴
  uint32_t remote_time_ms;
  uint32_t total_ms;
  uint32_t local_record_tick;
  uint8_t is_playing;

  // 歌词池
  uint32_t active_count;
  LyricArea lyric_pool[LYRIC_POOL_SIZE];
  LyricArea *sorted_lyrics[LYRIC_POOL_SIZE];

  // RTC 暂存
  uint8_t rtc_buf[7];
  uint8_t rtc_dirty;
} SystemStatus;

extern SystemStatus g_sys;
extern uint8_t media_updated;

// 接口函数
void LyricService_Init(void);
void Lyric_OnMetadataReceived(uint8_t *payload, uint8_t len);
void Lyric_OnSyncReceived(uint8_t *payload, uint8_t len);
void Lyric_OnContentReceived(uint8_t cmd, uint8_t *payload, uint8_t len);
void RTC_OnTimeSyncReceived(uint8_t *payload, uint8_t len);

#endif