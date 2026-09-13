/**
 * @file latency_service.h
 * @brief 全链路延迟测试（0x1F Ping / 0xAF Pong）
 *
 * 上行帧格式与上位机 PackageParser.TryParsePong 严格对称：
 *   AB AF 00 09 [1B Dev_ID] [4B C#_T1_ms] [4B STM32_Proc_us] [Check]
 */

#ifndef __LATENCY_SERVICE_H
#define __LATENCY_SERVICE_H

#include "main.h"

/* 0x1F 全链路延迟探测（上位机 MediaMonitor「测延迟」按钮下发）
 * payload: [0]=目标设备 ID  [1..4]=C#_T1_ms (uint32 小端)
 * 目标 ID 与本机 (DEVICE_ID_MAIN) 一致时回 0xAF Pong，否则静默丢弃。
 * 返回 1 = 已回包；0 = 非本机 / 帧长非法。 */
uint8_t Latency_OnPing(const uint8_t *payload, uint16_t len);

#endif
