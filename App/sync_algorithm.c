/*
 * ==========================================================================
 * 移植说明（本文件自 fake_stm32_sync_simulator 逐字节搬入，请勿随意改动）
 *   - 仅 1 行差异：上游 #include "hal_time.h"（模拟器的假 HAL_GetTick），
 *     改为 #include "main.h"（STM32 HAL 提供同原型的 HAL_GetTick）。
 *   - 除本注释与上述 include 外请勿修改本文件：模拟器自测
 *     (tests/sync_selftest.c) 的结论对固件仍然有效，且便于日后回灌上游。
 *   - 依赖：只有 HAL_GetTick() (ms)；无浮点、无 malloc、无 printf。
 *   - 调用约定：Sync_OnPacketAt() 应传入"报文真正到达"的 tick；
 *     本项目收包与取位置都在主循环上下文，无跨上下文撕裂风险。
 *   - 接入点：lyric_service.c (0x11 → Sync_OnPacketAt) /
 *     lyric_window_manager.c (每帧 Sync_GetTime 采样)。
 * ==========================================================================
 */
#include "sync_algorithm.h"
#include "main.h"

/*
====================================================
状态
====================================================
*/

#define SYNC_STATE_IDLE 0
#define SYNC_STATE_RUNNING 1
#define SYNC_STATE_PAUSED 2

typedef struct
{
    /* 0=未开始 1=运行中 2=暂停 */
    uint8_t state;

    /* 是否已经建立 anchor */
    uint8_t initialized;

    /*
        anchor:
        在某个 B tick 时刻，A 的时间轴位置是多少

        位置用微秒存:
        rate 是 ppm 级(1ppm = 1e-6)，如果 anchor 只存毫秒，
        每包重新折算 anchor 时的取整误差会累积成明显的位置漂移。
    */
    uint32_t anchor_local_tick;
    int64_t anchor_time_us;

    /*
        速率修正(ppm)
        >0 表示 B 的播放位置推进得比本地时钟快
        (只含 A/B 钟差修正部分，倍速在 pb_q16 里)
    */
    int32_t rate_ppm;

    /*
        模型斜率(Q16.16)
        65536 = 1.0，即本地 1ms 时间轴走 1ms
        slope = 倍速 × (1 + rate_ppm/1e6)
    */
    int32_t slope_q16;

    /*
        上一次做校正的本地 tick
    */
    uint32_t last_control_tick;

    /*
        时延窗口
        offset = B_tick - A_tick
    */
    uint32_t delay_samples[SYNC_DELAY_WINDOW_SIZE];
    uint8_t delay_index;
    uint8_t delay_count;
    uint32_t delay_min;
    int32_t delay_excess_avg;
    int32_t excess_limit;
    int32_t ontime_limit;

    /*
        历史最佳基准(长期): 链路最理想状态下的 (B_tick - A_tick)。
        0xFFFFFFFF = 尚未建立。

        与 delay_min 的区别:
          delay_min      = 当前窗口内的最快路径, 随窗口滚动,
                           暂停/恢复、epoch 重建、看门狗都会重建它;
          baseline_delay = 历史最佳, 跨暂停/恢复/普通 seek 都保留。

        用途:
          1) (B_tick - A_tick) 是否发生 epoch 级跳变的判据
          2) congestion = offset - baseline_delay, 拥塞程度(诊断)
    */
    uint32_t baseline_delay;
    int32_t congestion;

    /*
        A 的时间轴总长度(ms), 0 = 未知。
        只当"时间轴上限"用: B 的预测位置不允许超过它,
        不参与同步, 也不是新的时间源。
    */
    uint32_t total_ms;

    /*
        A 端最近状态
    */
    uint32_t last_remote_tick;
    uint32_t last_packet_local_tick;

    /*
        epoch(时延基线)是否有效
    */
    uint8_t epoch_valid;
    uint8_t epoch_vote;

    /*
        硬复位投票
    */
    uint8_t hard_vote;
    int8_t hard_dir;

    /*
        暂停时的时间轴位置
    */
    uint32_t pause_time;

    /*
        A/B 时钟频率差测量
        每段(10秒)内的最小 offset，索引 0 是最新一段
    */
    uint32_t drift_off[SYNC_DRIFT_SEGMENTS];
    uint32_t drift_tick[SYNC_DRIFT_SEGMENTS];
    uint8_t drift_count;
    uint32_t drift_seg_min;
    uint32_t drift_seg_start;
    int32_t drift_ppm;

    /*
        倍速窗口: 最近几个包的 (A tick, A 时间轴位置)
        索引 0 是最新的一个
    */
    uint32_t pb_tick[SYNC_RATE_WINDOW];
    uint32_t pb_pos[SYNC_RATE_WINDOW];
    uint8_t pb_count;
    uint8_t pb_valid;
    int32_t pb_q16;
    int32_t pb_pair_q16;

    int32_t last_error;
    int32_t last_excess;

    /*
        最近一次"采纳观测"的本地 tick，看门狗用
    */
    uint32_t last_accept_tick;

    /* 诊断统计 */
    SyncStats stats;

} SyncContext;

static SyncContext sync;

/*
====================================================
工具
====================================================
*/

/*
    a - b
    结果是有符号的小差值，天然处理 tick 回绕
*/
static int32_t wrap_diff(
    uint32_t a,
    uint32_t b)
{
    return (int32_t)(a - b);
}

/*
    位置模型:

        pos = anchor + elapsed_local * slope

    slope 用 Q16.16(65536 = 1.0)，
    slope 与 anchor 都必须用同一个公式折算，
    这样换斜率时把差值补回 anchor 才能做到位置精确不跳变。
*/
static int64_t slope_advance_us(
    uint32_t elapsed_ms,
    int32_t slope_q16)
{
    return (int64_t)elapsed_ms * (int64_t)slope_q16 * 1000 / 65536;
}

/*
====================================================
时延窗口
====================================================

注意:
offset = B_tick - A_tick，是两个独立 tick 计数器相减，
包含了 A/B 的 epoch 差，量级可能接近 2^32。

所以绝对不能对 offset 直接做乘加(会溢出)，
所有统计都必须建立在"相对最小值的残差"上。
*/

static void delay_window_clear(void)
{
    uint8_t i;

    for (i = 0; i < SYNC_DELAY_WINDOW_SIZE; i++)
        sync.delay_samples[i] = 0;

    sync.delay_index = 0;
    sync.delay_count = 0;
    sync.delay_min = 0xFFFFFFFFu;
    sync.delay_excess_avg = 0;
    sync.excess_limit = SYNC_EXCESS_MIN_MS;
    sync.ontime_limit = SYNC_EXCESS_MIN_MS;
}

static void delay_window_recalc(void)
{
    uint8_t i;
    uint32_t ref;
    int32_t d_min;
    uint32_t excess_sum;
    uint8_t excess_cnt;

    if (sync.delay_count == 0) {
        sync.delay_min = 0xFFFFFFFFu;
        sync.delay_excess_avg = 0;
        sync.excess_limit = SYNC_EXCESS_MIN_MS;
        sync.ontime_limit = SYNC_EXCESS_MIN_MS;
        return;
    }

    /*
        以最后一个样本为参考做有符号比较，
        直接比较 uint32 会在回绕处得出错误结果。
    */
    i = sync.delay_index;
    if (i == 0)
        i = SYNC_DELAY_WINDOW_SIZE;
    ref = sync.delay_samples[i - 1];

    d_min = 0x7FFFFFFF;

    for (i = 0; i < sync.delay_count; i++) {
        int32_t d = wrap_diff(sync.delay_samples[i], ref);
        if (d < d_min)
            d_min = d;
    }

    sync.delay_min = ref + (uint32_t)d_min;

    /*
        超额延迟 = 相对最快路径多花的时间，
        量级很小(几百毫秒)，乘加不会溢出。
    */
    excess_sum = 0;
    excess_cnt = 0;

    for (i = 0; i < sync.delay_count; i++) {
        int32_t e = wrap_diff(sync.delay_samples[i], sync.delay_min);
        if (e < 0)
            e = 0;
        if (e > SYNC_EXCESS_MAX_MS)
            e = SYNC_EXCESS_MAX_MS;
        excess_sum += (uint32_t)e;
        excess_cnt++;
    }

    if (excess_cnt > 0)
        sync.delay_excess_avg = (int32_t)(excess_sum / excess_cnt);

    /*
        过期门限跟着链路实际抖动走
    */
    sync.excess_limit = sync.delay_excess_avg * SYNC_EXCESS_FACTOR;

    if (sync.excess_limit < SYNC_EXCESS_MIN_MS)
        sync.excess_limit = SYNC_EXCESS_MIN_MS;
    if (sync.excess_limit > SYNC_EXCESS_MAX_MS)
        sync.excess_limit = SYNC_EXCESS_MAX_MS;

    /*
        "这个包算准时"的门限:
        门限内的包说明它明显快于最快路径 -> 观测可信 ->
        可以用来做即时硬复位(见 sync_apply_error)。

        抖动大的链路这个门限会跟着放大，
        否则高抖动链路上每个包都要等第二次确认，seek 反应就很慢。
    */
    sync.ontime_limit = sync.delay_excess_avg + SYNC_EXCESS_MIN_MS;

    if (sync.ontime_limit > sync.excess_limit)
        sync.ontime_limit = sync.excess_limit;
    if (sync.ontime_limit < SYNC_EXCESS_MIN_MS)
        sync.ontime_limit = SYNC_EXCESS_MIN_MS;
}

static void delay_window_push(
    uint32_t offset)
{
    sync.delay_samples[sync.delay_index] = offset;

    sync.delay_index++;
    if (sync.delay_index >= SYNC_DELAY_WINDOW_SIZE)
        sync.delay_index = 0;

    if (sync.delay_count < SYNC_DELAY_WINDOW_SIZE)
        sync.delay_count++;

    delay_window_recalc();
}

/*
    历史最佳基准(长期)。

    只降不升:
      更快的包是真实信息(说明链路能达到更好的状态), 立刻采纳;
      被网络拖慢的包不会把它抬高, 所以不会被普通大延迟样本污染。

    唯一允许"向上重设"的地方:
      sync_reset_epoch()  A 换了 tick epoch, 旧数值已经无意义
      sync_watchdog()     连续 15s 没有观测被采纳, 链路最佳延迟整体变了

    暂停 / 恢复播放 / 普通 seek 都不会碰它。
*/
static void delay_baseline_update(
    uint32_t offset)
{
    if (sync.baseline_delay == 0xFFFFFFFFu
        || wrap_diff(offset, sync.baseline_delay) < 0)
        sync.baseline_delay = offset;
}

/*
    强制把历史最佳基准重设为当前 offset
    (只在上面那两个"世界变了"的场景调用)
*/
static void delay_baseline_reset(
    uint32_t offset)
{
    sync.baseline_delay = offset;
}

/*
====================================================
A/B 时钟频率差(钟差)测量
====================================================

offset = B_tick - A_tick 对时间的斜率就是钟差。

单个样本的 offset 被网络抖动污染得很厉害，所以每段取最小值
(最快路径)，再比较首尾两段最小值 —— 抖动能被最小化掉，
而 120 秒的钟差(即使只有 10ppm)也有 1.2ms 的位移，足够分辨。
*/

static void drift_reset(
    uint32_t local_tick)
{
    int i;

    for (i = 0; i < SYNC_DRIFT_SEGMENTS; i++) {
        sync.drift_off[i] = 0;
        sync.drift_tick[i] = 0;
    }

    sync.drift_count = 0;
    sync.drift_seg_min = 0xFFFFFFFFu;
    sync.drift_seg_start = local_tick;
    sync.drift_ppm = 0;
}

static void drift_recalc(void)
{
    int32_t d_off;
    int32_t drift;
    uint32_t d_tick;
    uint8_t last;
    int i;

    if (sync.drift_count < SYNC_DRIFT_MIN_SEGMENTS) {
        sync.drift_ppm = 0;
        return;
    }

    last = (uint8_t)(sync.drift_count - 1);

    d_off = wrap_diff(sync.drift_off[0], sync.drift_off[last]);
    d_tick = sync.drift_tick[0] - sync.drift_tick[last];

    if (d_tick < 1000)
        return;

    drift = (int32_t)((int64_t)d_off * 1000000 / (int64_t)d_tick);

    /*
        超出 ±SYNC_DRIFT_PPM_LIMIT 的钟差是不可能存在的(晶振差不了这么多)，
        多半是 A 的 tick 出过异常: 被暂停过、被改过、或者换了计数器。

        这时保留上一次的钟差估计，把历史丢掉重新测 ——
        否则用一个错的钟差去改模型斜率，播放位置会慢慢漂走。
    */
    if (drift > SYNC_DRIFT_PPM_LIMIT || drift < -SYNC_DRIFT_PPM_LIMIT) {

        uint32_t o = sync.drift_off[0];
        uint32_t t = sync.drift_tick[0];

        for (i = 0; i < SYNC_DRIFT_SEGMENTS; i++) {
            sync.drift_off[i] = 0;
            sync.drift_tick[i] = 0;
        }

        /* 以刚采集到的这一段重新开始测 */
        sync.drift_off[0] = o;
        sync.drift_tick[0] = t;
        sync.drift_count = 1;
        sync.drift_seg_min = 0xFFFFFFFFu;
        sync.drift_seg_start = t;

        return;
    }

    sync.drift_ppm = drift;
}

static void drift_seg_push(
    uint32_t offset,
    uint32_t tick)
{
    int i;

    for (i = SYNC_DRIFT_SEGMENTS - 1; i > 0; i--) {
        sync.drift_off[i] = sync.drift_off[i - 1];
        sync.drift_tick[i] = sync.drift_tick[i - 1];
    }

    sync.drift_off[0] = offset;
    sync.drift_tick[0] = tick;

    if (sync.drift_count < SYNC_DRIFT_SEGMENTS)
        sync.drift_count++;

    drift_recalc();
}

/*
    每个被采纳的观测调用一次
*/
static void drift_update(
    uint32_t offset,
    uint32_t local_recv_tick)
{
    /* 段内取最小值 = 最快路径，抗网络抖动 */
    if (sync.drift_seg_min == 0xFFFFFFFFu
        || wrap_diff(offset, sync.drift_seg_min) < 0)
        sync.drift_seg_min = offset;

    if (local_recv_tick - sync.drift_seg_start >= SYNC_DRIFT_SEGMENT_MS) {
        drift_seg_push(sync.drift_seg_min, local_recv_tick);
        sync.drift_seg_start = local_recv_tick;
        sync.drift_seg_min = offset;
    }
}

/*
====================================================
模型斜率 / 倍速学习
====================================================
*/

/*
    改变模型斜率，同时保证播放位置连续。

    pos = anchor + elapsed * slope，elapsed 是"从 anchor 起"的全部时间，
    直接改 slope 会追溯性平移位置(和以前改 rate 是同一个坑)。

    这里把两个斜率下这段时间的差值补回 anchor:
    用的是同一个 slope_advance_us()，位置精确保不变(不是近似)。
*/
static void sync_set_slope(
    int32_t new_slope_q16)
{
    uint32_t elapsed;
    int64_t advance_old;
    int64_t advance_new;

    if (new_slope_q16 == sync.slope_q16)
        return;

    elapsed = HAL_GetTick() - sync.anchor_local_tick;

    advance_old = slope_advance_us(elapsed, sync.slope_q16);
    advance_new = slope_advance_us(elapsed, new_slope_q16);

    sync.anchor_time_us += (advance_old - advance_new);
    sync.slope_q16 = new_slope_q16;
}

/*
    重新计算模型斜率:
        slope = 倍速 × (1 + 钟差修正)
*/
static void sync_update_slope(void)
{
    int64_t s;

    s = (int64_t)sync.pb_q16
        * (1000000 + (int64_t)sync.rate_ppm) / 1000000;

    if (s < SYNC_SLOPE_MIN_Q16)
        s = SYNC_SLOPE_MIN_Q16;
    if (s > SYNC_SLOPE_MAX_Q16)
        s = SYNC_SLOPE_MAX_Q16;

    sync_set_slope((int32_t)s);
}

static void pb_window_reset(void)
{
    int i;

    for (i = 0; i < SYNC_RATE_WINDOW; i++) {
        sync.pb_tick[i] = 0;
        sync.pb_pos[i] = 0;
    }

    sync.pb_count = 0;
    sync.pb_pair_q16 = 0;
}

/*
    重新估计倍速。

    倍速 = Δcurrent_ms / Δremote_tick_ms，两个量都来自 A，
    所以跟 B 的时钟、跟网络抖动无关。

    限制单次变化幅度: 一个坏样本不能把模型带偏。
*/
static void pb_window_recalc(void)
{
    int32_t d_pos;
    uint32_t d_tick;
    int32_t est_q16;
    int32_t step;
    int32_t limit;
    uint8_t last;

    if (sync.pb_count < SYNC_RATE_MIN_SAMPLES)
        return;

    last = (uint8_t)(sync.pb_count - 1);

    d_tick = sync.pb_tick[0] - sync.pb_tick[last];
    d_pos = wrap_diff(sync.pb_pos[0], sync.pb_pos[last]);

    if (d_tick < SYNC_RATE_MIN_SPAN_MS)
        return;

    est_q16 = (int32_t)((int64_t)d_pos * 65536 / (int64_t)d_tick);

    if (est_q16 < SYNC_SLOPE_MIN_Q16)
        est_q16 = SYNC_SLOPE_MIN_Q16;
    if (est_q16 > SYNC_SLOPE_MAX_Q16)
        est_q16 = SYNC_SLOPE_MAX_Q16;

    if (!sync.pb_valid) {
        /* 第一次直接采用，尽快锁定倍速 */
        sync.pb_valid = 1;
        sync.pb_q16 = est_q16;
    } else if (est_q16 > sync.pb_q16 + sync.pb_q16 / 20
        || est_q16 < sync.pb_q16 - sync.pb_q16 / 20) {

        /*
            偏差超过 5%: 认为 A 改了倍速(不是噪声)，
            直接采用，并且把旧倍速的样本丢掉重新积累，
            否则窗口里旧样本会把估计拖住十几秒。
        */
        sync.pb_q16 = est_q16;

        /* 只保留最新一个样本，窗口重新积累 */
        sync.pb_count = 1;
    } else {
        step = (est_q16 - sync.pb_q16) / SYNC_PB_LPF_DIV;

        limit = sync.pb_q16 / SYNC_PB_STEP_DIV;
        if (limit < 1)
            limit = 1;

        if (step > limit)
            step = limit;
        if (step < -limit)
            step = -limit;

        sync.pb_q16 += step;
    }

    sync_update_slope();
}

/*
    喂样本。

    1) 窗口按时间抽样(默认 2s 一个点): 包发得很快时，
       8 个点也要覆盖足够长的时间跨度，斜率才够准。
    2) 相邻两段的斜率差太大 = 时间轴跳变(seek/reset)，
       丢掉旧样本重新积累 —— 否则会把倍速算成跳变后的值。

    注意 1: 这里不因为 rebase 就整体清空窗口。
    如果倍速还没学对，误差会很大并反复 rebase，
    一清空就永远凑不满样本(实测倍速永远学不出来)。

    注意 2: 存进窗口的是原始位置，不做低通。
    低通只在抽样时更新的话滞后会不断变化(实测位置呈 +1000/+7000 交替)，
    斜率会被算成 0.5/3.5 这种鬼值。
    A 端补偿噪声(±几十ms)在 14 秒跨度上只让斜率差 ±0.1~0.3%，可以接受。
*/
static void pb_window_push(
    uint32_t remote_tick,
    uint32_t remote_pos)
{
    uint32_t dt;
    int32_t d;
    int32_t pair_q16;
    int i;

    if (sync.pb_count > 0) {

        dt = remote_tick - sync.pb_tick[0];

        if (dt < SYNC_RATE_SAMPLE_MS)
            return;

        d = wrap_diff(remote_pos, sync.pb_pos[0]);
        pair_q16 = (int32_t)((int64_t)d * 65536 / (int64_t)dt);

        if (sync.pb_pair_q16 != 0
            && (pair_q16 > sync.pb_pair_q16 + sync.pb_pair_q16 / 2
                || pair_q16 < sync.pb_pair_q16 - sync.pb_pair_q16 / 2)) {

            /* 斜率突变: 时间轴跳变, 旧样本作废 */
            sync.pb_count = 0;
        }

        sync.pb_pair_q16 = pair_q16;
    }

    for (i = SYNC_RATE_WINDOW - 1; i > 0; i--) {
        sync.pb_tick[i] = sync.pb_tick[i - 1];
        sync.pb_pos[i] = sync.pb_pos[i - 1];
    }

    sync.pb_tick[0] = remote_tick;
    sync.pb_pos[0] = remote_pos;

    if (sync.pb_count < SYNC_RATE_WINDOW)
        sync.pb_count++;

    pb_window_recalc();
}

/*
    某个本地 tick 时刻的播放位置。

    误差校正必须用"包到达时刻"的位置，而不是"处理时刻"的位置:
    定时器/任务调度会让处理滞后几毫秒到几十毫秒，
    倍速 2x 时这点滞后就是 2 倍的偏差。
*/
static uint32_t pos_at_tick(
    uint32_t local_tick)
{
    uint32_t elapsed = local_tick - sync.anchor_local_tick;

    return (uint32_t)((sync.anchor_time_us
        + slope_advance_us(elapsed, sync.slope_q16)) / 1000);
}

/*
====================================================
anchor
====================================================
*/

/*
    建立/重建 anchor。

    不重置 rate_ppm:
    它描述的是 A/B 时钟频率差，跟相位无关，
    换 epoch / seek / 恢复播放之后依然有效。
*/
static void establish_anchor(
    uint32_t anchor_time_ms,
    uint32_t local_tick)
{
    /*
        时间轴上限保护:
        A 的时间轴不会超过 total_ms, 内部锚点也不允许超过它,
        否则模型会从"超过上限"的位置起跑, 一直产生 total_ms + N。
        total_ms = 0 表示未知, 不做限制。
    */
    if (sync.total_ms > 0 && anchor_time_ms > sync.total_ms)
        anchor_time_ms = sync.total_ms;

    sync.anchor_time_us = (int64_t)anchor_time_ms * 1000;
    sync.anchor_local_tick = local_tick;
    sync.last_control_tick = local_tick;
    sync.hard_vote = 0;
    sync.hard_dir = 0;
    sync.last_error = 0;
    sync.initialized = 1;
}

/*
    看门狗: 一直有包到达、却一直没有任何观测被采纳 -> 强制自愈

    tick_rewind != 0: 被丢的原因是 tick 回退/重复
        -> A 很可能换了计数器，整段重建(epoch_valid = 0，
           下一个包走 sync_reset_epoch)

    否则: 被丢的原因是观测一直被判过期 -> 链路的实际时延整体变了，
        只重建时延基线就够了，播放位置不用动(模型还在正确前进)。
*/
static void sync_watchdog(
    uint32_t offset,
    uint32_t local_recv_tick,
    uint8_t tick_rewind)
{
    if ((local_recv_tick - sync.last_accept_tick) < SYNC_RESYNC_AFTER_MS)
        return;

    sync.last_accept_tick = local_recv_tick;
    sync.stats.resync++;

    if (tick_rewind) {
        sync.epoch_valid = 0;
    } else {
        delay_window_clear();
        delay_window_push(offset);

        /*
            连续 15 秒没有任何观测被采纳, 说明链路的最佳延迟整体变了,
            旧的历史最佳基准已经没有参考价值, 用当前 offset 重设,
            否则后面每个包都会因为"比历史最佳慢太多"而一直被丢掉。
        */
        delay_baseline_reset(offset);
    }
}

/*
====================================================
初始化
====================================================
*/

void Sync_Init(void)
{
    sync.state = SYNC_STATE_IDLE;
    sync.initialized = 0;
    sync.anchor_local_tick = 0;
    sync.anchor_time_us = 0;
    sync.rate_ppm = 0;
    sync.slope_q16 = 65536;
    sync.last_control_tick = 0;

    delay_window_clear();

    sync.pb_q16 = 65536;
    sync.pb_valid = 0;
    pb_window_reset();

    sync.last_remote_tick = 0;
    sync.last_packet_local_tick = 0;
    sync.epoch_valid = 0;
    sync.epoch_vote = 0;
    sync.hard_vote = 0;
    sync.hard_dir = 0;
    sync.pause_time = 0;

    /* 历史最佳基准还没建立; total_ms 未知时不做时间轴上限裁剪 */
    sync.baseline_delay = 0xFFFFFFFFu;
    sync.congestion = 0;
    sync.total_ms = 0;

    drift_reset(0);

    sync.last_error = 0;
    sync.last_excess = 0;

    sync.last_accept_tick = 0;

    sync.stats.accepted = 0;
    sync.stats.stale = 0;
    sync.stats.reorder = 0;
    sync.stats.epoch_reset = 0;
    sync.stats.rebase = 0;
    sync.stats.resync = 0;
}

/*
====================================================
新 epoch
====================================================

A 重新上线 / 重启 / 换了 tick 计数器，
之前的时延基线全部失效，需要重建。
*/
static void sync_reset_epoch(
    uint32_t offset,
    uint8_t is_playing,
    uint32_t remote_time_ms,
    uint32_t local_recv_tick)
{
    delay_window_clear();
    delay_window_push(offset);

    /*
        历史最佳基准也属于旧 epoch:
        offset 里含 A/B 的 epoch 差, 换了计数器之后旧数值不再有意义,
        所以这里必须用当前 offset 重新种入。
        这不是"丢掉可靠的历史最小延迟": 暂停/恢复/seek 都不走这条路径。
    */
    delay_baseline_reset(offset);

    /* A 的 tick 换了一个 epoch，之前的钟差测量作废 */
    drift_reset(local_recv_tick);

    /* 倍速窗口里的样本 tick 也是旧 epoch 的，同样作废 */
    pb_window_reset();

    sync.epoch_valid = 1;
    sync.epoch_vote = 0;
    sync.last_excess = 0;

    /* 刚重建完，看门狗重新计时 */
    sync.last_accept_tick = local_recv_tick;
    sync.stats.epoch_reset++;

    establish_anchor(remote_time_ms, local_recv_tick);

    if (is_playing) {
        sync.state = SYNC_STATE_RUNNING;
    } else {
        sync.state = SYNC_STATE_PAUSED;
        sync.pause_time = remote_time_ms;
    }
}

/*
====================================================
误差校正
====================================================
*/

static void sync_apply_error(
    uint32_t observed_time,
    uint32_t remote_tick_ms,
    uint32_t local_recv_tick)
{
    int32_t error;
    int32_t abs_error;
    int32_t dir;
    int32_t slew;
    int32_t slew_limit;
    int32_t target_ppm;
    int32_t step;
    uint32_t dt_ms;

    error = wrap_diff(observed_time, pos_at_tick(local_recv_tick));
    sync.last_error = error;

    abs_error = (error < 0) ? -error : error;

    dt_ms = local_recv_tick - sync.last_control_tick;
    if (dt_ms == 0)
        dt_ms = 1;
    if (dt_ms > 10000)
        dt_ms = 10000;
    sync.last_control_tick = local_recv_tick;

    /*
        喂倍速窗口(必须在硬复位判定之前):
        seek / reset 造成的相位跳变由 pb_window_push 里的
        "相邻两段斜率突变"检查处理。
        如果放在硬复位分支之后，一旦倍速没对上、误差大到反复走硬复位，
        窗口就永远喂不进样本，倍速也就永远学不出来(实测踩过这个坑)。
    */
    pb_window_push(remote_tick_ms, observed_time);

    /*
        大误差:
        seek / 时间轴跳变 / 新时间轴
    */
    if (abs_error > SYNC_HARD_RESET_MS) {

        /*
            两种情况不再等确认，直接硬复位:

            1) 误差大到不可能是网络抖动;
            2) 这个包本身准时(excess 在"准时门限"内 = 走的是最快路径)，
               观测可信，误差这么大只能是 seek/reset。
               否则 A 每 10 秒才发一次包时，reset 要等 20 秒才有反应。
        */
        if (abs_error >= SYNC_HARD_RESET_FAST_MS
            || sync.last_excess <= sync.ontime_limit) {

            sync.stats.rebase++;
            establish_anchor(observed_time, local_recv_tick);
            return;
        }

        /*
            否则要连续同方向确认，防止单个被网络
            额外拖延的包把播放位置拉回过去
        */
        dir = (error > 0) ? 1 : -1;

        if (dir == sync.hard_dir) {
            sync.hard_vote++;
        } else {
            sync.hard_vote = 1;
            sync.hard_dir = (int8_t)dir;
        }

        if (sync.hard_vote >= SYNC_HARD_CONFIRM_COUNT) {
            sync.stats.rebase++;
            establish_anchor(observed_time, local_recv_tick);
        }

        return;
    }

    sync.hard_vote = 0;

    /*

        drift_ppm 是 offset = B_tick - A_tick 的变化率:
        它为正说明 B 的数比 A 快，所以 B 的播放位置要走慢一点，
        即 rate 取负。

        这是"直接测量"出来的钟差，不受网络抖动影响，
        而且 A 很久不发包时 B 也能靠它保持跟随(需求 6/10 节)。
    */
    target_ppm = -sync.drift_ppm;

    if (target_ppm > SYNC_RATE_PPM_LIMIT)
        target_ppm = SYNC_RATE_PPM_LIMIT;
    if (target_ppm < -SYNC_RATE_PPM_LIMIT)
        target_ppm = -SYNC_RATE_PPM_LIMIT;

    if (target_ppm != sync.rate_ppm) {
        /* 一阶低通，见 SYNC_RATE_LPF_DIV */
        step = (target_ppm - sync.rate_ppm) / SYNC_RATE_LPF_DIV;

        /*
            整数除法在最后逼近时会变成 0，导致速率停在离目标
            十几个 ppm 的地方(实测 -86ppm vs 目标 -100ppm)。
            这里补一步，让它收敛到目标(之后最多 ±1ppm 抖动)。
        */
        if (step == 0)
            step = (target_ppm > sync.rate_ppm) ? 1 : -1;

        /*
            改钟差修正后必须重算模型斜率,
            sync_update_slope 里会保证播放位置连续
        */
        sync.rate_ppm += step;
        sync_update_slope();
    }

    /*
        相位斜坡:
        把剩余误差(主要是网络抖动造成的相位偏差)平滑地压掉，
        速度上限按经过的时间算，跟发包间隔无关(需求 7 节)。
    */
    if (error > SYNC_PHASE_SLEW_MIN_MS
        || error < -SYNC_PHASE_SLEW_MIN_MS) {

        slew_limit = (int32_t)((int64_t)SYNC_SLEW_MS_PER_S
            * (int64_t)dt_ms / 1000);
        if (slew_limit < 1)
            slew_limit = 1;

        slew = error / SYNC_SLEW_DIV;

        if (slew > slew_limit)
            slew = slew_limit;
        if (slew < -slew_limit)
            slew = -slew_limit;

        sync.anchor_time_us += (int64_t)slew * 1000;
    }
}

/*
====================================================
收到同步包
====================================================
*/

void Sync_OnPacket(
    uint8_t is_playing,
    uint32_t remote_time_ms,
    uint32_t total_ms,
    uint32_t remote_tick_ms)
{
    Sync_OnPacketAt(
        is_playing,
        remote_time_ms,
        total_ms,
        remote_tick_ms,
        HAL_GetTick());
}

void Sync_OnPacketAt(
    uint8_t is_playing,
    uint32_t remote_time_ms,
    uint32_t total_ms,
    uint32_t remote_tick_ms,
    uint32_t local_recv_tick)
{
    uint32_t offset;
    int32_t tick_delta;
    int32_t baseline_delta;
    int32_t excess;

    /*
        时间轴总长度(时间轴上限)。
        0 = A 没给/未知, 保持上一次的有效值, 不做上限裁剪。
    */
    if (total_ms > 0)
        sync.total_ms = total_ms;

    /*
        offset 里包含 A/B 的 epoch 差，量级可能接近 2^32，
        下面只允许做"相减"和"有符号差值"，不做乘加。
    */
    offset = local_recv_tick - remote_tick_ms;

    sync.last_packet_local_tick = local_recv_tick;

    if (sync.epoch_valid) {

        tick_delta = wrap_diff(remote_tick_ms, sync.last_remote_tick);

        /*
            epoch 判据用历史最佳基准(长期量), 不用窗口的 delay_min:
            窗口会被暂停/恢复、看门狗重建, 参考系会瞬时变化,
            而这里问的是"A 的 tick 计数器是不是换了"这个长期结构问题。

            epoch_valid = 1 时 baseline_delay 必然已建立(见 sync_reset_epoch)。
        */
        baseline_delta = wrap_diff(offset, sync.baseline_delay);

        /*
            重复包或者小幅乱序:
            直接丢弃(tick 回绕在 wrap_diff 里天然正确)
        */
        if (tick_delta <= 0 && tick_delta > -SYNC_TICK_REWIND_MS) {
            sync.stats.reorder++;
            sync_watchdog(offset, local_recv_tick, 1);
            return;
        }

        /*
            (B_tick - A_tick) 基线大幅跳变:
            怀疑 A 重新上线 / 重启 / 换了计数器

            单个被网络拖延的包也会看到基线跳变，
            所以要连续确认，避免误判成新 epoch。
        */
        if (baseline_delta > SYNC_EPOCH_STEP_MS
            || baseline_delta < -SYNC_EPOCH_STEP_MS) {
            sync.epoch_vote++;
            if (sync.epoch_vote >= SYNC_EPOCH_CONFIRM_COUNT)
                sync.epoch_valid = 0;
        } else {
            sync.epoch_vote = 0;
        }

        /*
            tick 大幅回退但新 epoch 还没确认: 先丢掉
        */
        if (sync.epoch_valid && tick_delta <= 0) {
            sync.stats.reorder++;
            sync_watchdog(offset, local_recv_tick, 1);
            return;
        }
    }

    sync.last_remote_tick = remote_tick_ms;

    /*
        首个包或者新 epoch:
        重建时延基线并直接建立 anchor
    */
    if (!sync.epoch_valid) {
        sync_reset_epoch(
            offset,
            is_playing,
            remote_time_ms,
            local_recv_tick);
        return;
    }

    /*
        用旧的时延基线判断这个包是不是被额外拖延
        (此时还没有把自己放进窗口，避免污染基线)
    */
    excess = wrap_diff(offset, sync.delay_min);
    if (excess < 0)
        excess = 0;
    sync.last_excess = excess;

    /*
        拥塞程度(诊断): 相对历史最佳基准多花的时间。

        与 excess 的区别:
          excess     = 相对"当前窗口的最快路径", 用于判过期观测 / 判本包可信
          congestion = 相对"历史最佳", 反映当前网络拥塞/排队有多严重

        过期判定继续用 excess(窗口): 链路时延整体上移时窗口最小值会跟着上移,
        只有真正的离群积压包才会被拒, 可用性更稳。
    */
    sync.congestion = wrap_diff(offset, sync.baseline_delay);
    if (sync.congestion < 0)
        sync.congestion = 0;

    /*
        暂停:
        时间轴不增长，包里携带的就是权威位置
    */
    if (!is_playing) {
        sync.state = SYNC_STATE_PAUSED;
        sync.pause_time = remote_time_ms;
        sync.initialized = 1;
        sync.hard_vote = 0;

        /* 暂停是被正常处理的状态，看门狗不用计时 */
        sync.last_accept_tick = local_recv_tick;
        return;
    }

    /*
        由暂停/未开始转为运行:
        立即按 A 的位置开始播放
        (A 已经在包里补偿过传输时间，直接信任)
    */
    if (sync.state != SYNC_STATE_RUNNING) {
        sync.state = SYNC_STATE_RUNNING;

        /*
            暂停期间 A 的 tick 可能已经不连续，重建时延基线;
            暂停期间时间轴不前进，倍速窗口也要清掉(否则斜率会被算小)。
        */
        delay_window_clear();
        delay_window_push(offset);
        pb_window_reset();

        /*
            历史最佳基准跨暂停保留, 这里只允许更快的包把它降下来。
            窗口仍然重建: 它只表示"当前统计", 恢复后那段临时更高的延迟
            不该被当成过期观测全部丢掉。
        */
        delay_baseline_update(offset);

        establish_anchor(remote_time_ms, local_recv_tick);

        sync.last_accept_tick = local_recv_tick;
        return;
    }

    /*
        过期观测:
        被网络额外拖延的包，它携带的位置相对现在已经过时

        既不移动播放位置(否则会把位置拉回过去)，
        也不放进时延窗口(否则一大批积压旧包会把最快路径基线
        带偏，后续正常包就再也判断不出自己是否被拖延)

        代价:
        如果网络上实际时延整体永久变大，所有包都会被判为过期，
        B 退化成完全靠本地时钟预测(需求 5 节允许的行为)，不会跑飞。
        此时把 SYNC_EXCESS_MIN_MS / SYNC_EXCESS_MAX_MS 调大即可。
    */
    if (excess > sync.excess_limit) {
        sync.stats.stale++;
        sync_watchdog(offset, local_recv_tick, 0);
        return;
    }

    delay_window_push(offset);
    drift_update(offset, local_recv_tick);
    delay_baseline_update(offset);

    sync.stats.accepted++;
    sync.last_accept_tick = local_recv_tick;

    sync_apply_error(remote_time_ms, remote_tick_ms, local_recv_tick);
}

/*
====================================================
查询
====================================================
*/

/*
    当前播放位置:
    anchor_position + 本地经过时间 * (1 + rate_ppm)
*/
uint32_t Sync_GetTime(void)
{
    uint32_t time_ms;

    if (!sync.initialized)
        return 0;

    /*
        暂停时时间轴不增长，直接返回暂停位置
    */
    if (sync.state != SYNC_STATE_RUNNING)
        time_ms = sync.pause_time;
    else
        time_ms = pos_at_tick(HAL_GetTick());

    /*
        时间轴上限: 预测位置绝对不能超过 A 给的 total_ms
        (total_ms = 0 表示未知, 不限制)。

        只在"对外输出"这里裁剪:
        pos_at_tick() 本身不裁 —— sync_apply_error() 里
        error = 观测位置 - 模型位置 必须用未裁剪的模型值,
        否则误差会被伪造成上千 ms, 触发假硬复位。
    */
    if (sync.total_ms > 0 && time_ms > sync.total_ms)
        time_ms = sync.total_ms;

    return time_ms;
}

uint32_t Sync_GetLastPacketTick(void)
{
    return sync.last_packet_local_tick;
}

uint32_t Sync_GetDelayEstimate(void)
{
    return sync.delay_min;
}

/*
    历史最佳 (B_tick - A_tick)
    0xFFFFFFFF = 尚未建立
*/
uint32_t Sync_GetBaselineDelay(void)
{
    return sync.baseline_delay;
}

/*
    最近一个包相对历史最佳基准多花的毫秒数(拥塞程度), 纯诊断用
*/
int32_t Sync_GetCongestion(void)
{
    return sync.congestion;
}

/*
    A 给的时间轴总长度, 0 = 未知
*/
uint32_t Sync_GetTotalMs(void)
{
    return sync.total_ms;
}

int32_t Sync_GetLastExcess(void)
{
    return sync.last_excess;
}

int32_t Sync_GetRatePpm(void)
{
    return sync.rate_ppm;
}

int32_t Sync_GetPlaybackRateX1000(void)
{
    return (int32_t)((int64_t)sync.pb_q16 * 1000 / 65536);
}

uint8_t Sync_GetRateSamples(void)
{
    return sync.pb_count;
}

int32_t Sync_GetSlopePpm(void)
{
    return (int32_t)(((int64_t)sync.slope_q16 - 65536) * 1000000 / 65536);
}

void Sync_GetStats(SyncStats* stats)
{
    if (stats == 0)
        return;

    *stats = sync.stats;
}

int32_t Sync_GetLastError(void)
{
    return sync.last_error;
}

uint8_t Sync_IsLocked(void)
{
    if (!sync.initialized)
        return 0;

    return (sync.state == SYNC_STATE_RUNNING) ? 1 : 0;
}
