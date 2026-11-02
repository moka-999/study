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
    return idd_stats_feed_groups(stats, begin_ms, end_ms, sample,
                                MEMORY_MONITOR_VALID_VOLTAGE | MEMORY_MONITOR_VALID_IDD);
}

int idd_stats_feed_groups(idd_stats_t *stats, uint32_t begin_ms, uint32_t end_ms,
                          const memory_monitor_sample_t *sample, uint8_t attempted_mask)
{
    const uint8_t groups = MEMORY_MONITOR_VALID_VOLTAGE | MEMORY_MONITOR_VALID_IDD;
    if (!stats || !sample || (attempted_mask & (uint8_t)~groups) != 0U) return IF_ERR;
    if (stats->state != IDD_STATS_RUNNING || attempted_mask == 0U) return 0;
    uint32_t begin = begin_ms - stats->started_ms;
    uint32_t end = end_ms - stats->started_ms;
    if (begin >= stats->duration_ms || end >= stats->duration_ms || end < begin) {
        if (stats->discarded != UINT32_MAX) ++stats->discarded;
        idd_stats_poll(stats, end_ms);
        return 0;
    }
    uint8_t fresh = attempted_mask;
    if ((stats->seen_groups & MEMORY_MONITOR_VALID_VOLTAGE) != 0U && end <= stats->voltage_offset_ms)
        fresh &= (uint8_t)~MEMORY_MONITOR_VALID_VOLTAGE;
    if ((stats->seen_groups & MEMORY_MONITOR_VALID_IDD) != 0U && end <= stats->current_offset_ms)
        fresh &= (uint8_t)~MEMORY_MONITOR_VALID_IDD;
    /* 一次调用可含新组和重复组，只接纳新组；丢弃计数每次调用至多增加一次。 */
    if (fresh != attempted_mask && stats->discarded != UINT32_MAX) ++stats->discarded;
    if (fresh == 0U) return 0;
    ++stats->attempts;
    if (end > stats->last_offset_ms) stats->last_offset_ms = end;
    stats->seen_groups |= fresh;
    if ((fresh & MEMORY_MONITOR_VALID_VOLTAGE) != 0U) stats->voltage_offset_ms = end;
    if ((fresh & MEMORY_MONITOR_VALID_IDD) != 0U) stats->current_offset_ms = end;
    for (uint8_t i = 0U; i < MEMORY_MONITOR_RAIL_COUNT; ++i) {
        if ((fresh & MEMORY_MONITOR_VALID_VOLTAGE) != 0U) {
            int valid = sample->valid_mask & MEMORY_MONITOR_VALID_VOLTAGE;
            /* 无效数组可能尚未填写，连函数实参求值也不读取它们。 */
            metric_add(&stats->voltage[i], valid ? sample->voltage_mv[i] : 0, valid);
        }
        if ((fresh & MEMORY_MONITOR_VALID_IDD) != 0U) {
            int valid = (sample->valid_mask & MEMORY_MONITOR_VALID_IDD) != 0U &&
                        sample->idd[i].current_ua <= UINT32_MAX;
            metric_add(&stats->current[i], valid ? (int64_t)sample->idd[i].current_ua : 0, valid);
        }
    }
    return 1;
}

void idd_stats_cancel(idd_stats_t *stats)
{
    if (stats && stats->state == IDD_STATS_RUNNING) stats->state = IDD_STATS_CANCELLED;
}
