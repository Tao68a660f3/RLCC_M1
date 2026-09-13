#include "com_manager.h"
#include "main.h"
#include "protocol_parser.h"
#include "ui_manager.h"
#include <string.h>

static UART_HandleTypeDef *p_huart;
uint8_t rx_raw_buffer[RX_BUF_SIZE]; // DMA直接使用的原始缓冲区

volatile uint8_t cmd_ready = 0;

// 全链路延迟测试(0x1F/0xAF)时间戳（见 com_manager.h / latency_service.c）
volatile uint32_t g_uart_last_rx_cyccnt = 0;
volatile uint8_t g_uart_rx_ts_valid = 0;

// 消费者状态
static volatile uint16_t last_read_idx = 0;

/**
 * @brief 初始化串口管理逻辑
 *
 * 关键顺序：清理残留状态 → 启动 Circular DMA 接收 → 最后开启 IDLE 中断。
 * 这样对端（BLE 透传模块）即使先上电、在 STM32 应用初始化完成前就发出
 * "READY\r\n"，也能被环形缓冲区捕获；残留 ORE/IDLE 标志不会在 DMA 尚未
 * 就绪时提前触发错误或空闲中断。
 */
void COM_Init(UART_HandleTypeDef *huart) {
  p_huart = huart;

  // 1. 清理 USART 残留错误/IDLE 标志（读 SR 再读 DR 的硬件清除方式）
  __HAL_UART_CLEAR_OREFLAG(huart);   // 同时清除 FE/NE/ORE
  __HAL_UART_CLEAR_IDLEFLAG(huart);  // 清除 IDLE（同一机制，防御性再清一次）

  // 2. 清理 DMA2 Stream5 残留中断标志（Stream5 对应 HAL 的 *_1_5 组标志）
  if (huart->hdmarx != NULL) {
    __HAL_DMA_CLEAR_FLAG(huart->hdmarx,
                         DMA_FLAG_FEIF1_5 | DMA_FLAG_DMEIF1_5 |
                         DMA_FLAG_TEIF1_5 | DMA_FLAG_HTIF1_5 |
                         DMA_FLAG_TCIF1_5);
  }

  // 3. 启动 Circular DMA 接收（内部会再清一次 ORE，再使能 DMAR/EIE）
  HAL_UART_Receive_DMA(huart, rx_raw_buffer, RX_BUF_SIZE);

  // 4. 最后开启 IDLE 中断
  __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);

  // 消费指针复位（防御性，正常冷启动 .bss 已清零）
  last_read_idx = 0;
  cmd_ready = 0;
  g_uart_rx_ts_valid = 0; // 尚无有效 IDLE 打点
}

/**
 * @brief 处理串口空闲中断回调
 * 应在 stm32f4xx_it.c 的 USARTx_IRQHandler 中被手动调用
 *
 * 说明：DMA 为 Circular 模式，IDLE 只需清标志并置提醒信号，
 * 不能在这里停止/重启 DMA（否则会破坏环形缓冲的连续性）。
 */
void COM_UART_IDLE_Callback(UART_HandleTypeDef *huart) {
  if (p_huart == NULL) // 防止 IRQ 早于 COM_Init 触发
    return;
  if (huart->Instance == p_huart->Instance) {
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE)) {
      __HAL_UART_CLEAR_IDLEFLAG(huart);
      // 延迟测试时间戳：一帧收完的瞬间，中断里只存一个周期数，开销极小
      g_uart_last_rx_cyccnt = DWT->CYCCNT;
      g_uart_rx_ts_valid = 1;
      cmd_ready = 1; // 只是个提醒信号
    }
  }
}

/**
 * @brief UART 错误回调（覆盖 HAL 弱定义）
 *
 * DMA 模式下，任意 ORE/FE/NE/DMA 错误都会被 HAL 视为阻塞错误：HAL 会
 * 中止接收（RxState 回到 READY、清除 DMAR、中止 DMA 流）并调用本回调。
 * 若这里不恢复接收链路，RX DMA 将永久停止。此回调负责完整重启 RX。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance != USART1)
    return;

  // 1. 兜底：确保 DMA 流已停止（正常错误路径下 HAL 已中止，这里防漏）
  if (huart->hdmarx != NULL) {
    if (huart->hdmarx->State == HAL_DMA_STATE_BUSY) {
      HAL_DMA_Abort(huart->hdmarx);
    }
    // 2. 清理 DMA2 Stream5 残留中断标志
    __HAL_DMA_CLEAR_FLAG(huart->hdmarx,
                         DMA_FLAG_FEIF1_5 | DMA_FLAG_DMEIF1_5 |
                         DMA_FLAG_TEIF1_5 | DMA_FLAG_HTIF1_5 |
                         DMA_FLAG_TCIF1_5);
  }

  // 3. 清除 USART 残留错误/IDLE 标志，防止恢复后立即再次触发错误中断
  __HAL_UART_CLEAR_OREFLAG(huart);
  __HAL_UART_CLEAR_IDLEFLAG(huart);
  huart->ErrorCode = HAL_UART_ERROR_NONE;

  // 4. 兜底：确保 HAL 状态机处于可重启状态（正常错误路径已置 READY）
  huart->RxState = HAL_UART_STATE_READY;

  // 5. 环形缓冲从 0 重新开始，复位消费指针与提醒信号
  last_read_idx = 0;
  cmd_ready = 0;
  g_uart_rx_ts_valid = 0; // 重启接收后需等新的 IDLE 打点

  // 6. 重新启动 Circular DMA 接收，并重新开启 IDLE 中断
  if (HAL_UART_Receive_DMA(huart, rx_raw_buffer, RX_BUF_SIZE) == HAL_OK) {
    __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);
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
        // --- 抛给 UI 层按当前模式分发，DMA 后台继续存货 ---
        UI_Manager_OnTextLineReceived(line_cache);
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
        UI_Manager_OnTextLineReceived(line_cache);
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

/**
 * @brief 发送数据到串口（阻塞发送，短包场景使用）
 *        huart1 在 main.h 中有 extern，此处直接引用
 */
void COM_SendBytes(const uint8_t *data, uint16_t len) {
  extern UART_HandleTypeDef huart1;
  HAL_UART_Transmit(&huart1, data, len, HAL_MAX_DELAY);
}
