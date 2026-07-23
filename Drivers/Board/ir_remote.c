#include "ir_remote.h"
#include "main.h"
#include "system_utils.h"

IR_Data_t My_IR = {0};
static uint32_t last_tick = 0;
static uint8_t bit_count = 0;

void IR_Process_Callback(uint16_t GPIO_Pin) {
  if (GPIO_Pin != IR_IN_Pin)
    return;

  uint32_t current_tick = DWT->CYCCNT;
  uint32_t delta = (current_tick - last_tick) / (SystemCoreClock / 1000000);
  last_tick = current_tick;

  if (delta > 13000 && delta < 14000) {
    bit_count = 0;
    My_IR.raw_code = 0;
  } else if (delta > 1000 && delta < 1300) {
    My_IR.raw_code &= ~(1UL << bit_count);
    bit_count++;
  } else if (delta > 2100 && delta < 2400) {
    My_IR.raw_code |= (1UL << bit_count);
    bit_count++;
  }

  if (bit_count == 32) {
    My_IR.cmd = (uint8_t)(My_IR.raw_code >> 16); // 取命令码字节
    My_IR.ready = 1;
    bit_count = 0;
  }
}

// void IR_Process_Callback(uint16_t GPIO_Pin) {
//   if (GPIO_Pin != IR_IN_Pin)
//     return;

//   uint32_t current_tick = DWT->CYCCNT;
//   uint32_t delta = (current_tick - last_tick) / (SystemCoreClock / 1000000);
//   last_tick = current_tick;

//   // --- 识别正常的引导码 (9ms + 4.5ms = 13.5ms) ---
//   if (delta > 13000 && delta < 14000) {
//     bit_count = 0;
//     My_IR.raw_code = 0;
//   }
//   // --- 识别重复码 (9ms + 2.25ms = 11.25ms) ---
//   else if (delta > 11000 && delta < 12000) {
//     // 如果识别到重复码，说明按键没松开
//     // 我们不清除 cmd，直接把 ready 设为 1，让 main 函数再处理一次
//     My_IR.ready = 1;
//     bit_count = 0;
//   }
//   // --- 逻辑 0 (约 1.125ms) ---
//   else if (delta > 1000 && delta < 1300) {
//     My_IR.raw_code &= ~(1UL << bit_count);
//     bit_count++;
//   }
//   // --- 逻辑 1 (约 2.25ms) ---
//   else if (delta > 2100 && delta < 2400) {
//     My_IR.raw_code |= (1UL << bit_count);
//     bit_count++;
//   }

//   if (bit_count == 32) {
//     My_IR.cmd = (uint8_t)(My_IR.raw_code >> 16);
//     My_IR.ready = 1;
//     bit_count = 0;
//   }
// }