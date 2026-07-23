#ifndef __AHT30_H
#define __AHT30_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

#define AHT30_I2C_ADDR (0x38 << 1)

/* 状态机内部状态 */
typedef enum {
  AHT30_STATE_RESET = 0, // 重置 / 准备触发
  AHT30_STATE_IDLE,      // 空闲，等待下一次周期触发
  AHT30_STATE_MEASURING, // 正在测量中（非阻塞等待）
  AHT30_STATE_ERROR      // 硬件通信错误
} AHT30_StateTypeDef;

/* 设备控制句柄 */
typedef struct {
  float temperature; // 内部最新的温度
  float humidity;    // 内部最新的湿度

  AHT30_StateTypeDef state; // 状态机状态
  uint32_t state_tick;      // 状态切换时间戳
  uint8_t retry_count;      // 重试次数

  /* 供外部使用的健康与缓存数据 */
  uint32_t last_success_tick; // 上一次成功拿到有效数据的时间
  uint8_t is_valid;           // 数据有效标志 (1: 健康, 0: 故障/过期)
} AHT30_HandleTypeDef;

/* API 声明 */
void AHT30_Init(AHT30_HandleTypeDef *haht);

/* 核心：后台静默轮询函数，直接丢在 main 的 while(1) 里 */
void AHT30_Process_Background(I2C_HandleTypeDef *hi2c,
                              AHT30_HandleTypeDef *haht);

/* 应用层接口：前台需要时随时安全去拿数据 */
uint8_t AHT30_Get_SafeData(AHT30_HandleTypeDef *haht, float *temp, float *hum);

#endif /* __AHT30_H */