#include "com_manager.h"
#include <string.h>

static UART_HandleTypeDef *p_huart;
uint8_t rx_raw_buffer[RX_BUF_SIZE]; // DMA直接使用的原始缓冲区

volatile uint8_t cmd_ready = 0;

/**
 * @brief 初始化串口管理逻辑
 */
void COM_Init(UART_HandleTypeDef *huart) {
  p_huart = huart;

  // 1. 开启串口空闲中断
  __HAL_UART_ENABLE_IT(p_huart, UART_IT_IDLE);

  // 2. 启动 DMA 接收
  HAL_UART_Receive_DMA(p_huart, rx_raw_buffer, RX_BUF_SIZE);
}

/**
 * @brief 处理串口空闲中断回调
 * 应在 stm32f4xx_it.c 的 USARTx_IRQHandler 中被手动调用
 */
void COM_UART_IDLE_Callback(UART_HandleTypeDef *huart) {
  // 假设你之前定义了 p_huart 指向你的串口句柄
  if (huart->Instance == p_huart->Instance) {
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE)) {
      __HAL_UART_CLEAR_IDLEFLAG(huart);
      cmd_ready = 1; // 只是个提醒信号
    }
  }
}
