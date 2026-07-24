
#include "canvas_renderer.h"
#include "i2c.h"
#include "main.h"
#include "mem_pool.h"
#include "rtc.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"

#include "aht30.h"
#include "bsp_rtc.h"
#include "com_manager.h"
#include "event_manager.h"
#include "font_config.h"
#include "ir_remote.h"
#include "led_display.h"
#include "led_driver.h"
#include "ui_manager.h"
#include "w25q64.h"
#include "window_manager.h"

#include <stdio.h>

#include "app.h"

uint32_t ui_timer = 0;
AHT30_HandleTypeDef aht30;
RTC_DateTimeTypeDef current_dt;

extern uint8_t need_commit;

static void IR_Control();

static char Msg_01[] = {0xBB, 0xB6, 0xD3, 0xAD, 0xCA, 0xB9, 0xD3, 0xC3,
                        0xCB, 0xAB, 0xC9, 0xAB, 0xB8, 0xE8, 0xB4, 0xCA,
                        0xCF, 0xD4, 0xCA, 0xBE, 0xC6, 0xF7, 0xA3, 0xA1,
                        0x57, 0x65, 0x6C, 0x63, 0x6F, 0x6D, 0x65, 0x20,
                        0x74, 0x6F, 0x20, 0x75, 0x73, 0x65, 0x21, 0x00};
static char Msg_02[] = {0xBB, 0xB6, 0xD3, 0xAD, 0xCA, 0xB9, 0xD3, 0xC3, 0xCB,
                        0xAB, 0xC9, 0xAB, 0xB8, 0xE8, 0xB4, 0xCA, 0xCF, 0xD4,
                        0xCA, 0xBE, 0xC6, 0xF7, 0xA3, 0xA1, 0x00};

void App_Init() {
  COM_Init(&huart1);

  RTC_App_Init(&hrtc);
  AHT30_Init(&aht30);

  W25Q64_Init(&hspi1);
  Font_Config_Init();

  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616S);

  LED_Init(&htim1);
  WindowManager_Init();

  /* 窗口：显示测试文本 */
  Window_Config(0, 0, 0, 192, 16);
  Window_FillText(0, Msg_01, C_RED, CANVAS_R, VALIGN_MIDDLE);
  Window_SetAlignment(0, ALIGN_LEFT);
  window_list[0].scroll_divider = 1;
  window_list[0].scroll_step = -1;

  Window_Config(1, 0, 16, 192, 16);
  Window_FillText(1, Msg_02, C_GREEN, CANVAS_G, VALIGN_MIDDLE);
  Window_SetAlignment(1, ALIGN_LEFT);
  window_list[1].scroll_divider = 1;
  window_list[1].scroll_step = -1;
}

void App_Loop() {
  /* 1. 后台静默运行（微秒级，只管在后台采数据和监控健康度） */
  IR_Control();
  AHT30_Process_Background(&hi2c1, &aht30);

  /* 1b. 串口数据消费（根据系统模式选择文本/协议模式） */
  if (g_curr_sys_mode == SYS_MODE_PROTOCOL_MODE) {
    COM_Process_ProtocolMode();
  } else if (g_curr_sys_mode == SYS_MODE_TXT_MODE) {
    COM_Process_TextMode();
  }

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