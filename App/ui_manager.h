#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "main.h"
#include "mem_pool.h"

typedef enum {
  UI_MODE_2_Line =
      0, // 模式1：双行歌词(SYS_MODE_PROTOCOL_MODE)或双行文本(SYS_MODE_TXT_MODE)
         // 字体：FONT_ASC_1608+FONT_GBK_1616S
  UI_MODE_1_LINE_MID_FONT, // 模式2：单行歌词(SYS_MODE_PROTOCOL_MODE)或单行文本(SYS_MODE_TXT_MODE)
                           // 字体：FONT_ASC_FONT_ASC_2010+FONT_GBK_1624M
  UI_MODE_1_LINE_BIG_FONT, // 模式3：单行歌词(SYS_MODE_PROTOCOL_MODE)或单行文本(SYS_MODE_TXT_MODE)
                           // 字体：FONT_ASC_FONT_ASC_2412+FONT_GBK_2432S
  UI_MODE_MUSIC_INFO, // 模式4：时间进度 + 标题轮换 +
                      // 单行歌词(仅SYS_MODE_PROTOCOL_MODE可以使用)
  UI_MODE_HOME_LIFE,  // 模式5：大字时间 + 温湿度（两种sys_mode均可使用）

} UI_Mode_t;

typedef enum {
  SYS_MODE_TXT_MODE = 0,
  SYS_MODE_PROTOCOL_MODE,
} Sys_Mode_t;

extern UI_Mode_t g_curr_ui_mode;
extern Sys_Mode_t g_curr_sys_mode;

void UI_Manager_OnMusicChanged(void);
void UI_Manager_Tick(void);

// ====== 模式切换接口（供 event_manager 等调用）======
void UI_Manager_SetMode_2Line(void);
void UI_Manager_SetMode_1LineMid(void);
void UI_Manager_SetMode_1LineBig(void);
void UI_Manager_SetMode_MusicInfo(void);
void UI_Manager_SetMode_HomeLife(void);
void UI_Manager_NextUIMode(void);

void UI_Manager_SetSysMode(Sys_Mode_t mode);
void UI_Manager_ToggleSysMode(void);

// 文本模式：COM 层拆出完整行后回调此函数，UI 层负责渲染到窗口
void UI_Manager_OnLineReceived_Double(const char *text);
void UI_Manager_OnLineReceived_Single(const char *text, CanvasColor color);

// 通用文本行入口：COM 层拆包后调用，内部按 g_curr_ui_mode 自动分发
void UI_Manager_OnTextLineReceived(const char *text);

// ====== 协议内文本模式（0x16 驱动，保持在 SYS_MODE_PROTOCOL_MODE 内）======
/** 查询当前是否处于协议内文本模式 */
uint8_t UI_Manager_IsProtocolTextMode(void);
/** 进入协议内文本模式（由 0x16 首位触发，幂等） */
void UI_Manager_EnterProtocolTextMode(void);
/** 退出协议内文本模式，恢复进入前的 UI 模式布局（由非 0x16 指令触发） */
void UI_Manager_ExitProtocolTextMode(void);
/** 0x16 纯文本数据入口：进入子模式并渲染文本 */
void UI_Manager_OnProtocolTextReceived(const uint8_t *payload, uint16_t len);

// ====== 息屏控制 ======
/** 切换息屏/亮屏 */
void UI_Manager_ToggleScreen(void);
/** 返回当前是否息屏 */
uint8_t UI_Manager_IsScreenOff(void);
/** 唤醒屏幕（息屏状态下按任意键时调用） */
void UI_Manager_WakeScreen(void);

#endif
