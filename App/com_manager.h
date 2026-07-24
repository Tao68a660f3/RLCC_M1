#ifndef __COM_MANAGER_H
#define __COM_MANAGER_H

#include "main.h"
#include <stdint.h>

#define RX_BUF_SIZE 2048

extern uint8_t rx_raw_buffer[RX_BUF_SIZE];
extern volatile uint8_t cmd_ready;

void COM_Init(UART_HandleTypeDef *huart);
void COM_UART_IDLE_Callback(UART_HandleTypeDef *huart);

// 消费者：在 main loop 中根据系统模式调用
void COM_Process_TextMode(void);
void COM_Process_ProtocolMode(void);

#endif
