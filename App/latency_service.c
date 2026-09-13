/**
 * @file latency_service.c
 * @brief 全链路延迟测试（0x1F Ping / 0xAF Pong）—— 移植自 MediaMonitor readme
 *
 * 上位机按固定间隔（默认 100ms）下发：
 *   AA 1F 00 05 [1B 目标ID] [4B C#_T1_ms(小端)] [XOR]
 * 本机（DEVICE_ID_MAIN）匹配目标 ID 后原样回传 T1，并附上自己
 * "串口收完 → 组包完成"的微秒耗时 proc_us：
 *   AB AF 00 09 [1B Dev_ID] [4B C#_T1_ms 原样] [4B STM32_Proc_us(小端)] [XOR]
 *
 * 上位机结算：RTT = T2 - T1；OneWay = (RTT - proc_us/1000) / 2
 *   - 减去 proc_us：剔除 STM32 主循环排队 / 组包耗时
 *   - 除以 2      ：单向 ≈ 往返一半，消除两端晶振不同步与上下行不对称
 *
 * 测得的 Base（窗口内最小单向延迟）用于手工填写上位机「同步偏移(ms)」：
 * sync_algorithm 的相位环在误差 ≤ SYNC_PHASE_SLEW_MIN_MS(20ms) 时不动相位，
 * 因此"链路固定单向延迟"这份常数只能由 A 端补偿，不会自己消失。
 *
 * 时间戳说明：
 *   t_rx = USART1 IDLE 中断里打点的 DWT->CYCCNT（一帧收完的瞬间，见 com_manager.c）
 *   t_tx = 本函数组包时刻的 DWT->CYCCNT
 *   proc_us = (t_tx - t_rx) / SystemCoreClock × 1e6（84MHz）
 *   差值用无符号减法，DWT 计数回绕（84MHz 约 51.1s）时结果依然正确。
 *   注意：proc_us 只统计"收完 → 组包完成"，不含 UART 发送耗时。
 */

#include "latency_service.h"
#include "com_manager.h"
#include "device_id.h"
#include <string.h>

#define LATENCY_CMD_PONG 0xAF      // 上行：延迟回包
#define LATENCY_PING_PAYLOAD_LEN 5 // 1B 目标ID + 4B T1
#define LATENCY_PONG_PAYLOAD_LEN 9 // 1B DevID + 4B T1 + 4B proc_us
#define LATENCY_PONG_FRAME_LEN 14  // 4B 帧头/长度 + 9B 载荷 + 1B 校验

/* proc_us 合理性上限：超过 1s 说明 IDLE 打点与本次组包不属于同一帧
 * （例如多帧挤在同一突发里，或开机至今从未发生过 IDLE），
 * 此时填 0 —— 上位机打"未填写处理耗时"告警，测量结果仍可用。 */
#define LATENCY_PROC_US_MAX 1000000u

uint8_t Latency_OnPing(const uint8_t *payload, uint16_t len) {
  // 长度防御 + 主从过滤：目标 ID 不是本机 → 静默丢弃
  if (len != LATENCY_PING_PAYLOAD_LEN || payload[0] != DEVICE_ID_MAIN)
    return 0;

  // 非对齐安全解包：payload 为 DMA 字节流，偏移 1 非 4 对齐，必须 memcpy
  uint32_t t1_ms;
  memcpy(&t1_ms, &payload[1], 4);

  // 组包时刻；proc_us 在发送前算完，不把 UART 发送耗时算进去
  uint32_t t_tx_cyc = DWT->CYCCNT;
  uint32_t proc_us = 0;
  if (g_uart_rx_ts_valid) {
    uint32_t cycles = t_tx_cyc - g_uart_last_rx_cyccnt; // 无符号减法，抗回绕
    proc_us = (uint32_t)((uint64_t)cycles * 1000000ULL / SystemCoreClock);
    if (proc_us > LATENCY_PROC_US_MAX)
      proc_us = 0;
  }

  uint8_t frame[LATENCY_PONG_FRAME_LEN];
  frame[0] = 0xAB;                     // Head
  frame[1] = LATENCY_CMD_PONG;         // Cmd
  frame[2] = 0x00;                     // LenH
  frame[3] = LATENCY_PONG_PAYLOAD_LEN; // LenL
  frame[4] = DEVICE_ID_MAIN;           // Dev_ID
  memcpy(&frame[5], &t1_ms, 4);        // C#_T1_ms 原样透传（小端）
  memcpy(&frame[9], &proc_us, 4);      // STM32_Proc_us（小端）

  uint8_t check = 0;
  for (uint8_t i = 0; i < LATENCY_PONG_FRAME_LEN - 1; i++)
    check ^= frame[i]; // 全帧异或（不含校验位本身）
  frame[LATENCY_PONG_FRAME_LEN - 1] = check;

  COM_SendBytes(frame, LATENCY_PONG_FRAME_LEN); // 14B@115200 阻塞发送 ≈ 1.2ms
  return 1;
}
