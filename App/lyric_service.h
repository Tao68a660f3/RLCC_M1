/**
 * @file lyric_service.h
 * @brief 歌词服务：协议解析 + 全局时间轴锚点维护 + 歌词池管理
 *
 * 解析 C# 端下发的歌词协议包（0x10~0x15），把结果写入 g_sys：
 *  - 元数据 (title/artist/album)
 *  - 时间轴锚点 (base_remote_time / clock_offset / playback_speed 等)
 *  - 歌词池 (lyric_pool / sorted_lyrics)
 *
 * 时间轴锚点供 lyric_window_manager.c 外推为平滑播放时间；
 * 歌词池供其渲染调度使用。
 */

#ifndef __LYRIC_SERVICE_H
#define __LYRIC_SERVICE_H

#include "main.h"
#include <stdint.h>

#define MAX_METADATA_STR_LEN 256 // 元数据字符串上限
#define LYRIC_POOL_SIZE 4        // 环形歌词池大小
#define MAX_WORDS_PER_LINE 128   // 逐字词数上限
#define LYRIC_TEXT_SIZE 512      // 单行文本缓冲区

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
  uint32_t remote_time_ms;   // 最近同步包中的播放进度 (ms)
  uint32_t total_ms;         // 歌曲总时长
  uint32_t upstream_tick_ms; // C# 发包时刻的 Environment.TickCount32
  int32_t clock_offset_ms;   // C#时钟 - STM32时钟 偏差 (低通滤波)
  uint32_t base_remote_time; // 基准包进度锚点 (与外推共用，正常播放绝不覆盖)
  uint32_t base_remote_tick; // 基准包 C# tick 锚点
  uint32_t local_send_tick;  // 基准包映射到 STM32 本地 tick 锚点
  int32_t
      best_obs_latency_ms; // 运行途中最优观测延迟
                           // (obs=upstream_tick-local_rx 的历史最大值；
                           //  网络越好 obs 越大；
                           //  best_obs_valid==false 时数值无效，不得参与判断)
  uint16_t playback_speed; // 播放倍速 (Q8，256=1.0x，范围[128,768])
  uint8_t playback_speed_changed; // 变速事件标志 (1 帧有效，供渲染端感知)
  uint8_t clock_synced;           // 是否已完成首次时钟同步
                                  // (播放时间锚点是否建立)
  uint8_t best_obs_valid;         // 是否已获得过有效的历史最佳网络观测
                                  // (与 clock_synced 概念独立；切歌时复位)
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
void LyricService_ClearPool(void); // 清空歌词池（切歌时重置所有状态）
void Lyric_OnMetadataReceived(uint8_t *payload, uint16_t len);
void Lyric_OnSyncReceived(uint8_t *payload, uint16_t len);
void Lyric_OnContentReceived(uint8_t cmd, uint8_t *payload, uint16_t len);
void RTC_OnTimeSyncReceived(uint8_t *payload, uint16_t len);

#endif