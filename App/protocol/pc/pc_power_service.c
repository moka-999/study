#include "pc_power_service.h"
#include "monitor_task.h"
#include "main.h"
#include <string.h>
#if defined(B1_RK_UART)
#include "pc_trace.h"
#include "rk_task_control.h"
#endif

static pc_power_config_t s_config;
static pc_power_rk_adapter_t s_rk;
static pc_power_diagnostic_t s_diag;
static pc_power_target_t s_target, s_ready_target;
static uint8_t s_configured, s_ready_valid;
static pc_power_action_t s_ready_action; /* 成功配置用途；SLT完成不等价于RK算法就绪。 */

static int prepares_test(pc_power_action_t action)
{
    return action == PC_POWER_PREPARE_TEST || action == PC_POWER_PREPARE_SLT;
}

static int needs_rk_ready(void)
{
    return s_diag.action == PC_POWER_RESTART || s_diag.action == PC_POWER_PREPARE_TEST;
}

static void remember_target(void)
{
    s_ready_target = s_target;
    s_ready_action = s_diag.action;
    s_ready_valid = 1U;
}

static int valid_mv(ddr_memory_type_t type, const uint16_t mv[DDR_RAIL_COUNT])
{
    if ((unsigned)type > DDR_MEMORY_LPDDR4X) return 0;
    for (unsigned i = 0; i < DDR_RAIL_COUNT; ++i) {
        uint16_t min = i == DDR_RAIL_VDD1 ? DDR_VDD1_WRITABLE_MIN_MV : DDR_VDDX_WRITABLE_MIN_MV;
        uint16_t max = i == DDR_RAIL_VDD1 ? DDR_VDD1_WRITABLE_MAX_MV : DDR_VDDX_WRITABLE_MAX_MV;
        uint16_t step = i == DDR_RAIL_VDD1 ? DDR_VDD1_STEP_MV : DDR_VDDX_STEP_MV;
        if (mv[i] < min || mv[i] > max || (mv[i] - min) % step != 0U) return 0;
    }
    return 1;
}

int pc_power_service_configure(const pc_power_config_t *config,
                               const pc_power_rk_adapter_t *adapter)
{
    if (pc_power_service_busy() || pwr_ctrl_get_state() != PWR_CTRL_STATE_OFF) return -1;
    if (config) {
        const uint32_t timeouts[] = {config->transition_timeout_ms, config->ready_timeout_ms};
        for (unsigned i = 0; i < sizeof(timeouts) / sizeof(timeouts[0]); ++i)
            if (!timeouts[i] || timeouts[i] > INT32_MAX) return -1;
        if (config->off_hold_ms > INT32_MAX || config->boot_settle_ms > INT32_MAX) return -1;
        s_config = *config;
    }
    s_configured = config != NULL;
    s_rk = adapter ? *adapter : (pc_power_rk_adapter_t){0};
    s_ready_valid = 0U;
    return 0;
}

int pc_power_service_busy(void) { return s_diag.phase != PC_POWER_IDLE; }
void pc_power_service_invalidate(void) { s_ready_valid = 0U; }
const pc_power_diagnostic_t *pc_power_service_diagnostic(void) { return &s_diag; }

static void enter(pc_power_phase_t phase)
{
    s_diag.phase = phase;
    /* 在同步DAC/总线操作之后重新取时，不能把I/O耗时算成输出稳定等待。 */
    s_diag.phase_since_ms = HAL_GetTick();
#if defined(B1_RK_UART)
    pc_trace_power_phase(&s_diag);
#endif
}

static void complete(const char *error)
{
    s_diag.error = error;
    if (error) {
        s_diag.failed_phase = s_diag.phase;
        s_ready_valid = 0U;
    }
    enter(PC_POWER_DONE);
}

static int elapsed(uint32_t duration)
{
    return (uint32_t)(HAL_GetTick() - s_diag.phase_since_ms) >= duration;
}

static int same_target(const pc_power_target_t *a, const pc_power_target_t *b)
{
    return a->memory_type == b->memory_type && a->boot_mode == b->boot_mode &&
           memcmp(a->mv, b->mv, sizeof(a->mv)) == 0;
}

static const char *power_error(pwr_ctrl_result_t result)
{
    if (result == PWR_CTRL_OK) return NULL;
    if (result == PWR_CTRL_ERR_NOT_CONFIGURED) return "notConfigured";
    if (result == PWR_CTRL_ERR_BUSY || result == PWR_CTRL_ERR_RESOURCE_BUSY) return "busy";
    if (result == PWR_CTRL_ERR_PARAM) return "invalidArgument";
    return "executionFailed";
}

static int arm_rk(void)
{
    /* 每次启动用新令牌；旧心跳不能确认下一轮启动。 */
    if (++s_diag.rk_token == 0U) ++s_diag.rk_token;
    return s_rk.begin(s_diag.rk_token) == 0;
}

const char *pc_power_service_start(pc_power_action_t action,
                                  const pc_power_target_t *target, uint8_t reboot)
{
    if ((unsigned)action > PC_POWER_PREPARE_SLT || reboot > 1U ||
        (prepares_test(action) != (target != NULL))) return "invalidArgument";
    if (pc_power_service_busy()) return "busy";
    if (!s_configured) return "notConfigured";
    pwr_ctrl_state_t state = pwr_ctrl_get_state();
    if (state == PWR_CTRL_STATE_FAULT) return "executionFailed";
    if (state != PWR_CTRL_STATE_OFF && state != PWR_CTRL_STATE_ON) return "busy";
    if (action == PC_POWER_RESTART && state != PWR_CTRL_STATE_ON) return "invalidState";

    pc_power_target_t next = {0};
    if (target) next = *target;
    else {
        next.memory_type = ddr_power_get_memory_type();
        for (unsigned i = 0; i < DDR_RAIL_COUNT; ++i)
            next.mv[i] = ddr_power_get_training_mv(i);
    }
    if (action != PC_POWER_OFF) {
        if (!valid_mv(next.memory_type, next.mv) ||
            (target && (unsigned)target->boot_mode >= BOOT_MODE_COUNT)) return "invalidArgument";
        if (!pwr_ctrl_is_configured()) return "notConfigured";
    }
    /* SLT须重新走服务器下载启动流程；FUNC/PARA可以复用相同的成功配置。 */
    int reuse = action == PC_POWER_PREPARE_TEST && target && !reboot &&
                state == PWR_CTRL_STATE_ON && s_ready_valid &&
                s_ready_action == action &&
                same_target(target, &s_ready_target);
    if (!reuse && (action == PC_POWER_RESTART || action == PC_POWER_PREPARE_TEST) &&
        (!s_rk.begin || !s_rk.poll)) return "notReady";

    s_diag.action = action;
    s_diag.error = NULL;
    s_diag.failed_phase = PC_POWER_IDLE;
    s_target = next;
    if (reuse || (action == PC_POWER_ON && state == PWR_CTRL_STATE_ON) ||
        (action == PC_POWER_OFF && state == PWR_CTRL_STATE_OFF)) {
        complete(NULL);
        return NULL;
    }
    s_ready_valid = 0U;
    if (state == PWR_CTRL_STATE_ON) enter(PC_POWER_REQUEST_OFF);
    else enter(PC_POWER_OFF_HOLD);
    return NULL;
}

const char *pc_power_service_stop(void)
{
    if (!s_configured) return "notConfigured";
    s_ready_valid = 0U;
    s_diag.action = PC_POWER_OFF;
    s_diag.error = NULL;
    s_diag.failed_phase = PC_POWER_IDLE;
    pwr_ctrl_state_t state = pwr_ctrl_get_state();
    if (state == PWR_CTRL_STATE_OFF) complete(NULL);
    /* 已经反序下电时只等待，不能重复请求而缩短既有轨间延时。 */
    else if (state == PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF) enter(PC_POWER_WAIT_OFF);
    else enter(PC_POWER_REQUEST_OFF);
    return NULL;
}

static void request_off(void)
{
    /* 已确认采用直接切电：不发送关机TLV、不等待RK应答；仍走事务锁和轨间时序。 */
    const char *error = power_error(monitor_request_power_off(HAL_GetTick()));
    if (error) complete(error);
    else enter(PC_POWER_WAIT_OFF);
}

static void wait_off(void)
{
    pwr_ctrl_state_t state = pwr_ctrl_get_state();
    if (state == PWR_CTRL_STATE_FAULT) complete("executionFailed");
    else if (state == PWR_CTRL_STATE_OFF) {
        if (s_diag.failed_phase == PC_POWER_WAIT_RK_READY) {
            /* 冷启动训练已超时，到达OFF才交付原测试失败；不再用该点配置上电。 */
            enter(PC_POWER_DONE);
        } else if (s_diag.action == PC_POWER_OFF) complete(NULL);
        else enter(PC_POWER_OFF_HOLD);
    } else if (elapsed(s_config.transition_timeout_ms)) complete("timeout");
}

static void off_hold(void)
{
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_OFF) { complete("invalidState"); return; }
    if (!elapsed(s_config.off_hold_ms)) return;
    if (prepares_test(s_diag.action)) {
        monitor_boot_result_t result = monitor_set_boot_mode(s_target.boot_mode);
        if (result != MONITOR_BOOT_OK) {
            complete(result == MONITOR_BOOT_ERR_BUSY ? "busy" : "executionFailed");
            return;
        }
    }
    enter(PC_POWER_BOOT_SETTLE);
}

static void boot_settle(void)
{
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_OFF) { complete("invalidState"); return; }
    if (!elapsed(s_config.boot_settle_ms)) return;
    if (needs_rk_ready() && !arm_rk()) {
        complete("notReady"); return;
    }
#if defined(B1_RK_UART)
    rk_link_manual_frequency(s_target.manual_boot ? s_target.manual_frequency : -1);
#endif
    const char *error = power_error(monitor_request_training_power_on(
        s_target.memory_type, s_target.mv, HAL_GetTick()));
    if (error) complete(error);
    else {
#if defined(B1_RK_UART)
        /* 只对实际新上电的RK布防一次，复用已运行的配置不会经过这里。 */
        rk_timing_readback_arm(s_target.timing_writes_pending);
#endif
        enter(PC_POWER_WAIT_ON);
    }
}

static void wait_on(void)
{
    pwr_ctrl_state_t state = pwr_ctrl_get_state();
    if (state == PWR_CTRL_STATE_FAULT) complete("executionFailed");
    else if (state == PWR_CTRL_STATE_ON) {
        if (needs_rk_ready()) enter(PC_POWER_WAIT_RK_READY);
        else {
            if (s_diag.action == PC_POWER_PREPARE_SLT) remember_target();
            complete(NULL);
        }
    } else if (state == PWR_CTRL_STATE_OFF) complete("powerOff");
    else if (elapsed(s_config.transition_timeout_ms)) complete("timeout");
}

static void wait_ready(void)
{
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) { complete("powerOff"); return; }
    if (elapsed(s_config.ready_timeout_ms)) {
        if (s_diag.action == PC_POWER_PREPARE_TEST) {
            s_diag.failed_phase = PC_POWER_WAIT_RK_READY;
            s_diag.error = "timeout";
            enter(PC_POWER_REQUEST_OFF);
        } else complete("timeout");
        return;
    }
    int result = s_rk.poll(s_diag.rk_token);
    if (result == 1) {
        if (s_diag.action == PC_POWER_PREPARE_TEST) {
            remember_target();
        }
        complete(NULL);
    } else if (result < 0) complete("notReady");
}

void pc_power_service_poll(void)
{
    /* 轨内毫秒时序仍由defaultTask推进；此表只推进直接关电/Boot/心跳就绪。 */
    static void (*const handlers[])(void) = {
        [PC_POWER_IDLE] = NULL, [PC_POWER_REQUEST_OFF] = request_off,
        [PC_POWER_WAIT_OFF] = wait_off, [PC_POWER_OFF_HOLD] = off_hold,
        [PC_POWER_BOOT_SETTLE] = boot_settle, [PC_POWER_WAIT_ON] = wait_on,
        [PC_POWER_WAIT_RK_READY] = wait_ready, [PC_POWER_DONE] = NULL
    };
    if (s_ready_valid && pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) s_ready_valid = 0U;
    if (handlers[s_diag.phase]) handlers[s_diag.phase]();
}

int pc_power_service_take(pc_power_action_t action, const char **error)
{
    if (!error || s_diag.phase != PC_POWER_DONE || s_diag.action != action) return 0;
    *error = s_diag.error;
    s_diag.phase = PC_POWER_IDLE;
    return 1;
}
