#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "main.h"

typedef enum {
  UI_MODE_LYRIC_2P = 0, // 模式1：双行歌词（默认）
  UI_MODE_MUSIC_INFO,   // 模式2：时间进度 + 标题轮换 + 单行歌词
  UI_MODE_HOME_LIFE,    // 模式3：大字时间 + 温湿度 + 单行歌词
  UI_MODE_MAX,
} UI_Mode_t;

typedef enum {
  SYS_MODE_TXT_MODE = 0,
  SYS_MODE_PROTOCOL_MODE,
  SYS_MODE_MAX,
} Sys_Mode_t;

extern UI_Mode_t g_curr_ui_mode;
extern Sys_Mode_t g_curr_sys_mode;

void UI_Manager_OnMusicChanged(void);
void UI_Manager_Tick(void);
void UI_Layout_Apply(UI_Mode_t mode);

void UI_Manager_Mode_1(void);
void UI_Manager_Mode_2(void);
void UI_Manager_Mode_3(void);
void UI_Manager_NextMode(void); // 给红外遥控调用

void UI_Manager_SysMode1(void); // 系统模式切换器，协议模式和普通文本模式
void UI_Manager_SysMode2(void);

// 文本模式：COM 层拆出完整行后回调此函数，UI 层负责渲染到窗口
void UI_Manager_OnLineReceived(const char *text);

#endif
