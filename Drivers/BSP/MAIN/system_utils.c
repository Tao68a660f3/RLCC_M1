#include "system_utils.h"
#include "main.h"

/* --- 初始化 DWT 周期计数器 ---
 * 无条件使能：若调试器/仿真器已先置了 TRCENA，旧写法会跳过 CYCCNTENA，
 * 导致 DWT->CYCCNT 恒为 0 → 延迟测试的 proc_us 与 IDLE 时间戳全部失效。
 * 本函数幂等，重复调用仅把 CYCCNT 归零。 */
void DWT_Init(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* --- 精准微秒延迟 --- */
void delay_us(uint32_t us) {
  uint32_t start = DWT->CYCCNT;
  uint32_t ticks = us * (SystemCoreClock / 1000000);
  while ((DWT->CYCCNT - start) < ticks)
    ;
}
