#include "idd_stats.h"
#include <limits.h>
#include <string.h>

void idd_stats_reset(idd_stats_t *stats)
{
    if (stats) memset(stats, 0, sizeof(*stats));
}

int idd_stats_begin(idd_stats_t *stats, uint32_t now_ms,
                    uint32_t duration_ms, uint32_t minimum_samples)
{
    if (!stats || stats->state == IDD_STATS_RUNNING || duration_ms == 0U ||
        duration_ms > INT32_MAX || minimum_samples == 0U ||
        minimum_samples > duration_ms) return IF_ERR;
    idd_stats_reset(stats);
    stats->started_ms = now_ms;
    stats->duration_ms = duration_ms;
    stats->minimum_samples = minimum_samples;
    stats->state = IDD_STATS_RUNNING;
    return IF_OK;
}

void idd_stats_poll(idd_stats_t *stats, uint32_t now_ms)
{
    if (!stats || stats->state != IDD_STATS_RUNNING) return;
    uint32_t elapsed = now_ms - stats->started_ms;
    /* 半周期之外视为旧时间，不能拿缓存快照的旧时间让新窗口提前结束。 */
    if (elapsed > INT32_MAX || elapsed < stats->duration_ms) return;
    idd_stats_finish(stats);
}

void idd_stats_finish(idd_stats_t *stats)
{
    if (!stats || stats->state != IDD_STATS_RUNNING) return;
    stats->state = IDD_STATS_COMPLETE;
    for (uint8_t i = 0U; i < MEMORY_MONITOR_RAIL_COUNT; ++i) {
        if (stats->voltage[i].valid_count < stats->minimum_samples ||
            stats->current[i].valid_count < stats->minimum_samples)
            stats->state = IDD_STATS_TIMEOUT;
    }
}

static void metric_add(idd_metric_stats_t *metric, int64_t value, int valid)
{
    if (!valid) {
        ++metric->failed_count;
        return;
    }
    if (metric->valid_count == 0U || value < metric->minimum) metric->minimum = value;
    if (metric->valid_count == 0U || value > metric->maximum) metric->maximum = value;
    /* 时间戳严格递增且窗口<=INT32_MAX毫秒，所以样本数<=INT32_MAX。
     * 即使电流每次为UINT32_MAX，乘积仍小于INT64_MAX，不会累加溢出。 */
    metric->sum += value;
    ++metric->valid_count;
    metric->mean = metric->sum / metric->valid_count;
}

int idd_stats_feed(idd_stats_t *stats, uint32_t begin_ms, uint32_t end_ms,
                   const memory_monitor_sample_t *sample)
{
    if (!stats || !sample) return IF_ERR;
    if (stats->state != IDD_STATS_RUNNING) return 0;
    uint32_t begin = begin_ms - stats->started_ms;
    uint32_t end = end_ms - stats->started_ms;
    if (begin >= stats->duration_ms || end >= stats->duration_ms || end < begin ||
        (stats->attempts != 0U && end <= stats->last_offset_ms)) {
        if (stats->discarded != UINT32_MAX) ++stats->discarded;
        idd_stats_poll(stats, end_ms);
        return 0;
    }
    ++stats->attempts;
    stats->last_offset_ms = end;
    for (uint8_t i = 0U; i < MEMORY_MONITOR_RAIL_COUNT; ++i) {
        int voltage_valid = sample->valid_mask & MEMORY_MONITOR_VALID_VOLTAGE;
        int current_valid = sample->valid_mask & MEMORY_MONITOR_VALID_IDD;
        /* 无效数组可能尚未填写，连函数实参求值也不读取它们。 */
        metric_add(&stats->voltage[i], voltage_valid ? sample->voltage_mv[i] : 0,
                   voltage_valid);
        metric_add(&stats->current[i], current_valid ? sample->idd[i].current_ma : 0,
                   current_valid);
    }
    return 1;
}

void idd_stats_cancel(idd_stats_t *stats)
{
    if (stats && stats->state == IDD_STATS_RUNNING) stats->state = IDD_STATS_CANCELLED;
}
