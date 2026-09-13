/**
 * @file lyric_service.h
 * @brief 歌词服务：协议解析 + 时间轴输入转发 + 歌词池管理
 *
 * 解析 C# 端下发的歌词协议包（0x10~0x15），把结果写入 g_sys：
 *  - 元数据 (title/artist/album)
 *  - 时间轴输入：0x11 字段原样转交 sync_algorithm (Sync_OnPacketAt)
 *  - 歌词池 (lyric_pool / sorted_lyrics)
 *
 * 时间轴状态与播放位置的唯一权威是 sync_algorithm
 * （由 lyric_window_manager.c 每帧采样 Sync_GetTime()）；
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

  // 时间轴输入镜像（0x11 原始量，供诊断 / 歌词池时长预测使用；
  // 时间轴状态与播放位置的唯一权威是 sync_algorithm，见 Sync_GetTime()）
  uint32_t remote_time_ms;   // 最近同步包中的播放进度 (ms)
  uint32_t total_ms;         // 歌曲总时长 (ms)
  uint32_t upstream_tick_ms; // C# 发包时刻的 Environment.TickCount
  uint8_t is_playing;        // 最近同步包中的播放状态

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
void LyricService_Init(void); // 内部调用 Sync_Init() 初始化时间轴模块
/* 清空歌词池（切歌时重置歌词相关状态）。
 * 注意：绝不复位 sync_algorithm —— 新歌首包会被算法自身的硬复位
 * (|误差| ≥ SYNC_HARD_RESET_FAST_MS) 直接接管，复位反而会清空已建立的
 * epoch 基线 / 钟差 / 倍速学习。详见 lyric_service.c 内注释。 */
void LyricService_ClearPool(void);
void Lyric_OnMetadataReceived(uint8_t *payload, uint16_t len);
/* 0x11 时间同步包：解析后原样转交 sync_algorithm (Sync_OnPacketAt) */
void Lyric_OnSyncReceived(uint8_t *payload, uint16_t len);
void Lyric_OnContentReceived(uint8_t cmd, uint8_t *payload, uint16_t len);
void RTC_OnTimeSyncReceived(uint8_t *payload, uint16_t len);

#endif