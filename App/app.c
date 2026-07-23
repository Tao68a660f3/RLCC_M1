#include "i2c.h"
#include "main.h"

#include "aht30.h"
#include "event_manager.h"
#include "ir_remote.h"
#include "w25q64.h"

#include <stdio.h>

#include "app.h"

uint32_t ui_timer = 0;
AHT30_HandleTypeDef aht30;

void App_Init() {
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

    float cur_temp, cur_hum;

    // 只需要管拿数据，拿之前驱动会自动校验 is_valid
    if (AHT30_Get_SafeData(&aht30, &cur_temp, &cur_hum)) {
      // 正常拿到数据，刷新 UI 界面
      printf("Temp: %.1f C, Hum: %.1f %%\r\n", cur_temp, cur_hum);
    } else {
      // 传感器掉线或发生问题，屏幕画警告图标或者显示 "--.-"
      printf("Error: AHT30 Offline / Data Invalid!\r\n");
    }
  }
}