#include "aht30.h"

#define AHT30_CMD_MEASURE 0xAC
#define AHT30_CMD_SOFT_RESET 0xBA

#define AHT30_MEASURE_TIME_MS 80    // 传感器硬件转换耗时 80ms
#define AHT30_TIMEOUT_MS 200        // 最长容忍转换超时 200ms
#define AHT30_AUTO_INTERVAL_MS 2000 // 后台自动采样间隔 (2秒测一次)
#define AHT30_DATA_EXPIRE_MS                                                   \
  6000                    // 数据超时判定 (超过6秒没成功更新视为数据失效)
#define AHT30_MAX_RETRY 3 // 通信最大允许失败重试次数

/**
 * @brief 初始化 AHT30 句柄
 */
void AHT30_Init(AHT30_HandleTypeDef *haht) {
  haht->temperature = 0.0f;
  haht->humidity = 0.0f;
  haht->state = AHT30_STATE_RESET;
  haht->state_tick = HAL_GetTick();
  haht->last_success_tick = 0;
  haht->retry_count = 0;
  haht->is_valid = 0;
}

/**
 * @brief 内部无阻塞状态机核心处理
 */
static void AHT30_StateMachine(I2C_HandleTypeDef *hi2c,
                               AHT30_HandleTypeDef *haht) {
  uint32_t current_tick = HAL_GetTick();

  switch (haht->state) {
  /* ------------------ 1. 发起测量指令 ------------------ */
  case AHT30_STATE_RESET: {
    uint8_t cmd[3] = {AHT30_CMD_MEASURE, 0x33, 0x00};

    // 发送触发测量指令（非阻塞短超时 10ms）
    if (HAL_I2C_Master_Transmit(hi2c, AHT30_I2C_ADDR, cmd, 3, 10) == HAL_OK) {
      haht->state_tick = current_tick;
      haht->state = AHT30_STATE_MEASURING;
      haht->retry_count = 0;
    } else {
      haht->retry_count++;
      if (haht->retry_count >= AHT30_MAX_RETRY) {
        haht->state = AHT30_STATE_ERROR;
        haht->state_tick = current_tick;
      }
    }
    break;
  }

  /* ------------------ 2. 非阻塞等待与读取 ------------------ */
  case AHT30_STATE_MEASURING: {
    uint32_t elapsed_time = current_tick - haht->state_tick;

    // 没到 80ms 转换时间，直接退出，CPU 干别的去
    if (elapsed_time < AHT30_MEASURE_TIME_MS) {
      break;
    }

    // 尝试接收 6 字节数据
    uint8_t data[6] = {0};
    if (HAL_I2C_Master_Receive(hi2c, AHT30_I2C_ADDR, data, 6, 10) == HAL_OK) {
      // 检查 Bit 7 BUSY 标志位（为 0 表示测量完成）
      if ((data[0] & 0x80) == 0) {
        uint32_t raw_hum = ((uint32_t)data[1] << 12) |
                           ((uint32_t)data[2] << 4) | ((uint32_t)data[3] >> 4);
        uint32_t raw_temp = (((uint32_t)data[3] & 0x0F) << 16) |
                            ((uint32_t)data[4] << 8) | data[5];

        // 换算结果，更新本地数据缓存
        haht->humidity = ((float)raw_hum / 1048576.0f) * 100.0f;
        haht->temperature = ((float)raw_temp / 1048576.0f) * 200.0f - 50.0f;

        // 刷新健康状态
        haht->last_success_tick = current_tick;
        haht->is_valid = 1;

        haht->state = AHT30_STATE_IDLE; // 本次采样完成，进入空闲等待下一次触发
        haht->retry_count = 0;
        break;
      }
    }

    // 读取失败或超时保护
    if (elapsed_time > AHT30_TIMEOUT_MS) {
      haht->retry_count++;
      if (haht->retry_count >= AHT30_MAX_RETRY) {
        haht->state = AHT30_STATE_ERROR;
      } else {
        haht->state = AHT30_STATE_RESET; // 重新发起
      }
      haht->state_tick = current_tick;
    }
    break;
  }

  /* ------------------ 3. 硬件自愈重试 ------------------ */
  case AHT30_STATE_ERROR: {
    // 处于错误状态时，每隔 2 秒尝试发一次软复位
    if (current_tick - haht->state_tick >= 2000) {
      uint8_t reset_cmd = AHT30_CMD_SOFT_RESET;
      if (HAL_I2C_Master_Transmit(hi2c, AHT30_I2C_ADDR, &reset_cmd, 1, 10) ==
          HAL_OK) {
        haht->state = AHT30_STATE_IDLE;
        haht->retry_count = 0;
      }
      haht->state_tick = current_tick;
    }
    break;
  }

  case AHT30_STATE_IDLE:
    break;
  }
}

/**
 * @brief  后台静默自动测量主流程（放在主循环 while(1) 中高频轮询）
 */
void AHT30_Process_Background(I2C_HandleTypeDef *hi2c,
                              AHT30_HandleTypeDef *haht) {
  uint32_t current_tick = HAL_GetTick();

  // 1. 运行底层的状态机
  AHT30_StateMachine(hi2c, haht);

  // 2. 周期调度：如果传感器处于 IDLE，且距离上次成功采集超过了 2
  // 秒，自动重新触发
  if (haht->state == AHT30_STATE_IDLE) {
    if (current_tick - haht->last_success_tick >= AHT30_AUTO_INTERVAL_MS) {
      haht->state = AHT30_STATE_RESET; // 切到 RESET 状态自动触发新测量
    }
  }

  // 3. 数据过期自检：如果超过 6
  // 秒都没更新过，判定为数据失效（传感器断线或死锁）
  if (haht->is_valid &&
      (current_tick - haht->last_success_tick > AHT30_DATA_EXPIRE_MS)) {
    haht->is_valid = 0;
  }
}

/**
 * @brief  应用层安全读取函数
 * @return 1: 数据健康可用; 0: 传感器异常或数据已过期
 */
uint8_t AHT30_Get_SafeData(AHT30_HandleTypeDef *haht, float *temp, float *hum) {
  // 检查数据健康状态
  if (haht->is_valid == 0) {
    return 0; // 传感器有故障/掉线，返回 0 拒绝输出野数据
  }

  if (temp)
    *temp = haht->temperature;
  if (hum)
    *hum = haht->humidity;

  return 1; // 成功拿到数据
}

// 使用示例
// #include "main.h"
// #include "i2c.h"
// #include "aht30.h"

// AHT30_HandleTypeDef aht30;

// int main(void)
// {
//     HAL_Init();
//     SystemClock_Config();
//     MX_I2C1_Init();

//     AHT30_Init(&aht30);

//     uint32_t ui_timer = 0;

//     while (1)
//     {
//         /* 1. 后台静默运行（微秒级，只管在后台采数据和监控健康度） */
//         AHT30_Process_Background(&hi2c1, &aht30);

//         /* 2. 前台业务：比如屏幕每 500ms 刷一次，或者串口/网络上报 */
//         if (HAL_GetTick() - ui_timer >= 500)
//         {
//             ui_timer = HAL_GetTick();

//             float cur_temp, cur_hum;

//             // 只需要管拿数据，拿之前驱动会自动校验 is_valid
//             if (AHT30_Get_SafeData(&aht30, &cur_temp, &cur_hum))
//             {
//                 // 正常拿到数据，刷新 UI 界面
//                 printf("Temp: %.1f C, Hum: %.1f %%\r\n", cur_temp, cur_hum);
//             }
//             else
//             {
//                 // 传感器掉线或发生问题，屏幕画警告图标或者显示 "--.-"
//                 printf("Error: AHT30 Offline / Data Invalid!\r\n");
//             }
//         }

//         /* 3. 继续干别的任务 */
//         // Matrix_Screen_Refresh();
//     }
// }