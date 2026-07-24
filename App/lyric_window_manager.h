#ifndef __LYRIC_WINDOW_MANAGER_H
#define __LYRIC_WINDOW_MANAGER_H

#include "lyric_service.h"
#include "window_manager.h"
#include <stdint.h>

#define MAX_LYRIC_LINES 2
#define HEAD_RATE 1000
#define TAIL_RATE 2000

// 渲染配置
typedef struct {
  uint8_t win_idx;          // 绑定的物理窗口索引
  LyricArea *bound_area;    // 【新增】当前绑定的数据源地址
  uint16_t last_line_index; // 行号缓存
  uint8_t last_cmd;         // 指令号缓存
  CanvasColor color_base;   // 底色
  CanvasColor color_high;   // 高亮色

  uint16_t offset_x; // 逻辑位置偏移量
  uint16_t offset_y; // 逻辑位置偏移量

  uint8_t is_occupied; // 标记该物理窗口本帧是否在岗
} LyricWinConfig;

extern uint16_t g_lyric_progress; // 全局同步进度
extern uint16_t used_lyric_lines; // 使用的歌词行数
extern LyricWinConfig l_win_cfg[MAX_LYRIC_LINES];

void LyricWM_Init(void);
void LyricWM_Reset(void);
uint32_t Get_Current_PlayTime(void);
void LyricWM_RenderMgr(void);
void LyricWM_Process(void);

#endif