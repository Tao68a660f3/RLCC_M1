#ifndef ENV_MANAGER_H
#define ENV_MANAGER_H

#include "aht30.h"
#include "bsp_rtc.h"
#include "main.h"
#include <stdint.h>

// 时间显示配置
typedef enum {
  TIME_FORMAT_24H = 0,
  TIME_FORMAT_12H,
  TIME_FORMAT_MAX,
} TimeFormat_t;

// 温湿度舒适度等级
typedef enum {
  COMFORT_GREEN = 0,  // 舒适（绿）
  COMFORT_YELLOW = 1, // 较不适（黄）
  COMFORT_RED = 2,    // 极端（红）
} ComfortLevel_t;

// --- 全局影子数据（供 UI 直接读取） ---
extern char g_str_time[9];      // "12:00:00"
extern char g_str_time_s[7];    // "12:00"
extern char g_str_date_std[11]; // "2026-02-21"
extern char g_str_date_md[6];   // "02/21"
extern char g_str_week_en[4];   // "SAT"
extern char g_str_temp[7];      // "25.4C"
extern char g_str_humi[5];      // "45%"
extern char g_str_ampm[3];      // "AM", "PM" 或 ""

extern ComfortLevel_t g_temp_comfort; // 温度舒适度等级
extern ComfortLevel_t g_humi_comfort; // 湿度舒适度等级

extern TimeFormat_t g_time_format_config;

// --- 接口函数 ---
void Env_Manager_Init(void);
void Env_Manager_Tick(void);
void Env_Manager_SetFormat(TimeFormat_t format);

// 串口对时分发目标
void RTC_OnTimeSyncReceived(uint8_t *payload, uint8_t len);

#endif