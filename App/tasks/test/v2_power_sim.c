#include "v2_power_sim.h"
#include <limits.h>
#include <string.h>

/* 只模拟软件可观察的配置与数据，不代表硬件启动时序、训练电压范围或IDD合规性。 */
static pwr_ctrl_state_t s_state = PWR_CTRL_STATE_OFF;
static ddr_memory_type_t s_type = DDR_MEMORY_LPDDR4X;
static uint16_t s_mv[3] = {1800U, 1100U, 600U};
static uint16_t s_training_mv[3];
static ddr_memory_type_t s_training_type;
static boot_mode_t s_boot, s_training_boot;
static uint8_t s_boot_valid, s_training_valid;
static uint8_t s_training_slt; /* 不把SLT虚拟准备缓存复用成FUNC启动已确认。 */
static uint32_t s_cycles, s_sample_index;
static monitor_snapshot_t s_latest;
static idd_stats_t s_stats;

static void sample_now(uint32_t now)
{
    static const int32_t wave[] = {-1, 0, 1, 0};
    static const uint32_t current_base[] = {300U, 140U, 85U};
    static const uint32_t current_swing[] = {30U, 15U, 10U};
    int32_t step = wave[s_sample_index++ % 4U];
    memset(&s_latest, 0, sizeof(s_latest));
    s_latest.timestamp_ms = now;
    s_latest.power_state = s_state;
    if (s_state != PWR_CTRL_STATE_ON) return;
    s_latest.memory.valid_mask = MEMORY_MONITOR_VALID_VOLTAGE | MEMORY_MONITOR_VALID_IDD;
    for (unsigned rail = 0U; rail < 3U; ++rail) {
        s_latest.memory.voltage_mv[rail] = (int32_t)s_mv[rail] + step * 10;
        s_latest.memory.idd[rail].current_ma = (uint32_t)((int32_t)current_base[rail] + step * (int32_t)current_swing[rail]);
        s_latest.memory.idd[rail].power_mw =
            (uint32_t)s_latest.memory.voltage_mv[rail] * s_latest.memory.idd[rail].current_ma / 1000U;
    }
    (void)idd_stats_feed(&s_stats, now, now, &s_latest.memory);
}

void v2_power_sim_poll(void)
{
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - s_latest.timestamp_ms) >= 500U) sample_now(now);
    idd_stats_poll(&s_stats, now);
}

pwr_ctrl_state_t v2_power_sim_state(void) { return s_state; }
uint32_t v2_power_sim_cycles(void) { return s_cycles; }

pwr_ctrl_result_t v2_power_sim_on(uint32_t now_ms)
{
    if (s_state != PWR_CTRL_STATE_ON) {
        ++s_cycles;
        s_state = PWR_CTRL_STATE_ON;
        sample_now(now_ms);
    }
    return PWR_CTRL_OK;
}

void v2_power_sim_off(void)
{
    idd_stats_cancel(&s_stats);
    s_state = PWR_CTRL_STATE_OFF;
    sample_now(HAL_GetTick());
}

void v2_power_sim_restart(void)
{
    ++s_cycles; /* 虚拟冷重启立即完成，不声称已取得真实RK安全下电/ready确认。 */
    sample_now(HAL_GetTick());
}

monitor_boot_result_t v2_power_sim_set_boot(boot_mode_t mode)
{
    uint16_t mv;
    if (mcp4725_boot_mode_target_mv(mode, &mv) != IF_OK) return MONITOR_BOOT_ERR_PARAM;
    if (s_state != PWR_CTRL_STATE_OFF) return MONITOR_BOOT_ERR_STATE;
    s_boot = mode;
    s_boot_valid = 1U;
    return MONITOR_BOOT_OK;
}

monitor_boot_result_t v2_power_sim_get_boot(boot_mode_t *mode, uint16_t *mv)
{
    if (!mode || !mv) return MONITOR_BOOT_ERR_PARAM;
    if (!s_boot_valid) return MONITOR_BOOT_ERR_NOT_SET;
    *mode = s_boot;
    return mcp4725_boot_mode_target_mv(s_boot, mv) == IF_OK ? MONITOR_BOOT_OK : MONITOR_BOOT_ERR_IO;
}

pwr_ctrl_result_t v2_power_sim_set_ddr(ddr_memory_type_t type, const int16_t steps[3])
{
    uint16_t target[3];
    if (ddr_power_build_step_targets(type, steps, target) != IF_OK) return PWR_CTRL_ERR_PARAM;
    if ((s_state == PWR_CTRL_STATE_OFF && (steps[0] || steps[1] || steps[2])) ||
        (s_state == PWR_CTRL_STATE_ON && type != s_type)) return PWR_CTRL_ERR_BUSY;
    s_type = type;
    memcpy(s_mv, target, sizeof(s_mv));
    /* 运行拉偏不覆盖最近的训练目标；下一次0103仍须对比完整训练配置。 */
    sample_now(HAL_GetTick());
    return PWR_CTRL_OK;
}

int v2_power_sim_latest(monitor_snapshot_t *out)
{
    if (!out) return 0;
    *out = s_latest;
    return 1;
}

static void prepare_target(const pc_test_start_view_t *start)
{
    if (!start->external_ready) {
        boot_mode_t mode = BOOT_MODE_USB_MASKROM;
        (void)pc_boot_mode_parse(start->boot_mode, &mode);
        uint8_t slt = strcmp(start->algorithm_type, "slt") == 0;
        ddr_memory_type_t type = strcmp(start->memory_type, "LPDDR4") == 0 ? DDR_MEMORY_LPDDR4 : DDR_MEMORY_LPDDR4X;
        int changed = !s_training_valid || s_training_slt != slt || s_training_type != type || s_training_boot != mode ||
                      memcmp(s_training_mv, start->target_mv, sizeof(s_mv)) != 0 ||
                      s_type != type || !s_boot_valid || s_boot != mode ||
                      memcmp(s_mv, start->target_mv, sizeof(s_mv)) != 0;
        /* SLT每次计一次虚拟冷启动；FUNC仍按配置/reboot判断，均不复位真实RK。 */
        if (slt || s_state != PWR_CTRL_STATE_ON || start->reboot || changed) ++s_cycles;
        s_training_type = s_type = type;
        s_training_boot = s_boot = mode;
        s_boot_valid = s_training_valid = 1U;
        s_training_slt = slt;
        memcpy(s_training_mv, start->target_mv, sizeof(s_mv));
        memcpy(s_mv, start->target_mv, sizeof(s_mv));
    }
    /* 外部已启动模式按现有虚拟目标采样，不声称执行了冷重启或改变了启动介质。 */
    s_state = PWR_CTRL_STATE_ON;
}

void v2_power_sim_prepare_slt(const pc_test_start_view_t *start)
{
    prepare_target(start);
    sample_now(HAL_GetTick());
}

void v2_power_sim_test_begin(const pc_test_start_view_t *start)
{
    prepare_target(start);
    idd_stats_reset(&s_stats);
    /* ponytail: 复用现有有界窗口，上限约24.8天；更长测试需扩展时基再开放。 */
    (void)idd_stats_begin(&s_stats, HAL_GetTick(), INT32_MAX, 1U);
    s_sample_index = 0U;
    sample_now(HAL_GetTick());
}

void v2_power_sim_test_end(int complete)
{
    v2_power_sim_poll();
    if (complete) idd_stats_finish(&s_stats);
    else idd_stats_cancel(&s_stats);
}

int v2_power_sim_add_statistics(cJSON *body)
{
    return pc_test_add_statistics(body, &s_stats);
}

int v2_power_sim_add_measurement(cJSON *body)
{
    return pc_test_add_measurement(body, &s_stats);
}
