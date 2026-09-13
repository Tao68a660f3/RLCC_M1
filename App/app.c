
#include "i2c.h"
#include "main.h"
#include "rtc.h"
#include "spi.h"
#include "stm32f4xx_hal.h"
#include "tim.h"
#include "usart.h"

#include "bsp_rtc.h"
#include "canvas_renderer.h"
#include "com_manager.h"
#include "env_manager.h"
#include "event_manager.h"
#include "font_config.h"
#include "ir_remote.h"
#include "led_display.h"
#include "led_driver.h"
#include "lyric_service.h"
#include "lyric_window_manager.h"
#include "mem_pool.h"
#include "ui_manager.h"
#include "w25q64.h"
#include "window_manager.h"

#include <stdio.h>

#include "app.h"

// 歌词窗口模式选择：改为 1 则单行32点，改为 2 则双行16点，改为 4 则四行8点
#define LYRIC_LINE_COUNT 2

uint32_t ui_timer = 0;
RTC_DateTimeTypeDef current_dt;

extern uint8_t need_commit;

static void IR_Control();

static char Msg_01[] = {0xBB, 0xB6, 0xD3, 0xAD, 0xCA, 0xB9, 0xD3, 0xC3, 0xCB,
                        0xAB, 0xC9, 0xAB, 0xB8, 0xE8, 0xB4, 0xCA, 0xCF, 0xD4,
                        0xCA, 0xBE, 0xC6, 0xF7, 0xA3, 0xA1, 0x00};
static char Msg_02[] = {0x57, 0x65, 0x6C, 0x63, 0x6F, 0x6D, 0x65, 0x20,
                        0x54, 0x6F, 0x20, 0x55, 0x73, 0x65, 0x21, 0x00};

void App_Init() {
  LyricService_Init(); // 歌词服务：初始化 g_sys 并调用 Sync_Init() 初始化
                       // 时间轴模块（sync_algorithm），必须早于收包处理
  // 注：COM_Init(&huart1) 已提前到 main.c 中 MX_USART1_UART_Init() 之后调用，
  //     使 RX DMA 尽早启动，避免对端先上电时数据丢失。
  W25Q64_Init(&hspi1);
  Font_Config_Init();

  RTC_App_Init(&hrtc);
  Env_Manager_Init();

  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616H);

  LED_Init(&htim1);
  LED_SetBrightness(30);
  WindowManager_Init();

  Window_Config(0, 0, 0, 192, 16);
  Window_Config(1, 0, 16, 192, 16);
  Window_FillText(0, Msg_01, C_YELLOW, CANVAS_Y, VALIGN_MIDDLE);
  Window_SetAlignment(0, ALIGN_CENTER);
  for (uint8_t j = 0; j < 2; j++) {
    need_commit = 0; // 确保显示的启动信息能提交上。
    LED_Commit();
    WindowManager_Process();
  }
  HAL_Delay(500);
  Window_FillText(1, Msg_02, C_GREEN, CANVAS_G, VALIGN_MIDDLE);
  Window_SetAlignment(1, ALIGN_CENTER);
  for (uint8_t j = 0; j < 2; j++) {
    need_commit = 0; // 确保显示的启动信息能提交上。
    LED_Commit();
    WindowManager_Process();
  }
  HAL_Delay(5000);

  /* 启动默认 UI 模式（HomeLife：大字时间 + 温湿度） */
  UI_Manager_SetMode_HomeLife();
}

void App_Loop() {
  /* 1. 后台静默运行（微秒级，只管在后台采数据和监控健康度） */
  IR_Control();

  Env_Manager_Tick();
  UI_Manager_Tick();

  // /* 1b. 串口数据消费（根据系统模式选择文本/协议模式） */
  // if (g_curr_sys_mode == SYS_MODE_PROTOCOL_MODE) {
  //   COM_Process_ProtocolMode();
  // } else if (g_curr_sys_mode == SYS_MODE_TXT_MODE) {
  //   COM_Process_TextMode();
  // }

  // // WindowManager_Process();
  // LyricWM_RenderMgr();

  // /* 2. 前台业务：比如屏幕每 500ms 刷一次，或者串口/网络上报 */
  // if (HAL_GetTick() - ui_timer >= 3000) {
  //   ui_timer = HAL_GetTick();

  //   // 获取最新时间
  //   RTC_GetDateTime(&hrtc, &current_dt);

  //   // 获取最新温湿度
  //   float temp = 0.0f, hum = 0.0f;
  //   uint8_t aht_ok = AHT30_Get_SafeData(&aht30, &temp, &hum);

  //   // 格式化输出/刷屏
  //   printf("[%04d-%02d-%02d %02d:%02d:%02d] ", current_dt.year,
  //          current_dt.month, current_dt.day, current_dt.hours,
  //          current_dt.minutes, current_dt.seconds);

  //   if (aht_ok) {
  //     printf("Temp: %.1f C | Hum: %.1f %%\r\n", temp, hum);
  //   } else {
  //     printf("Temp: --.- C | Hum: --.- %%\r\n");
  //   }
  // }
}

static void IR_Control() {
  if (My_IR.ready) {
    Event_Dispatch_IR(My_IR.cmd); // 交给管家处理
    My_IR.ready = 0;
  }
}