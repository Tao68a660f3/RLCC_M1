#include "event_manager.h"
#include "com_manager.h"
#include "ir_remote.h"
#include "ui_manager.h"
#include <stdio.h>

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
static void Slot_HostReq_1(void) { /* TODO */ }
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
    // 数字键 → 5种UI模式
    {0x45, UI_Manager_SetMode_2Line},     // 1
    {0x46, UI_Manager_SetMode_1LineMid},  // 2
    {0x47, UI_Manager_SetMode_1LineBig},  // 3
    {0x44, UI_Manager_SetMode_MusicInfo}, // 4
    {0x40, UI_Manager_SetMode_HomeLife},  // 5
    // 功能键
    {0x16, UI_Manager_NextUIMode},    // * → 循环切换UI模式
    {0x0d, UI_Manager_ToggleSysMode}, // # → 切换系统模式
    // 方向键 → 播放控制
    {0x18, Slot_Playback_Prev},        // 上 → 上一曲
    {0x52, Slot_Playback_Next},        // 下 → 下一曲
    {0x08, Slot_Playback_Rewind},      // 左 → 快退
    {0x5a, Slot_Playback_FastForward}, // 右 → 快进
    {0x1c, Slot_Playback_PlayPause},   // OK → 播放/暂停
};
static const uint8_t protocol_bindings_cnt =
    sizeof(protocol_bindings) / sizeof(IR_Binding_t);

// ====== 文本模式绑定表 =======
static const IR_Binding_t text_bindings[] = {
    // 数字键 → 5种UI模式
    {0x45, UI_Manager_SetMode_2Line},    // 1
    {0x46, UI_Manager_SetMode_1LineMid}, // 2
    {0x47, UI_Manager_SetMode_1LineBig}, // 3
    {0x44, UI_Manager_SetMode_HomeLife}, // 4
    {0x40, UI_Manager_SetMode_HomeLife}, // 5
    // 功能键
    {0x16, UI_Manager_NextUIMode},    // * → 循环切换UI模式
    {0x0d, UI_Manager_ToggleSysMode}, // # → 切换系统模式
    // 方向键 → 上位机请求
    {0x18, Slot_HostReq_1}, // 上
    {0x52, Slot_HostReq_2}, // 下
    {0x08, Slot_HostReq_3}, // 左
    {0x5a, Slot_HostReq_4}, // 右
    {0x1c, Slot_HostReq_5}, // OK
};
static const uint8_t text_bindings_cnt =
    sizeof(text_bindings) / sizeof(IR_Binding_t);

// event管家处理
void Event_Dispatch_IR(uint8_t cmd) {
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
  printf("Key 0x%02X: Nothing happens in this mode.\r\n", cmd);
}
