#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
====================================================
外部HAL
====================================================
*/

uint32_t HAL_GetTick(void);

/*
====================================================
参数
====================================================

A 端(上位机)会自己测量网络延迟，并把补偿量直接加进
同步包携带的时间轴位置里。

所以 B 端把 current_ms 视为:
    "B 收到这个包的时刻，A 的时间轴位置"
直接信任，不再叠加任何延迟补偿。
*/

/*
    时延窗口样本数
    A 端可能很久才发一次同步包(10 秒甚至更长)，
    窗口太大的话要很久才能填满，所以取小一点。
*/
#define SYNC_DELAY_WINDOW_SIZE 8

/*
    硬复位阈值(ms)
    误差超过它认为发生了 seek / 时间轴跳变 / 新时间轴。
    取需求文档 8 节的示例值 1000。
*/
#define SYNC_HARD_RESET_MS 1000

/*
    误差超过这个值(ms)时不再等确认，直接硬复位。
    这么大的误差不可能是网络抖动。
*/
#define SYNC_HARD_RESET_FAST_MS 10000

/*
    硬复位需要连续同方向确认的包数。
    防止单个被网络额外拖延的包把播放位置拉回过去(需求 4 节)。
*/
#define SYNC_HARD_CONFIRM_COUNT 2

/*
    过期观测判定

    超额延迟 = 该包的 (B_tick - A_tick) 比窗口内最快路径大出的毫秒数。

    超过门限的包认为被网络额外拖延了(例如阻塞后积压的旧包)，
    它携带的时间轴位置已经过时:

        limit = max(SYNC_EXCESS_MIN_MS,
                    SYNC_EXCESS_FACTOR * 窗口平均超额)
        limit = min(limit, SYNC_EXCESS_MAX_MS)

    门限跟着链路实际抖动走:
    抖动小的链路会拒绝更大的异常延迟，抖动大的链路不会误杀正常包。

    这种包:
      1. 不能用来移动播放位置(否则会把位置拉回过去，需求 4 节)
      2. 也不能放进时延窗口(否则一大批积压旧包会把最快路径基线
         带偏，后续正常包就再也判断不出自己是否被拖延)
*/
/*
    两个"基线"的分工(不要混用):

        delay_min      当前时延窗口内的最快路径。随窗口滚动,
                       暂停/恢复、epoch 重建、看门狗都会重建它。
                       用途: 算 excess(判过期观测 / 判本包是否准时可信)。

        baseline_delay 历史最佳 (B_tick - A_tick)。只降不升,
                       跨暂停/恢复/普通 seek 都保留。
                       用途: 判 (B_tick - A_tick) 是否 epoch 级跳变;
                             算 congestion = offset - baseline_delay(诊断)。

    为什么过期判定用窗口最小而不是历史最佳:
    链路时延整体上移时窗口最小会跟着上移, 只拒绝真正的离群积压包,
    可用性更稳; 而"包携带的位置是否已过期"由误差处理(相位斜坡/硬复位)兜住。
*/
#define SYNC_EXCESS_MIN_MS 100
#define SYNC_EXCESS_FACTOR 4
#define SYNC_EXCESS_MAX_MS 1000

/*
    相位斜坡的启用门限(ms)

    小于它的误差不动相位(位置始终连续, 不会有任何跳变)，
    大于它说明确实偏了(seek / 阻塞后追上 / 抖动尖峰)，
    用相位斜坡快速拉回。
*/
#define SYNC_PHASE_SLEW_MIN_MS 20

/*
    相位斜坡速度上限(ms/s)
    误差通过缓慢移动 anchor 平滑消除，绝不直接跳变(需求 7 节)
*/
#define SYNC_SLEW_MS_PER_S 200

/*
    相位斜坡比例: 每包最多移动误差的 1/SYNC_SLEW_DIV
*/
#define SYNC_SLEW_DIV 8

/*
    速率的一阶低通系数
    每个包只允许把速率朝目标推近 1/N，让速率变化平滑
*/
#define SYNC_RATE_LPF_DIV 32

/*
    A/B 时钟频率差(钟差)测量

    offset = B_tick - A_tick，它对时间的斜率就是两个时钟的频率差。
    直接做斜率会被网络抖动淹没，所以每段取最小值(最快路径)，
    再比较首尾两段的最小值:

        钟差(ppm) = (最新段最小 offset - 最旧段最小 offset)
                    * 1e6 / 两段之间的时间(ms)

    这样测出来的钟差是"直接测量值"，不受网络抖动影响，
    也不需要靠相位矫正的结果去反推。
*/
#define SYNC_DRIFT_SEGMENT_MS 10000   /* 每段 10 秒 */
#define SYNC_DRIFT_SEGMENTS 12        /* 保存 12 段 = 120 秒 */
#define SYNC_DRIFT_MIN_SEGMENTS 4     /* 至少 4 段才开始输出 */
#define SYNC_DRIFT_PPM_LIMIT 2000     /* 超过它认为测量被污染，丢弃重测 */

/*
    速率修正上限(ppm)

    A/B 晶振频率差是有限量: 一般几十 ppm，最差也就几百 ppm。
    这里给 ±1000ppm 的余量就够了。
    限得太大会让速率环在大误差时冲到限幅，之后回不来(过冲)。
*/
#define SYNC_RATE_PPM_LIMIT 1000

/*
    倍速学习(时间轴速率)

    只用 A 自己包里的两个量:

        倍速 = Δcurrent_ms / Δremote_tick_ms

    两者都来自 A，跟 B 的时钟和网络抖动无关。
    记录最近 SYNC_RATE_WINDOW 个包，用首尾两点的跨度算斜率。

    只喂"已经锁定(误差小)"的包:
    这样 seek / reset 造成的相位跳变不会污染倍速估计。
*/
#define SYNC_RATE_WINDOW 8            /* 记录最近 8 个包 */
#define SYNC_RATE_MIN_SAMPLES 4       /* 至少 4 个样本才出结果 */
#define SYNC_RATE_MIN_SPAN_MS 400     /* 首尾跨度至少 400ms 才够精度 */
#define SYNC_RATE_SAMPLE_MS 2000      /* 窗口采样间隔(不每个包都进窗口) */
#define SYNC_PB_LPF_DIV 8             /* 倍速的一阶低通 */
#define SYNC_PB_STEP_DIV 4            /* 单次倍速变化不超过 25% */

/*
    支持的倍速范围

    模型斜率 = 倍速 × 钟差修正，用 Q16.16 表示(65536 = 1.0x)

    范围外的倍速靠相位斜坡(200ms/s)追不上，会变成反复硬复位。
*/
#define SYNC_SLOPE_MIN_Q16 16384      /* 0.25x */
#define SYNC_SLOPE_MAX_Q16 262144     /* 4.0x */

/*
    A 端 tick 小幅回退(ms)
    认为只是重复包或者乱序包，直接丢弃。
*/
#define SYNC_TICK_REWIND_MS 1000

/*
    (B_tick - A_tick) 基线跳变超过这个值(ms)
    认为 A 重新上线/重启/换了计数器，即进入了新的 epoch。
*/
#define SYNC_EPOCH_STEP_MS 5000

/*
    进入新 epoch 需要连续确认的包数
*/
#define SYNC_EPOCH_CONFIRM_COUNT 2

/*
    看门狗(ms)

    一直有包到达、却一直没有任何观测被采纳超过这么长时间，
    说明进了死角(链路时延整体变了 / A 换了 tick 计数器)，
    强制自愈一次，避免永久丢包。

    取 15s: 要明显长于"网络阻塞后突发积压"的长度，
    否则会把突发旧包当成新基线采纳。
*/
#define SYNC_RESYNC_AFTER_MS 15000

/*
====================================================
API
====================================================
*/

void Sync_Init(void);

/*
    收到同步包。
    is_playing    : A 端时间轴是否在运行
    current_ms    : A 端时间轴位置(A 已补偿传输时间)
    total_ms      : A 的时间轴总长度(0 = 未知, 不做上限限制)
    remote_tick_ms: A 端本地 tick(ms)

    协议要求:
    暂停(is_playing = 0)时，包里携带的应该是权威的(已经冻结的)
    时间轴位置，不要加传输补偿 — 时间轴已经不增长，加了反而让 B 偏前。
*/
void Sync_OnPacket(
    uint8_t is_playing,
    uint32_t remote_time_ms,
    uint32_t total_ms,
    uint32_t remote_tick_ms);

/*
    带本地接收时刻的版本。
    用包真正到达的 tick，而不是处理时刻，
    可以消除定时器量化误差。
*/
void Sync_OnPacketAt(
    uint8_t is_playing,
    uint32_t remote_time_ms,
    uint32_t total_ms,
    uint32_t remote_tick_ms,
    uint32_t local_recv_tick);

/*
    B 端当前播放位置(ms)
*/
uint32_t Sync_GetTime(void);

/*
    最近一次同步包的本地接收时刻
    用于显示"距上次同步过了多久"(A 可以很久不发包，不做失联处理)
*/
uint32_t Sync_GetLastPacketTick(void);

/*
    时延窗口最小 (B_tick - A_tick) = 当前窗口的最快路径，纯诊断用
    (会被暂停/恢复、epoch 重建、看门狗重建; 长期基准见 Sync_GetBaselineDelay)
*/
uint32_t Sync_GetDelayEstimate(void);

/*
    历史最佳 (B_tick - A_tick): 跨暂停/恢复/普通 seek 都保留的长期基准
    0xFFFFFFFF = 尚未建立
*/
uint32_t Sync_GetBaselineDelay(void);

/*
    最近一个包相对历史最佳基准多花的毫秒数(拥塞程度)，纯诊断用
    (过期观测判定仍然用相对“当前最快路径”的 excess, 见 Sync_GetLastExcess)
*/
int32_t Sync_GetCongestion(void);

/*
    A 给的时间轴总长度(ms)，0 = 未知
*/
uint32_t Sync_GetTotalMs(void);

/*
    最近一个包相对最快路径多花的毫秒数，纯诊断用
*/
int32_t Sync_GetLastExcess(void);

/*
    当前速率修正(ppm)
    >0 表示 B 的播放位置推进得比本地时钟快
    (这只是 A/B 钟差修正部分，不含倍速)
*/
int32_t Sync_GetRatePpm(void);

/*
    学到的倍速 × 1000
    1000 = 1.000x, 2000 = 2.000x
*/
int32_t Sync_GetPlaybackRateX1000(void);

/*
    倍速窗口里已有的样本数(达到 SYNC_RATE_MIN_SAMPLES 才算锁定)
*/
uint8_t Sync_GetRateSamples(void);

/*
    模型斜率(ppm，相对 1.0)，= 倍速 × 钟差修正
*/
int32_t Sync_GetSlopePpm(void);

/*
    统计(诊断用): 为什么没反应，看这几个计数就清楚
        accepted    被采纳的观测数
        stale       因为"被网络拖延过"而丢弃的包数
        reorder     因为重复/乱序(tick 回退)而丢弃的包数
        epoch_reset 判定 A 换 tick epoch 而整段重建的次数
        rebase      硬复位(seek/reset)次数
        resync      看门狗强制自愈次数
*/
typedef struct
{
    uint32_t accepted;
    uint32_t stale;
    uint32_t reorder;
    uint32_t epoch_reset;
    uint32_t rebase;
    uint32_t resync;

} SyncStats;

void Sync_GetStats(SyncStats* stats);

/*
    最近一次误差(ms) = A 位置 - B 预测位置
*/
int32_t Sync_GetLastError(void);

/*
    是否已经建立同步(收到过 playing 包)
*/
uint8_t Sync_IsLocked(void);

#ifdef __cplusplus
}
#endif
