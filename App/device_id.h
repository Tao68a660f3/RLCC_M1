/**
 * @file device_id.h
 * @brief 本机设备 ID
 */

#ifndef __DEVICE_ID_H
#define __DEVICE_ID_H

/* 本机设备 ID（1 字节，协议 0x1F/0xAF 全链路延迟测试使用）
 *
 * 0x01 = 串口 STM32 主设备，与上位机 MediaMonitor 的
 * ProtocolLatencyTester.TargetSerialMaster 保持一致。
 *
 * 多机 / 主从迭代：从机把此宏改成自己的 ID；
 * 收到"目标 ID 与本机不符"的 0x1F 帧必须静默丢弃（不回包、不留日志）。 */
#define DEVICE_ID_MAIN 0x01

#endif
