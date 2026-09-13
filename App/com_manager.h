#ifndef __COM_MANAGER_H
#define __COM_MANAGER_H

#include "main.h"
#include <stdint.h>

#define RX_BUF_SIZE 4096

extern uint8_t rx_raw_buffer[RX_BUF_SIZE];
extern volatile uint8_t cmd_ready;

/* 全链路延迟测试(0x1F/0xAF)时间戳：
 * g_uart_last_rx_cyccnt = USART1 IDLE 中断里打点的 DWT 周期数（一帧收完的瞬间）；
 * g_uart_rx_ts_valid    = 打点是否有效（COM_Init / 错误恢复后为 0）。
 * 消费者：latency_service.c */
extern volatile uint32_t g_uart_last_rx_cyccnt;
extern volatile uint8_t g_uart_rx_ts_valid;

void COM_Init(UART_HandleTypeDef *huart);
void COM_UART_IDLE_Callback(UART_HandleTypeDef *huart);

// 消费者：在 main loop 中根据系统模式调用
void COM_Process_TextMode(void);
void COM_Process_ProtocolMode(void);

// 发送：将数据写入串口（阻塞发送，短包场景使用）
void COM_SendBytes(const uint8_t *data, uint16_t len);

#endif
