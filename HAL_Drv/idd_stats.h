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
    int64_t sum;          /* 累加器；电压输入int32_t，电流输入uint32_t，不截断电流。 */
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
    uint32_t attempts;       /* 每个完整采样尝试最多计一次，含部分/全部无效。 */
    uint32_t discarded;      /* 旧、重复、逆序或跨边界快照，饱和计数。 */
    uint32_t last_offset_ms; /* 最近纳入快照的完成时间相对窗口起点的偏移。 */
    idd_metric_stats_t voltage[MEMORY_MONITOR_RAIL_COUNT]; /* mV，顺序VDD1/VDD2/VDDQ。 */
    idd_metric_stats_t current[MEMORY_MONITOR_RAIL_COUNT]; /* mA，同上；未valid时0不是实测。 */
} idd_stats_t;

/** 清为IDLE；NULL不操作。调用方串行访问，任何API均不分配内存或访问硬件。 */
void idd_stats_reset(idd_stats_t *stats);

/**
 * 创建新窗口并清空旧结果：IF_OK成功；IF_ERR参数无效/已有运行窗口且保持原值。
 * duration_ms为1..INT32_MAX，minimum_samples为1..duration_ms；每毫秒最多接纳一次。
 * now_ms为同一单调uint32_t毫秒时基，允许回绕；必须在半个回绕周期内poll/feed。
 * 此参数不是已批准的JEDEC IDD窗口或传感器实际采样率。
 */
int idd_stats_begin(idd_stats_t *stats, uint32_t now_ms,
                    uint32_t duration_ms, uint32_t minimum_samples);

/**
 * 纳入一次新采样：1接纳，0未运行/时间不合规，IF_ERR为空指针。
 * begin/end为整次物理读取开始/完成时间；必须全部落在[start,start+duration)内。
 * 完成时间不递增、早于窗口或跨边界的快照不计算；仅按valid_mask分组处理，
 * voltage/current无效独立计失败，不将无效数组的0作为样本。TPS状态不参与统计。
 */
int idd_stats_feed(idd_stats_t *stats, uint32_t begin_ms, uint32_t end_ms,
                   const memory_monitor_sample_t *sample);

/** 到期后进入COMPLETE/TIMEOUT，之后结果冻结；NULL/非运行状态不操作。 */
void idd_stats_poll(idd_stats_t *stats, uint32_t now_ms);
/** 提前结束运行窗口并冻结统计；样本达标为COMPLETE，否则为TIMEOUT，不判断算法结果。 */
void idd_stats_finish(idd_stats_t *stats);
/** 仅运行态转CANCELLED，不清空结果；NULL/终态不操作。 */
void idd_stats_cancel(idd_stats_t *stats);

#endif
