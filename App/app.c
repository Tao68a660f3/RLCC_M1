#include "i2c.h"
#include "main.h"
#include "rtc.h"


#include "aht30.h"
#include "bsp_rtc.h"
#include "event_manager.h"
#include "ir_remote.h"
#include "w25q64.h"

#include <stdio.h>

#include "app.h"

uint32_t ui_timer = 0;
AHT30_HandleTypeDef aht30;
RTC_DateTimeTypeDef current_dt;

void App_Init() {
  RTC_App_Init(&hrtc);
  // W25Q64_Init(&hspi1);
  AHT30_Init(&aht30);
}

static void IR_Control() {
  if (My_IR.ready) {
    Event_Dispatch_IR(My_IR.cmd); // 交给管家处理
    My_IR.ready = 0;
  }
}

void App_Loop() {
  /* 1. 后台静默运行（微秒级，只管在后台采数据和监控健康度） */
  IR_Control();
  AHT30_Process_Background(&hi2c1, &aht30);

  /* 2. 前台业务：比如屏幕每 500ms 刷一次，或者串口/网络上报 */
  if (HAL_GetTick() - ui_timer >= 3000) {
    ui_timer = HAL_GetTick();

    // 获取最新时间
    RTC_GetDateTime(&hrtc, &current_dt);

    // 获取最新温湿度
    float temp = 0.0f, hum = 0.0f;
    uint8_t aht_ok = AHT30_Get_SafeData(&aht30, &temp, &hum);

    // 格式化输出/刷屏
    printf("[%04d-%02d-%02d %02d:%02d:%02d] ", current_dt.year,
           current_dt.month, current_dt.day, current_dt.hours,
           current_dt.minutes, current_dt.seconds);

    if (aht_ok) {
      printf("Temp: %.1f C | Hum: %.1f %%\r\n", temp, hum);
    } else {
      printf("Temp: --.- C | Hum: --.- %%\r\n");
    }
  }
}