#include "com_manager.h"
#include "main.h"
#include "protocol_parser.h"
#include "ui_manager.h"
#include <string.h>

static UART_HandleTypeDef *p_huart;
uint8_t rx_raw_buffer[RX_BUF_SIZE]; // DMA直接使用的原始缓冲区

volatile uint8_t cmd_ready = 0;

// 消费者状态
static volatile uint16_t last_read_idx = 0;

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

/**
 * @brief 文本模式消费者：从环形缓冲区拆出完整行，抛给 UI 层
 *
 * DMA 后台往 rx_raw_buffer 写，本函数在主循环中轮询消费。
 * 遇到 '\n' / '\r' / '\0' 则切出一条完整行，回调 UI_Manager_OnLineReceived。
 */
void COM_Process_TextMode(void) {
#define LINE_BUF_SIZE 256

  // huart1.hdmarx 在 main.h 中有 extern，直接用
  extern UART_HandleTypeDef huart1;

  // 1. 计算当前的 DMA 写入位置
  uint16_t dma_write_ptr = RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart1.hdmarx);

  // 2. 行缓存（static 保持跨调用状态）
  static char line_cache[LINE_BUF_SIZE];
  static uint8_t cache_idx = 0;

  // 3. 只要读指针 != 写指针，就有新数据
  while (last_read_idx != dma_write_ptr) {
    uint8_t c = rx_raw_buffer[last_read_idx];

    if (c == '\n' || c == '\r' || c == '\0') {
      if (cache_idx > 0) {
        line_cache[cache_idx] = '\0';
        // --- 耗时操作：抛给 UI 层处理，DMA 后台继续存货 ---
        UI_Manager_OnLineReceived(line_cache);
        cache_idx = 0;
      }
      // 跳过 CR/LF 后面可能跟着的配对字符（如 \r\n 或 \n\r）
      // 避免空行被回调（cache_idx==0 时不会触发）
    } else {
      if (cache_idx < LINE_BUF_SIZE - 1) {
        line_cache[cache_idx++] = (char)c;
      } else {
        // cache 满了，强制切行，避免整行数据永久丢失
        line_cache[LINE_BUF_SIZE - 1] = '\0';
        UI_Manager_OnLineReceived(line_cache);
        cache_idx = 0;
        // 这个字符还没存进去，重试一次
        line_cache[cache_idx++] = (char)c;
      }
    }

    // 4. 读指针前移，环形回绕
    last_read_idx = (last_read_idx + 1) % RX_BUF_SIZE;

    // 5. 如果在处理过程中又有新数据进来，重新获取写指针
    if (last_read_idx == dma_write_ptr) {
      dma_write_ptr = RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart1.hdmarx);
    }
  }

  cmd_ready = 0;

#undef LINE_BUF_SIZE
}

/**
 * @brief 协议模式消费者：从环形缓冲区喂给协议状态机
 */
void COM_Process_ProtocolMode(void) {
  extern UART_HandleTypeDef huart1;

  uint16_t dma_write_ptr = RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart1.hdmarx);
  static ProtocolPacket packet_cache;

  while (last_read_idx != dma_write_ptr) {
    uint8_t c = rx_raw_buffer[last_read_idx];

    // 喂给协议状态机
    if (Protocol_FeedByte(c, &packet_cache)) {
      // 解析到一个完整包，立即分发
      Protocol_Dispatcher(&packet_cache);
    }

    // 指针后移
    last_read_idx = (last_read_idx + 1) % RX_BUF_SIZE;

    // 防止处理期间有新数据进来
    if (last_read_idx == dma_write_ptr) {
      dma_write_ptr = RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart1.hdmarx);
    }
  }

  cmd_ready = 0;
}