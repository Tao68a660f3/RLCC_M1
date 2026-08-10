/**
 * @file lyric_window_manager.h
 * @brief 歌词窗口管理接口：播放时间轴读取 + 歌词渲染调度
 *
 * 时间轴语义见 lyric_window_manager.c：s_pi_time 为全局播放时间轴，
 * 其生命周期由 _UpdateSmoothTime 边界条件（未同步/暂停/变速/Seek）
 * 管理，UI 模式切换不清零。
 */

#ifndef __LYRIC_WINDOW_MANAGER_H
#define __LYRIC_WINDOW_MANAGER_H

#include "lyric_service.h"
#include "window_manager.h"
#include <stdint.h>

#define MAX_LYRIC_LINES 4
#define HEAD_RATE 1000 // 进度映射：头部 10% 视作 0%
#define TAIL_RATE 2000 // 进度映射：尾部 20% 视作 100%

// 渲染配置
typedef struct {
  uint8_t win_idx;          // 绑定的物理窗口索引
  LyricArea *bound_area;    // 当前绑定的歌词数据源地址
  uint16_t last_line_index; // 行号缓存
  uint8_t last_cmd;         // 指令号缓存
  CanvasColor color_base;   // 底色
  CanvasColor color_high;   // 高亮色

  uint16_t offset_x; // 逻辑位置偏移量
  uint16_t offset_y; // 逻辑位置偏移量

  uint8_t is_occupied; // 标记该物理窗口本帧是否在岗
} LyricWinConfig;

extern uint16_t g_lyric_progress;   // 全局同步进度
extern uint16_t used_lyric_lines;   // 使用的歌词行数
extern uint16_t lyric_total_height; // 所有歌词窗口竖直总高度
extern LyricWinConfig l_win_cfg[MAX_LYRIC_LINES];

void LyricWM_Init(uint8_t line_count);
void LyricWM_Reset(void);
uint32_t Get_Current_PlayTime(void);
/** 每帧推进全局播放时间轴（PI 平滑外推）。
 *  由 UI_Manager_Tick 每帧统一调用一次，与渲染路径解耦：
 *  即使 need_commit 短路 / 协议内文本子模式 / 息屏，时间轴也照常推进。
 */
void LyricWM_UpdatePlayTime(void);
void LyricWM_RenderMgr(void);
void LyricWM_Process(void);

#endif