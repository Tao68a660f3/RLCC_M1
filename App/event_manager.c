#include "event_manager.h"
#include "ir_remote.h"
#include <stdio.h>

// 映射表定义
typedef struct {
  uint8_t cmd;
  void (*handler)(void);
} IR_Binding_t;

static void Slot_ShowSystemInfo(void) {
  printf("[Event] IR Key 0x46 -> [Slot] System says: Hello World!\r\n\r\n");
}

//  功能 Slot
static void Slot_PrankAction(void) {
  printf("Oops! Your CPU is pretending to be busy... Just kidding! 😜\r\n");
}

// 各种模式下功能表定义
// 正式功能用
// static const IR_Binding_t protocol_bindings[] = {
//     {0x45, UI_Manager_Mode_1},   // 双行模式
//     {0x46, UI_Manager_Mode_2},   // 单行+信息
//     {0x47, UI_Manager_Mode_3},   // 时钟模式
//     {0x16, UI_Manager_NextMode}, // 通用切换界面
//     {0x0d, Slot_ToggleMode},     // 用来切换系统模式
// };
// 整蛊模式下的表
static const IR_Binding_t text_bindings[] = {
    {0x45, Slot_PrankAction}, {0x46, Slot_ShowSystemInfo},
    // {0x0d, Slot_ToggleMode},  // 用来切换系统模式
};

// event管家处理
void Event_Dispatch_IR(uint8_t cmd) {
  const IR_Binding_t *target_table;
  uint8_t table_size;

  // 档位选择逻辑
  target_table = text_bindings;
  table_size = sizeof(text_bindings) / sizeof(IR_Binding_t);

  // 执行分发
  for (int i = 0; i < table_size; i++) {
    if (cmd == target_table[i].cmd) {
      target_table[i].handler();
      return;
    }
  }
  printf("Key 0x%02X: Nothing happens in this mode.\r\n", cmd);
}
