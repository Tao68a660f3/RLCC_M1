
#include "i2c.h"
#include "main.h"
#include "rtc.h"
#include "spi.h"
#include "tim.h"

#include "aht30.h"
#include "bsp_rtc.h"
#include "event_manager.h"
#include "flash_font.h"
#include "ir_remote.h"
#include "led_display.h"
#include "led_driver.h"
#include "w25q64.h"
#include "window_manager.h"

#include <stdio.h>

#include "app.h"

/* Flash 字库地址（来自 font_offsets.txt） */
#define TEST_ASC_ADDR 0x010000 /* ASC2410 */
#define TEST_GBK_ADDR 0x378000 /* GBK2432S */

uint32_t ui_timer = 0;
AHT30_HandleTypeDef aht30;
RTC_DateTimeTypeDef current_dt;

extern uint8_t need_commit;

static void IR_Control();

static uint8_t surpriseMsg[] = {0xCB, 0xAB, 0xC9, 0xAB, 0xB8, 0xE8, 0xB4, 0xCA,
                                0xCF, 0xD4, 0xCA, 0xBE, 0xC6, 0xF7, 0x00};

void App_Init() {
  RTC_App_Init(&hrtc);
  AHT30_Init(&aht30);

  W25Q64_Init(&hspi1);
  Font_Flash_Init();

  /* 注册并选中 ASC1608 和 GBK1616H */
  int asc_id = FlashASC_Register(TEST_ASC_ADDR);
  int gbk_id = FlashGBK_Register(TEST_GBK_ADDR, 24, 32, 96);
  if (asc_id >= 0)
    FlashASC_Select(asc_id);
  if (gbk_id >= 0)
    FlashGBK_Select(gbk_id);

  LED_Init(&htim1);
  WindowManager_Init();

  /* 窗口0：全屏画布，显示测试文本 */
  Window_Config(0, 0, 0, 192, 32);
  Window_FillText(0, surpriseMsg, C_RED, CANVAS_R);
  Window_SetAlignment(0, ALIGN_LEFT);
  window_list[0].scroll_divider = 1;
  window_list[0].scroll_step = -1;
}

void App_Loop() {
  /* 1. 后台静默运行（微秒级，只管在后台采数据和监控健康度） */
  IR_Control();
  AHT30_Process_Background(&hi2c1, &aht30);

  WindowManager_Process();

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

static void IR_Control() {
  if (My_IR.ready) {
    Event_Dispatch_IR(My_IR.cmd); // 交给管家处理
    My_IR.ready = 0;
  }
}