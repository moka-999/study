/* 三轨测量窗口的纯统计逻辑：不操作器件，不解释算法，不判断芯片PASS/FAIL。 */
#ifndef IDD_STATS_H
#define IDD_STATS_H

#include "memory_monitor.h"

typedef enum {
    IDD_STATS_IDLE = 0,
    IDD_STATS_RUNNING,
    IDD_STATS_COMPLETE,   /* 到达窗口末端，六组电压/电流均达到最低有效样本数。 */
    IDD_STATS_TIMEOUT,    /* 到达窗口末端，但至少一组有效样本不足。 */
    IDD_STATS_CANCELLED,  /* 调用方取消、下电或重新调压；保留已采集的部分统计。 */
} idd_stats_state_t;

typedef struct {
    int64_t sum;          /* 累加器；电压为int32_t mV，电流为0..UINT32_MAX µA。 */
    int64_t mean;         /* 样本算术均值，整数除法向零截断；不是时间加权均值。 */
    int64_t minimum;
    int64_t maximum;      /* 仅为样点最大值，不能当作未采到的瞬态尖峰。 */
    uint32_t valid_count;
    uint32_t failed_count;/* 本窗口已纳入的采样尝试中，此类测量无效的次数。 */
} idd_metric_stats_t;

typedef struct idd_stats {
    idd_stats_state_t state;
    uint32_t started_ms;
    uint32_t duration_ms;
    uint32_t minimum_samples;
    uint32_t attempts;       /* 至少一组新采样获接纳的feed次数，含部分/全部无效。 */
    uint32_t discarded;      /* 旧、重复、逆序或跨边界快照，饱和计数。 */
    uint32_t last_offset_ms; /* 最近纳入快照的完成时间相对窗口起点的偏移。 */
    uint32_t voltage_offset_ms; /* 最近纳入电压组的完成时间偏移，与电流组独立。 */
    uint32_t current_offset_ms; /* 最近纳入电流组的完成时间偏移。 */
    uint8_t seen_groups;     /* 已尝试组的VALID_VOLTAGE/VALID_IDD位，区分首次的0ms。 */
    idd_metric_stats_t voltage[MEMORY_MONITOR_RAIL_COUNT]; /* mV，顺序VDD1/VDD2/VDDQ。 */
    idd_metric_stats_t current[MEMORY_MONITOR_RAIL_COUNT]; /* µA，同上；上报时再换算mA。 */
} idd_stats_t;

/** 清为IDLE；NULL不操作。调用方串行访问，任何API均不分配内存或访问硬件。 */
void idd_stats_reset(idd_stats_t *stats);

/**
 * 创建新窗口并清空旧结果：IF_OK成功；IF_ERR参数无效/已有运行窗口且保持原值。
 * duration_ms为1..INT32_MAX，minimum_samples为1..duration_ms；每组每毫秒最多接纳一次。
 * now_ms为同一单调uint32_t毫秒时基，允许回绕；必须在半个回绕周期内poll/feed。
 * 此参数不是已批准的JEDEC IDD窗口或传感器实际采样率。
 */
int idd_stats_begin(idd_stats_t *stats, uint32_t now_ms,
                    uint32_t duration_ms, uint32_t minimum_samples);

/**
 * 纳入一次新采样：1接纳，0未运行/时间不合规，IF_ERR为空指针。
 * begin/end为整次物理读取开始/完成时间；必须全部落在[start,start+duration)内。
 * 兼容入口，等价于feed_groups同时尝试电压和电流两组。分时采样请使用下方接口，
 * 避免把尚未采集的另一组缓存再次纳入统计。TPS状态不参与统计。
 */
int idd_stats_feed(idd_stats_t *stats, uint32_t begin_ms, uint32_t end_ms,
                   const memory_monitor_sample_t *sample);

/**
 * 纳入本次实际尝试的采样组。attempted_mask只允许VALID_VOLTAGE/VALID_IDD组合。
 * 1表示至少一组新采样已接纳；0表示空组、非运行或没有新组；IF_ERR为参数非法。
 * begin/end必须全部在窗口内；每组完成时间独立递增，同毫秒可各纳入一组。
 * 未尝试组不读取、不计有效或失败；已尝试但valid_mask无对应位则计失败。
 * 电流读取current_ua；单轨超过UINT32_MAX µA计该轨失败，不截断或绕回。
 */
int idd_stats_feed_groups(idd_stats_t *stats, uint32_t begin_ms, uint32_t end_ms,
                          const memory_monitor_sample_t *sample, uint8_t attempted_mask);

/** 到期后进入COMPLETE/TIMEOUT，之后结果冻结；NULL/非运行状态不操作。 */
void idd_stats_poll(idd_stats_t *stats, uint32_t now_ms);
/** 提前结束运行窗口并冻结统计；样本达标为COMPLETE，否则为TIMEOUT，不判断算法结果。 */
void idd_stats_finish(idd_stats_t *stats);
/** 仅运行态转CANCELLED，不清空结果；NULL/终态不操作。 */
void idd_stats_cancel(idd_stats_t *stats);

#endif
