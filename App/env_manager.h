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

// --- 全局影子数据（供 UI 直接读取） ---
extern char g_str_time[9];      // "12:00:00"
extern char g_str_date_std[11]; // "2026-02-21"
extern char g_str_week_en[4];   // "SAT"
extern char g_str_temp[7];      // "25.4C"
extern char g_str_humi[5];      // "45%"
extern char g_str_ampm[3];      // "AM", "PM" 或 ""

extern TimeFormat_t g_time_format_config;

// --- 接口函数 ---
void Env_Manager_Init(AHT30_HandleTypeDef *haht30);
void Env_Manager_Tick(void);
void Env_Manager_SetFormat(TimeFormat_t format);

// 串口对时分发目标
void RTC_OnTimeSyncReceived(uint8_t *payload, uint8_t len);

#endif