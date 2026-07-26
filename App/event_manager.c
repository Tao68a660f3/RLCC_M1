#include "event_manager.h"
#include "com_manager.h"
#include "ir_remote.h"
#include "ui_manager.h"
#include <stdio.h>

// ====== IR 按键码 (定义于 ir_remote.h) ======
// KEY_1~KEY_9, KEY_STAR, KEY_0, KEY_POUND
// KEY_UP, KEY_DOWN, KEY_LEFT, KEY_OK, KEY_RIGHT

// ====== 协议模式 — 播放控制 Slot ======
static void Slot_Playback_Next(void) {
  // 下一曲: AB A1 00 00
  uint8_t pkt[] = {0xAB, 0xA1, 0x00, 0x00};
  COM_SendBytes(pkt, 4);
}
static void Slot_Playback_Prev(void) {
  // 上一曲: AB A2 00 00
  uint8_t pkt[] = {0xAB, 0xA2, 0x00, 0x00};
  COM_SendBytes(pkt, 4);
}
static void Slot_Playback_PlayPause(void) {
  // 播放/暂停: AB A3 00 00
  uint8_t pkt[] = {0xAB, 0xA3, 0x00, 0x00};
  COM_SendBytes(pkt, 4);
}
static void Slot_Playback_Rewind(void) {
  // TODO: 快退 — 上位机协议尚未定义，暂不实现
}
static void Slot_Playback_FastForward(void) {
  // TODO: 快进 — 上位机协议尚未定义，暂不实现
}

// ====== 文本模式 — 向上位机请求信息 Slot ======
static void Slot_HostReq_1(void) {
  /* TODO */
  printf("request: sys_info\r\n");
}
static void Slot_HostReq_2(void) { /* TODO */ }
static void Slot_HostReq_3(void) { /* TODO */ }
static void Slot_HostReq_4(void) { /* TODO */ }
static void Slot_HostReq_5(void) { /* TODO */ }

// ====== 映射表定义 ======
typedef struct {
  uint8_t cmd;
  void (*handler)(void);
} IR_Binding_t;

// ====== 协议模式绑定表（21个按键全分配）======
static const IR_Binding_t protocol_bindings[] = {
    // 数字键
    {KEY_0, UI_Manager_ToggleScreen},      // 0 → 息屏/亮屏
    {KEY_1, UI_Manager_SetMode_2Line},     // 1
    {KEY_2, UI_Manager_SetMode_1LineMid},  // 2
    {KEY_3, UI_Manager_SetMode_1LineBig},  // 3
    {KEY_4, UI_Manager_SetMode_MusicInfo}, // 4
    {KEY_5, UI_Manager_SetMode_HomeLife},  // 5
    // 功能键
    {KEY_STAR, UI_Manager_NextUIMode},     // * → 循环切换UI模式
    {KEY_POUND, UI_Manager_ToggleSysMode}, // # → 切换系统模式
    // 方向键 → 播放控制
    {KEY_UP, Slot_Playback_Rewind},        // 上 → 快退
    {KEY_DOWN, Slot_Playback_FastForward}, // 下 → 快进
    {KEY_LEFT, Slot_Playback_Prev},        // 左 → 上一曲
    {KEY_RIGHT, Slot_Playback_Next},       // 右 → 下一曲
    {KEY_OK, Slot_Playback_PlayPause},     // OK → 播放/暂停
};
static const uint8_t protocol_bindings_cnt =
    sizeof(protocol_bindings) / sizeof(IR_Binding_t);

// ====== 文本模式绑定表 =======
static const IR_Binding_t text_bindings[] = {
    // 数字键
    {KEY_0, UI_Manager_ToggleScreen},     // 0 → 息屏/亮屏
    {KEY_1, UI_Manager_SetMode_2Line},    // 1
    {KEY_2, UI_Manager_SetMode_1LineMid}, // 2
    {KEY_3, UI_Manager_SetMode_1LineBig}, // 3
    {KEY_4, UI_Manager_SetMode_HomeLife}, // 4
    {KEY_5, UI_Manager_SetMode_HomeLife}, // 5
    // 功能键
    {KEY_STAR, UI_Manager_NextUIMode},     // * → 循环切换UI模式
    {KEY_POUND, UI_Manager_ToggleSysMode}, // # → 切换系统模式
    // 方向键 → 上位机请求
    {KEY_UP, Slot_HostReq_1},    // 上
    {KEY_DOWN, Slot_HostReq_2},  // 下
    {KEY_LEFT, Slot_HostReq_3},  // 左
    {KEY_RIGHT, Slot_HostReq_4}, // 右
    {KEY_OK, Slot_HostReq_5},    // OK
};
static const uint8_t text_bindings_cnt =
    sizeof(text_bindings) / sizeof(IR_Binding_t);

// event管家处理
void Event_Dispatch_IR(uint8_t cmd) {
  // 非屏幕切换键 → 息屏状态下按任意键先唤醒屏幕
  if (cmd != KEY_0) {
    UI_Manager_WakeScreen();
  }

  const IR_Binding_t *target_table;
  uint8_t table_size;

  // 根据当前系统模式选择绑定表
  if (g_curr_sys_mode == SYS_MODE_PROTOCOL_MODE) {
    target_table = protocol_bindings;
    table_size = protocol_bindings_cnt;
  } else {
    target_table = text_bindings;
    table_size = text_bindings_cnt;
  }

  // 执行分发
  for (int i = 0; i < table_size; i++) {
    if (cmd == target_table[i].cmd) {
      target_table[i].handler();
      return;
    }
  }
  // printf("Key 0x%02X: Nothing happens in this mode.\r\n", cmd);
}
