#include "env_manager.h"
#include "i2c.h"
#include "lyric_service.h"
#include "rtc.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CELSIUS "\02"

// --- 静态资源：英文查表 ---
static const char *WEEK_NAMES[] = {"SUN", "MON", "TUE", "WED",
                                   "THU", "FRI", "SAT"};

// --- 全局影子变量初始化 ---
char g_str_time[9] = "00:00:00";
char g_str_time_s[7] = "00:00";
char g_str_date_std[11] = "2000-01-01";
char g_str_date_md[6] = "01/01";
char g_str_week_en[4] = "???";
char g_str_temp[7] = "--.-" CELSIUS;
char g_str_humi[5] = "--%";
char g_str_ampm[3] = "";

TimeFormat_t g_time_format_config = TIME_FORMAT_24H;

// 温湿度舒适度等级
ComfortLevel_t g_temp_comfort = COMFORT_GREEN;
ComfortLevel_t g_humi_comfort = COMFORT_GREEN;

// 内部状态记录
static RTC_DateTimeTypeDef last_sync_time;
static AHT30_HandleTypeDef s_haht30;
static uint32_t last_aht_trigger_tick = 0;
static uint32_t last_rtc_read_tick = 0;

/**
 * @brief 手搓数字转字符（补零）
 */
static void _uitoa_2d(uint8_t val, char *out) {
  out[0] = (val / 10 % 10) + '0';
  out[1] = (val % 10) + '0';
}

/**
 * @brief 精炼时间字符串 (逻辑核心)
 */
static void _Env_Refine_Time(RTC_DateTimeTypeDef *t) {
  uint8_t h = t->hours;

  // 1. 处理 AM/PM 和 12/24H 转换
  if (g_time_format_config == TIME_FORMAT_12H) {
    if (h >= 12) {
      strcpy(g_str_ampm, "PM");
      if (h > 12)
        h -= 12;
    } else {
      strcpy(g_str_ampm, "AM");
      if (h == 0)
        h = 12;
    }
  } else {
    strcpy(g_str_ampm, " ");
  }

  // 2. 填充 g_str_time "HH:MM:SS" 和 g_str_time_s "HH:MM"
  _uitoa_2d(h, &g_str_time[0]);
  _uitoa_2d(h, &g_str_time_s[0]);
  g_str_time[2] = ':';
  g_str_time_s[2] = ':';
  _uitoa_2d(t->minutes, &g_str_time[3]);
  _uitoa_2d(t->minutes, &g_str_time_s[3]);
  g_str_time[5] = ':';
  _uitoa_2d(t->seconds, &g_str_time[6]);
  g_str_time[8] = '\0';
  g_str_time_s[5] = '\0'; // fix: missing null terminator

  if (g_str_time[0] == '0') {
    g_str_time[0] = ' ';
  }
}

/**
 * @brief 精炼日期与星期
 */
static void _Env_Refine_Date(RTC_DateTimeTypeDef *t) {
  // yyyy-mm-dd
  uint16_t full_year = t->year;
  g_str_date_std[0] = (full_year / 1000) + '0';
  g_str_date_std[1] = (full_year / 100 % 10) + '0';
  g_str_date_std[2] = (full_year / 10 % 10) + '0';
  g_str_date_std[3] = (full_year % 10) + '0';
  g_str_date_std[4] = '-';
  _uitoa_2d(t->month, &g_str_date_std[5]);
  g_str_date_std[7] = '-';
  _uitoa_2d(t->day, &g_str_date_std[8]);
  g_str_date_std[10] = '\0';

  // 填充 g_str_date_md "MM/DD"
  _uitoa_2d(t->month, &g_str_date_md[0]);
  g_str_date_md[2] = '/';
  _uitoa_2d(t->day, &g_str_date_md[3]);
  g_str_date_md[5] = '\0';

  // Week: SUN, MON... (STM32 HAL RTC: 1=Mon, 2=Tue...6=Sat, 7=Sun)
  uint8_t wix = (t->weekday == 7) ? 0 : t->weekday;
  strncpy(g_str_week_en, WEEK_NAMES[wix], 3);
  g_str_week_en[3] = '\0';
}

void Env_Manager_Init(void) {
  // --- 1. 初始化 AHT30 传感器（env_manager 自己持有）---
  AHT30_Init(&s_haht30);

  // --- 2. 强制进行第一次同步读取 ---
  RTC_GetDateTime(&hrtc, &last_sync_time);
  _Env_Refine_Time(&last_sync_time);
  _Env_Refine_Date(&last_sync_time);

  // --- 3. 初始化传感器计时器 ---
  last_aht_trigger_tick = HAL_GetTick();
  last_rtc_read_tick = HAL_GetTick();
}

void Env_Manager_Tick(void) {
  uint32_t now = HAL_GetTick();

  // --- 1. 检查并处理串口发来的对时请求 ---
  if (g_sys.rtc_dirty) {
    RTC_DateTimeTypeDef sync_time;

    // 将 rtc_buf[7] 转换为结构体 (顺序是 Y, M, D, h, m, s, w)
    sync_time.year = 2000U + g_sys.rtc_buf[0];
    sync_time.month = g_sys.rtc_buf[1];
    sync_time.day = g_sys.rtc_buf[2];
    sync_time.hours = g_sys.rtc_buf[3];
    sync_time.minutes = g_sys.rtc_buf[4];
    sync_time.seconds = g_sys.rtc_buf[5];
    // 协议: 0~6 (0=Sun, 1=Mon...6=Sat) → STM32 HAL RTC: 1~7 (1=Mon...7=Sun)
    sync_time.weekday = (g_sys.rtc_buf[6] == 0) ? 7 : g_sys.rtc_buf[6];

    // 调用 HAL RTC 设置
    RTC_SetDateTime(&hrtc, &sync_time);

    // 立即更新影子字符串
    _Env_Refine_Time(&sync_time);
    _Env_Refine_Date(&sync_time);

    g_sys.rtc_dirty = 0;

    // printf("[RTC] Time Sync Applied: %04d-%02d-%02d\r\n", sync_time.year,
    //  sync_time.month, sync_time.day);
  }

  // --- 2. RTC 定时读取 (每 250ms 读一次) ---
  if (now - last_rtc_read_tick > 250) {
    RTC_DateTimeTypeDef cur;
    RTC_GetDateTime(&hrtc, &cur);

    if (cur.seconds != last_sync_time.seconds) {
      _Env_Refine_Time(&cur);
      if (cur.day != last_sync_time.day) {
        _Env_Refine_Date(&cur);
      }
      last_sync_time = cur;
    }
    last_rtc_read_tick = now;
  }

  // --- 2.5 AHT30 状态机驱动（每帧调用，内部非阻塞）---
  AHT30_Process_Background(&hi2c1, &s_haht30);

  // --- 3. AHT30 数据读取（每 3s 取一次最新值）---
  if (now - last_aht_trigger_tick > 3000) {
    float temp = 0.0f, hum = 0.0f;
    uint8_t aht_ok = AHT30_Get_SafeData(&s_haht30, &temp, &hum);
    if (aht_ok) {
      int t_int = (int)temp;
      int t_dec = (int)(temp * 10) % 10;
      if (t_dec < 0)
        t_dec = -t_dec;

      snprintf(g_str_temp, sizeof(g_str_temp), "%2d.%d" CELSIUS, t_int, t_dec);
      snprintf(g_str_humi, sizeof(g_str_humi), "%2d%%", (int)hum);
      printf("Temp: %.1f C | Hum: %.1f %%\r\n", temp, hum);

      // 温度舒适度
      if (temp >= 18.0f && temp <= 26.0f)
        g_temp_comfort = COMFORT_GREEN;
      else if (temp >= 14.0f && temp <= 32.0f)
        g_temp_comfort = COMFORT_YELLOW;
      else
        g_temp_comfort = COMFORT_RED;

      // 湿度舒适度
      if (hum >= 40.0f && hum <= 70.0f)
        g_humi_comfort = COMFORT_GREEN;
      else if (hum >= 30.0f && hum <= 80.0f)
        g_humi_comfort = COMFORT_YELLOW;
      else
        g_humi_comfort = COMFORT_RED;
    } else {
      strcpy(g_str_temp, "--.-" CELSIUS);
      strcpy(g_str_humi, "--%");
    }
    last_aht_trigger_tick = now;
  }
}

void Env_Manager_SetFormat(TimeFormat_t format) {
  g_time_format_config = format;
  RTC_DateTimeTypeDef cur;
  RTC_GetDateTime(&hrtc, &cur);
  _Env_Refine_Time(&cur);
}

void RTC_OnTimeSyncReceived(uint8_t *payload, uint8_t len) {
  if (len < 7)
    return;
  for (int i = 0; i < 7; i++) {
    g_sys.rtc_buf[i] = payload[i];
  }
  g_sys.rtc_dirty = 1;
}