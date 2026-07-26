#include "led_driver.h"
#include "main.h"
#include <string.h>

#define FLUSH_DIV 2

volatile uint8_t need_commit = 1;
volatile uint8_t flush_counter = 0;

/** 屏幕使能标志：1=亮屏，0=灭屏（中断中据此控制 OE） */
static uint8_t s_led_enabled = 1;

// 1. 显存定义：双缓冲
uint8_t frame_buffer[2][SCAN_ROWS][DRIVER_WIDTH] __attribute__((aligned(4)));
static volatile uint8_t read_idx = 0;
volatile uint8_t write_idx = 1;

// 2. 底层指针与句柄
static TIM_HandleTypeDef *p_htim;
static uint8_t *p_active_row;
static uint8_t row_idx = 0;
extern DMA_HandleTypeDef hdma_tim1_ch2;

// 换行逻辑：控制 A, B, C, D 引脚
void LED_UpdateRow(uint8_t row) {
  if (row == 15) {
    flush_counter++;
    if (need_commit) {
      if (flush_counter >= FLUSH_DIV) {
        LED_Commit();
        flush_counter = 0;
        need_commit = 0;
      }
    }
  }
  uint32_t set_mask = 0;
  uint32_t reset_mask = 0;

  // Row Bit 0 -> PA8
  if (row & 0x01)
    set_mask |= LED_LA_Pin;
  else
    reset_mask |= LED_LA_Pin;
  // Row Bit 1 -> PA11
  if (row & 0x02)
    set_mask |= LED_LB_Pin;
  else
    reset_mask |= LED_LB_Pin;
  // Row Bit 2 -> PA04 -> PA12
  if (row & 0x04)
    set_mask |= LED_LC_Pin;
  else
    reset_mask |= LED_LC_Pin;
  // Row Bit 3 -> PA08 -> PA15
  if (row & 0x08)
    set_mask |= LED_LD_Pin;
  else
    reset_mask |= LED_LD_Pin;

  // 一次性写入 BSRR 保证时序对齐
  LED_LA_GPIO_Port->BSRR = (reset_mask << 16) | set_mask;
}

// 5. 绘图接口：写到 write_idx 缓冲区
void LED_SetPixel(int16_t x, int16_t y, LED_Color color) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
    return;

  uint8_t row_idx = y % 16;
  uint8_t r = (color & 0x01);
  uint8_t g = (color & 0x02) >> 1;

  // 按区选择操作的位: Q0=bit0~1, Q1=bit2~3, Q2=bit4~5, Q3=bit6~7
  uint8_t q = (uint8_t)(y / 16);
  uint8_t r_bit = (uint8_t)(1 << (q * 2)); // R bit mask
  uint8_t g_bit = (uint8_t)(2 << (q * 2)); // G bit mask

  if (r)
    frame_buffer[write_idx][row_idx][x] &= ~r_bit;
  else
    frame_buffer[write_idx][row_idx][x] |= r_bit;
  if (g)
    frame_buffer[write_idx][row_idx][x] &= ~g_bit;
  else
    frame_buffer[write_idx][row_idx][x] |= g_bit;
}

void LED_Clear(void) { memset(frame_buffer, 0xff, sizeof(frame_buffer)); }

/** 清除指定物理区域在两个双缓冲中的像素 */
void LED_ClearAreaAllBuffers(int16_t x, int16_t y, uint16_t w, uint16_t h) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > WIDTH)
    w = WIDTH - x;
  if (y + h > HEIGHT)
    h = HEIGHT - y;
  if ((int16_t)w <= 0 || (int16_t)h <= 0)
    return;

  for (uint8_t buf = 0; buf < 2; buf++) {
    for (uint16_t row = y; row < y + h; row++) {
      uint8_t r = row % 16;
      uint8_t q = row / 16;
      uint8_t clr = (uint8_t)((1 << (q * 2)) | (2 << (q * 2)));
      uint8_t *p = &frame_buffer[buf][r][(uint16_t)x];

      uint16_t col = 0;
      // 4 字节对齐头
      while (col < w && ((uint32_t)&p[col] & 3)) {
        p[col++] |= clr;
      }
      // 32 位批量写
      uint32_t clr32 = (uint32_t)clr * 0x01010101;
      for (; col + 3 < w; col += 4) {
        *(uint32_t *)&p[col] |= clr32;
      }
      // 尾部零头
      for (; col < w; col++) {
        p[col] |= clr;
      }
    }
  }
}

// 6. 提交接口：交换缓冲区并清空下一帧
void LED_Commit(void) {
  uint8_t old_read_idx = read_idx;
  read_idx = write_idx;
  write_idx = old_read_idx;
}

void LED_IRQHandler_Logic(void) {
  // 消隐 (OE=1)
  LED_EN_GPIO_Port->BSRR = LED_EN_Pin;

  // 更新地址并切换行指针
  LED_UpdateRow(row_idx);
  row_idx = (row_idx + 1) % SCAN_ROWS;
  p_active_row = &frame_buffer[read_idx][row_idx][0];

  // 锁存当前行
  LED_LAT_GPIO_Port->BSRR = LED_LAT_Pin;
  __asm("nop");
  __asm("nop");
  LED_LAT_GPIO_Port->BSRR = (uint32_t)LED_LAT_Pin << 16;

  // 重装 DMA (必须手动重启，因为不是 Circular 模式)
  DMA2_Stream2->CR &= ~DMA_SxCR_EN;
  while (DMA2_Stream2->CR & DMA_SxCR_EN)
    ;

  DMA2->LIFCR = 0x3D << 16;

  DMA2_Stream2->M0AR = (uint32_t)&p_active_row[0];
  DMA2_Stream2->NDTR = DRIVER_WIDTH;
  DMA2_Stream2->CR |= DMA_SxCR_EN;

  // 【核心】为下一行重新配置硬件计数器
  p_htim->Instance->RCR = DRIVER_WIDTH - 1;
  p_htim->Instance->EGR |= TIM_EGR_UG; // 强行把 RCR 装入 shadow 寄存器
  p_htim->Instance->SR = 0;
  p_htim->Instance->CNT = 0;

  // 启动硬件
  for (volatile int i = 0; i < 20; i++)
    ; // 等待 ABCD 地址线稳定
  p_htim->Instance->CR1 |= TIM_CR1_CEN;
  if (s_led_enabled)
    LED_EN_GPIO_Port->BSRR = (uint32_t)LED_EN_Pin << 16; // OE=0 亮屏
  // else: OE 保持高电平，屏幕消隐
}

void LED_Init(TIM_HandleTypeDef *htim) {
  p_htim = htim;
  memset(frame_buffer, 0xff, sizeof(frame_buffer));

  row_idx = 0;
  LED_UpdateRow(row_idx);
  p_active_row = &frame_buffer[read_idx][row_idx][0];

  // --- 寄存器级硬件配置 ---
  // 1. 设置 RCR: 发送 WIDTH 个脉冲后停止 (RCR 从 N 减到 0 触发更新)
  p_htim->Instance->RCR = DRIVER_WIDTH - 1;

  // 2. 开启 OPM (单脉冲模式): 硬件数完脉冲后自动清除 CEN 位停止定时器
  p_htim->Instance->CR1 |= TIM_CR1_OPM;

  // 3. 开启 DMA 请求和主输出
  p_htim->Instance->DIER |= TIM_DIER_CC2DE;
  __HAL_TIM_MOE_ENABLE(p_htim);

  // 4. 初次启动 DMA (使用 Normal 模式)
  // 注意：在 IOC 中虽然是 Circular，但代码里 HAL_DMA_Start 会按 Normal 配置
  // 建议手动检查 hdma_tim1_ch2.Init.Mode = DMA_NORMAL;
  HAL_DMA_Start_IT(&hdma_tim1_ch2, (uint32_t)&p_active_row[0],
                   (uint32_t)&(GPIOA->ODR), DRIVER_WIDTH);

  // 5. 产生一次更新事件，将 RCR 值立即装载到硬件逻辑中
  p_htim->Instance->EGR |= TIM_EGR_UG;
  p_htim->Instance->SR = 0; // 清除 UG 产生的标志位
  p_htim->Instance->CNT = 0;

  // 6. 启动互补输出 PWM (PB0)
  HAL_TIMEx_PWMN_Start(p_htim, TIM_CHANNEL_2);

  // 初始保持消隐
  HAL_GPIO_WritePin(LED_EN_GPIO_Port, LED_EN_Pin, GPIO_PIN_SET);
}

void LED_SetScreenEnable(uint8_t enable) {
  s_led_enabled = enable;
  // if (enable) {
  //   // OE=0 → 亮屏
  //   LED_EN_GPIO_Port->BSRR = (uint32_t)LED_EN_Pin << 16;
  // } else {
  //   // OE=1 → 灭屏
  //   LED_EN_GPIO_Port->BSRR = LED_EN_Pin;
  // }
}
