#include "bsp_rtc.h"

/* 备份寄存器暗号，用来判断 RTC 是否已经初始化过 */
#define RTC_BKP_SIGNATURE 0xA5A5

/**
 * @brief  RTC 应用层初始化（带备份域防重置检查）
 * @param  hrtc RTC句柄
 * @return 1: 第一次初始化（重新设置了默认时间）; 0:
 * 已经运行中（保留了先前时间）
 */
uint8_t RTC_App_Init(RTC_HandleTypeDef *hrtc) {
  // 检查备份寄存器 DR0
  if (HAL_RTCEx_BKUPRead(hrtc, RTC_BKP_DR0) != RTC_BKP_SIGNATURE) {
    // 说明是第一次上电（或备用电池没电了），设置一个默认初始时间
    RTC_DateTimeTypeDef default_dt = {.year = 2026,
                                      .month = 7,
                                      .day = 23,
                                      .weekday = 4, // 周四
                                      .hours = 12,
                                      .minutes = 0,
                                      .seconds = 0};

    RTC_SetDateTime(hrtc, &default_dt);

    // 写入暗号，防止下次复位被重置
    HAL_RTCEx_BKUPWrite(hrtc, RTC_BKP_DR0, RTC_BKP_SIGNATURE);
    return 1;
  }

  return 0; // 内部 RTC 一直在跑，无需重置
}

/**
 * @brief  设置 RTC 年月日时分秒
 */
uint8_t RTC_SetDateTime(RTC_HandleTypeDef *hrtc, RTC_DateTimeTypeDef *dt) {
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  // 1. 设置时间
  sTime.Hours = dt->hours;
  sTime.Minutes = dt->minutes;
  sTime.Seconds = dt->seconds;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;

  if (HAL_RTC_SetTime(hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
    return 0;
  }

  // 2. 设置日期 (STM32 HAL 年份只存后两位，如 2026 年存 26)
  sDate.WeekDay = dt->weekday;
  sDate.Month = dt->month;
  sDate.Date = dt->day;
  /* 统一约定：dt->year 必须是 2000~2099 之间的四位数 */
  if (dt->year >= 2000 && dt->year <= 2099) {
    sDate.Year = dt->year - 2000; // 或者 dt->year % 100
  } else {
    return 0; // 传入非法年份直接报错返回
  }

  if (HAL_RTC_SetDate(hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
    return 0;
  }

  return 1;
}

/**
 * @brief  读取 RTC 当前完整时间
 * @note   必须按照 先 ReadTime 再 ReadDate 的顺序！否则寄存器锁存不会更新！
 */
uint8_t RTC_GetDateTime(RTC_HandleTypeDef *hrtc, RTC_DateTimeTypeDef *dt) {
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* 必须步骤 1：先读 Time */
  if (HAL_RTC_GetTime(hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
    return 0;
  }

  /* 必须步骤 2：紧接着读
   * Date（即使你不需要日期，也必须读，否则时钟解锁失败，时间不再更新） */
  if (HAL_RTC_GetDate(hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
    return 0;
  }

  /* 赋值输出 */
  dt->hours = sTime.Hours;
  dt->minutes = sTime.Minutes;
  dt->seconds = sTime.Seconds;

  dt->year = 2000 + sDate.Year;
  dt->month = sDate.Month;
  dt->day = sDate.Date;
  dt->weekday = sDate.WeekDay;

  return 1;
}