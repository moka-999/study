#include "pc_test_service.h"
#include "pc_site_control.h"
#include "handoff_mcu.h"
#include "pc_trace.h"
#include "pc_tester_info.h"
#include "pc_power_service.h"
#include "rk_link.h"
#include "rk_task_control.h"
#include "result_led.h"
#include "b2_service.h"
#include "board_rk_config.h"
#if defined(B1_RK_UART)
#include "monitor_task.h"
#include "board_network_config.h"
#endif
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
#include "v2_power_sim.h"
#endif
#include <string.h>
#include <stdio.h>


/* PC命令、算法类别和结果通道在同一表中绑定，避免IDD从FUNC入口误下发。
 * FUNC等待RK最终结果；PARA由RK任务完成进入、5s采集及退出后交付冻结统计。
 */
typedef struct {
    uint16_t start_command;
    uint16_t result_command;
    rk_command_category_t category;
    const char *algorithm_type;
} pc_rk_test_kind_t;
static const pc_rk_test_kind_t s_rk_test_kinds[] = {
    {PC_COMMAND_START_TEST, PC_COMMAND_TEST_RESULT_REPORT, RK_COMMAND_CATEGORY_FUNCTIONAL, "functional"},
    {PC_COMMAND_PARA_START, PC_COMMAND_PARA_RESULT, RK_COMMAND_CATEGORY_PARAMETRIC, "parametric"},
};

static const pc_rk_test_kind_t *find_test_kind(uint16_t command)
{
    for (size_t i = 0U; i < sizeof(s_rk_test_kinds) / sizeof(s_rk_test_kinds[0]); ++i)
        if (s_rk_test_kinds[i].start_command == command) return &s_rk_test_kinds[i];
    return NULL;
}

/* 仅唯一TCP任务读写。断线后仍保存cookie，旧结果不得交给新连接。 */
static struct {
    uint32_t cookie;
    uint32_t index;
    const char *name;           /* 来自RK只读算法表，不借用已经释放的JSON。 */
    const pc_rk_test_kind_t *kind; /* FUNC/PARA静态通道描述；不借用PC包体。 */
    char site_id[16];           /* 受理时固定B2配置的槽位；未配置则使用W25测试标签。 */
    uint8_t active;             /* 包含已经超时但RK仍可能执行的状态。 */
    uint8_t owner_connected;    /* 只有发起测试的原TCP连接可接收结果。 */
    uint8_t preparing;          /* 电源/Boot/READY尚未完成，此时绝不向RK下发算法。 */
    uint8_t slt;                /* 1=EDK执行并以RELEASE收尾的SLT，不消费RK算法结果队列。 */
    uint8_t discarded;          /* 0104/0107/断线后不再交付结果；停止是否完成另行确认。 */
    uint8_t slt_cancelled;       /* 尚未进入EDK便撤销预约，收尾无需等待EDK RELEASE。 */
    uint8_t slt_off_started, slt_off_done;
    const char *slt_error;      /* 静态错误字符串；收尾失败保留busy，不自动重新断电。 */
    uint8_t slt_error_reported;
    rk_request_t pending;       /* 只保存值，不借用已释放的JSON字符串。 */
    uint8_t delivery_pending;   /* 1=已领取结果，等待当前连接成功组包；不重复执行RK。 */
    rk_response_t delivery;    /* 固定一份结果副本；内存不足时不再消费后续恢复事件。 */
    cJSON *timing_parameters;   /* 自有RAM原文副本；发送完成不代表RK已经应用。 */
    pc_timing_parameters_t timing;
    uint8_t timing_cursor, timing_inflight, configuring;
    uint8_t check_frequency_tx, ready_required, report_prepared;
    uint32_t startup_since_ms;
    uint32_t accepted_ms;       /* IDD从本条0108受理到0109组包，含准备阶段；uint32毫秒允许回绕。 */
} s_test;
static uint8_t s_round_active; /* 仅属于当前TCP连接；算法占用与轮次是否收尾分开记录。 */
/* 0104不依附被取消算法的响应归属；断线后仍推进，不能交给新连接。 */
static struct {
    uint8_t active, owner_connected, power_done, reported;
    uint32_t cleanup_since_ms;
    const char *error;
} s_stop;
#define PC_STOP_CLEANUP_TIMEOUT_MS 3000U
static int64_t s_last_frequency = -1; /* 最近受理的完整启动目标；物理就绪仍由电源服务确认。 */
#define PC_CONFIG_STARTUP_TIMEOUT_MS 60000U

/* 空普通时序的SLT可以先completed、稍后才收到DDR查询；PC收尾/断线不改已启动目标。 */
static int slt_keeps_boot_frequency(void)
{
    return s_test.active && s_test.slt && !s_test.preparing && !s_test.configuring;
}

/* 映射集中一处，物理颜色仍由板级驱动决定；未来扩展镜像时显式扩充此表。 */
static const result_led_target_t s_mirror_leds[] = {
    RESULT_LED_LPDDR0, RESULT_LED_LPDDR1, RESULT_LED_EMCP
};
_Static_assert(sizeof(s_mirror_leds) / sizeof(s_mirror_leds[0]) == RESULT_LED_TARGET_COUNT,
               "镜像灯映射必须覆盖当前三组结果灯");

/* body由调用者释放；成功只代表组帧完成，不代表PC已经收到。 */
static int pack(uint16_t command, uint32_t sequence, const cJSON *body,
                pc_dispatch_response_t *response)
{
    char json[PC_DISPATCH_RESPONSE_CAPACITY - PC_PROTOCOL_HEADER_SIZE];
    if (!body || !response) return PC_DISPATCH_ERROR;
    if (!cJSON_PrintPreallocated((cJSON *)body, json, sizeof(json), 0))
        return PC_DISPATCH_ERROR;
    response->command_id = command;
    response->result_index = s_test.index;
    return pc_protocol_build_response(command, sequence, (const uint8_t *)json,
        (uint32_t)strlen(json), response->data, sizeof(response->data), &response->length) == 0
        ? PC_DISPATCH_RESPONSE_READY : PC_DISPATCH_ERROR;
}

/* status区分受理、启动完成和业务失败；error非NULL绝不等价于内存算法FAIL。 */
static int operation_response(uint16_t command, const uint32_t *index, uint32_t sequence,
    const char *status, const char *error, pc_dispatch_response_t *response)
{
    if (!response) return PC_DISPATCH_ERROR;
    response->request_command = command;
    cJSON *body = cJSON_CreateObject();
    int result = PC_DISPATCH_ERROR;
    int has_index = command == PC_COMMAND_START_TEST || command == PC_COMMAND_PARA_START || command == PC_COMMAND_SLT_START;
    if (body && cJSON_AddNumberToObject(body, "requestCommandId", command) &&
        (!has_index || (index ? cJSON_AddNumberToObject(body, "algorithmIndex", *index)
                             : cJSON_AddNullToObject(body, "algorithmIndex"))) &&
        cJSON_AddStringToObject(body, "status", status) &&
        (!error || cJSON_AddStringToObject(body, "errorCode", error)))
        result = pack(PC_COMMAND_COMMAND_RESP, sequence, body, response);
    cJSON_Delete(body);
    return result;
}

static int command_status_response(uint16_t command, uint32_t sequence, uint32_t index,
    const char *status, const char *error, pc_dispatch_response_t *response)
{
    return operation_response(command, &index, sequence, status, error, response);
}

static int slt_status_response(uint32_t sequence, const char *status,
    const char *error, pc_dispatch_response_t *response)
{
    return operation_response(PC_COMMAND_SLT_START, NULL, sequence, status, error, response);
}

/* 只更新显示，不控制加热；调用前已校验完整失败清单，不逐芯片驱动GPIO。 */
static HAL_StatusTypeDef set_round_lights(uint32_t failed_mask, int beginning)
{
    result_led_state_t states[RESULT_LED_TARGET_COUNT];
    uint8_t modes[B2_MIRROR_COUNT];
    uint8_t known = b2_service_thermal_modes(modes);
    for (size_t mirror = 0U; mirror < RESULT_LED_TARGET_COUNT; ++mirror)
        states[s_mirror_leds[mirror]] = beginning ?
            (!(known & (1U << mirror)) ? RESULT_LED_OFF :
                modes[mirror] ? RESULT_LED_TEST_HOT : RESULT_LED_TEST_NORMAL) :
            (failed_mask & (1U << mirror)) ? RESULT_LED_FAIL : RESULT_LED_PASS;
    return result_led_set_all(states);
}

int pc_test_round_update_modes(uint8_t mask, const uint8_t modes[3])
{
    if (mask >> RESULT_LED_TARGET_COUNT || !modes) return 0;
    for (size_t i = 0U; i < RESULT_LED_TARGET_COUNT; ++i)
        if ((mask & (1U << i)) && modes[i] > 1U) return 0;
    if (!s_round_active || !mask) return 1;
    result_led_state_t states[RESULT_LED_TARGET_COUNT];
    if (result_led_get_all(states) != HAL_OK) return 0;
    for (size_t i = 0U; i < RESULT_LED_TARGET_COUNT; ++i)
        if (mask & (1U << i))
            states[s_mirror_leds[i]] = modes[i] ? RESULT_LED_TEST_HOT : RESULT_LED_TEST_NORMAL;
    return result_led_set_all(states) == HAL_OK;
}

/* 正式B2配置的槽位与IP配对；尚未配置时保留FRU本地测试标签，不改持久化记录。 */
static void save_site_id(const char *fru_site)
{
    uint8_t site = b2_service_site_id();
    if (site) (void)snprintf(s_test.site_id, sizeof(s_test.site_id), "%u", (unsigned int)site);
    else memcpy(s_test.site_id, fru_site, sizeof(s_test.site_id));
}

pc_dispatch_result_t pc_test_round_begin(uint32_t sequence, pc_dispatch_response_t *response)
{
    const char *error = NULL;
    if (s_round_active || pc_test_service_busy() || pc_power_service_busy() || handoff_mcu_busy())
        error = "busy";
    int result = operation_response(PC_COMMAND_ROUND_BEGIN, NULL, sequence,
        error ? "failed" : "completed", error, response);
    if (error || result != PC_DISPATCH_RESPONSE_READY) return result;
    if (set_round_lights(0U, 1) != HAL_OK)
        return operation_response(PC_COMMAND_ROUND_BEGIN, NULL, sequence, "failed", "executionFailed", response);
    s_round_active = 1U;
    return result;
}

pc_dispatch_result_t pc_test_round_end(uint32_t failed_mask,
    uint32_t sequence, pc_dispatch_response_t *response)
{
    const char *error = NULL;
    if (failed_mask >> RESULT_LED_TARGET_COUNT) error = "invalidArgument";
    else if (!s_round_active) error = "invalidState";
    int result = operation_response(PC_COMMAND_ROUND_END, NULL, sequence,
        error ? "failed" : "completed", error, response);
    if (error || result != PC_DISPATCH_RESPONSE_READY) return result;
    if (set_round_lights(failed_mask, 0) != HAL_OK)
        return operation_response(PC_COMMAND_ROUND_END, NULL, sequence, "failed", "executionFailed", response);
    s_round_active = 0U;
    if (s_test.active && !s_test.discarded && !s_test.slt) pc_trace_discard(s_test.cookie, "0107");
    s_test.discarded = 1U;
    if (!slt_keeps_boot_frequency()) rk_link_frequency_end(s_test.cookie);
    if (s_test.active && !s_test.slt) {
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
        v2_power_sim_test_end(0);
#elif defined(B1_RK_UART)
        monitor_idd_cancel();
#endif
    }
    return result;
}

static int status_response(uint16_t command, uint32_t sequence, uint32_t index, const char *error,
                           pc_dispatch_response_t *response)
{
    return command_status_response(command, sequence, index, error ? "failed" : "accepted", error, response);
}

int pc_test_service_busy(void)
{
    return s_test.active != 0U;
}

const cJSON *pc_test_service_timing_parameters(void)
{
    return s_test.timing_parameters;
}

/* 开关只改变本轮执行副本；保留PC原文和解析校验，频率复用比较也使用实际目标。 */
static pc_timing_parameters_t effective_timing(const pc_timing_parameters_t *requested)
{
    pc_timing_parameters_t timing = *requested;
#if !B1_RK_TIMING_ENABLE
    timing.present = 0U;
#endif
#if !B1_RK_FREQ_ENABLE
    timing.frequency = -1;
#endif
    return timing;
}

/* 在真正投递算法之前才开启采样统计；队列失败时收回窗口，不自动重发。 */
static const char *queue_test(const rk_request_t *request)
{
#if defined(B1_RK_UART)
    const rk_command_descriptor_t *descriptor = rk_command_find_by_id(request->command_id);
    int functional = descriptor != NULL && descriptor->category == RK_COMMAND_CATEGORY_FUNCTIONAL;
    if (functional) {
        int measuring = monitor_idd_start(INT32_MAX, 1U);
        if (measuring != IF_OK) return measuring == IF_TIMEOUT ? "busy" : "notReady";
    }
#endif
    int queued = rk_link_request(request);
    if (queued != 0) {
#if defined(B1_RK_UART)
        if (functional) monitor_idd_cancel();
#endif
        return queued == -2 ? "busy" : "notReady";
    }
    return NULL;
}

/* 一次只投递一个时序值；RK任务报告SENT后再投下一项，全部发送后才启动算法。 */
static const char *queue_timing(const pc_timing_parameters_t *timing, uint8_t *cursor, uint32_t cookie)
{
    unsigned i = *cursor;
    while (i < PC_TIMING_PARAMETER_COUNT && !(timing->present & (1U << i))) ++i;
    if (i == PC_TIMING_PARAMETER_COUNT) { *cursor = (uint8_t)i; return NULL; }
    rk_request_t request = {pc_timing_command(i), cookie, 0U, timing->values[i]};
    int result = rk_link_request(&request);
    if (result) return result == -2 ? "busy" : "notReady";
    *cursor = (uint8_t)(i + 1U);
    return NULL;
}

pc_dispatch_result_t pc_test_service_start(uint16_t request_command, const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response)
{
    const pc_rk_test_kind_t *kind = find_test_kind(request_command);
    if (!kind || !start || !response) return PC_DISPATCH_ERROR;
    if (handoff_mcu_busy())
        return status_response(request_command, sequence, start->algorithm_index, "busy", response);
    if (s_test.active || pc_power_service_busy())
        return status_response(request_command, sequence, start->algorithm_index, "busy", response);
    if (!s_round_active)
        return status_response(request_command, sequence, start->algorithm_index, "invalidState", response);

#if defined(B1_RK_UART)
    /* 用户确认RK测试程序已就绪，但仍要求MCU真实电源状态为ON。
     * 不改Boot/电压，也不把心跳或ON状态当作自动DDR-ready确认。
     */
    if (start->external_ready && pwr_ctrl_get_state() != PWR_CTRL_STATE_ON)
        return status_response(request_command, sequence, start->algorithm_index, "notReady", response);
#endif
    if (strcmp(start->algorithm_type, kind->algorithm_type) != 0)
        return status_response(request_command, sequence, start->algorithm_index, "notSupported", response);
    const rk_command_descriptor_t *algorithm = rk_command_find_by_name(start->algorithm_name);
    if (!algorithm || algorithm->category != kind->category)
        return status_response(request_command, sequence, start->algorithm_index, "notSupported", response);
    pc_timing_parameters_t rk_timing = effective_timing(&start->timing);
    /* 外部模式不重新训练；仅在Freq开关启用时拒绝非负启动频率。 */
    if (start->external_ready && rk_timing.frequency >= 0)
        return status_response(request_command, sequence, start->algorithm_index, "invalidState", response);
    fru_info_t fru;
    if (pc_tester_info_load(&fru) != 0)
        return status_response(request_command, sequence, start->algorithm_index, "flashError", response);

    /* 先保证受理应答能组出，再投递命令；内存分配失败不能偷偷启动RK。 */
    int result = status_response(request_command, sequence, start->algorithm_index, NULL, response);
    if (result != PC_DISPATCH_RESPONSE_READY) return result;
    /* 先复制再产生任何硬件/队列副作用；输入JSON返回后会释放，不能保存借用指针。 */
    cJSON *timing = start->timing_parameters ? cJSON_Duplicate(start->timing_parameters, 1) : NULL;
    if (start->timing_parameters && !timing) return PC_DISPATCH_ERROR;
    uint32_t accepted_ms = HAL_GetTick();
    uint32_t cookie = s_test.cookie + 1U;
    if (!cookie) ++cookie;
    rk_request_t request = {algorithm->command_id, cookie, start->timeout_ms, 0U};
    const char *error = NULL;
    uint8_t preparing = 0U, cursor = 0U, inflight = 0U, booting = 0U;
    uint8_t ready_required = 0U;
#if !defined(ENABLE_V2_POWER_SIMULATION) || !ENABLE_V2_POWER_SIMULATION
    if (!start->external_ready) {
        pc_power_target_t target = {0};
        target.memory_type = strcmp(start->memory_type, "LPDDR4") == 0
                           ? DDR_MEMORY_LPDDR4 : DDR_MEMORY_LPDDR4X;
        memcpy(target.mv, start->target_mv, sizeof(target.mv));
        if (pc_boot_mode_parse(start->boot_mode, &target.boot_mode) != 0)
            error = "invalidArgument";
        else error = pc_power_service_start(PC_POWER_PREPARE_TEST, &target,
            start->reboot || rk_timing.frequency != s_last_frequency);
        preparing = error == NULL;
        booting = preparing && pc_power_service_diagnostic()->phase != PC_POWER_DONE;
    } else
#else
    /* 模拟电源时也只按查询回复Freq；本次还要发普通时序才等真实SPL心跳。 */
    if (!start->external_ready && rk_timing.frequency >= 0) {
        booting = 1U;
        ready_required = rk_timing.present != 0U;
    } else
#endif
    {
        if (rk_timing.present) {
            error = queue_timing(&rk_timing, &cursor, cookie);
            inflight = error == NULL;
        } else error = queue_test(&request);
    }
    if (error) {
        cJSON_Delete(timing);
        return status_response(request_command, sequence, start->algorithm_index, error, response);
    }
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    /* 仍投递真实RK命令。队列拒绝时不改变虚拟电源、不清空上一轮统计。 */
    v2_power_sim_test_begin(start);
#endif
    s_test.cookie = request.cookie;
    s_test.index = start->algorithm_index;
    s_test.name = algorithm->name;
    s_test.kind = kind;
    s_test.accepted_ms = accepted_ms;
    save_site_id(fru.site_label);
    s_test.site_id[sizeof(s_test.site_id) - 1U] = '\0';
    s_test.active = 1U;
    s_test.owner_connected = 1U;
    s_test.preparing = preparing;
    s_test.slt = 0U;
    s_test.discarded = 0U;
    s_test.pending = request;
    s_test.timing = rk_timing;
    s_test.timing_cursor = cursor;
    s_test.timing_inflight = inflight;
    s_test.configuring = inflight || booting;
    s_test.report_prepared = preparing;
    s_test.check_frequency_tx = booting && rk_timing.frequency >= 0;
    s_test.ready_required = ready_required;
    s_test.startup_since_ms = ready_required ? HAL_GetTick() : 0U;
    if (!start->external_ready) s_last_frequency = rk_timing.frequency;
    if (booting) rk_link_frequency_begin(cookie, rk_timing.frequency);
    if (ready_required) (void)rk_link_ready_begin(cookie);
    pc_trace_start(request_command, start, s_test.name, cookie);
    cJSON_Delete(s_test.timing_parameters);
    s_test.timing_parameters = timing;
    return PC_DISPATCH_RESPONSE_READY;
}

pc_dispatch_result_t pc_test_slt_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response)
{
    if (!start || !response) return PC_DISPATCH_ERROR;
    if (handoff_mcu_busy()) return slt_status_response(sequence, "failed", "busy", response);
    if (s_test.active || pc_power_service_busy())
        return slt_status_response(sequence, "failed", "busy", response);
    if (!s_round_active) return slt_status_response(sequence, "failed", "invalidState", response);
#if !B1_SERIAL_NET_KEEP_TCP
    /* 正式PC流程必须保持原连接；独占IP模式仅供串口调试。 */
    return slt_status_response(sequence, "failed", "notSupported", response);
#endif
    pc_timing_parameters_t rk_timing = effective_timing(&start->timing);
    fru_info_t fru;
    if (pc_tester_info_load(&fru) != 0)
        return slt_status_response(sequence, "failed", "flashError", response);
    int result = slt_status_response(sequence, "accepted", NULL, response);
    if (result != PC_DISPATCH_RESPONSE_READY) return result;
    cJSON *timing = start->timing_parameters ? cJSON_Duplicate(start->timing_parameters, 1) : NULL;
    if (start->timing_parameters && !timing) return PC_DISPATCH_ERROR;
    uint32_t cookie = s_test.cookie + 1U;
    if (!cookie) ++cookie;
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    v2_power_sim_prepare_slt(start);
#else
    pc_power_target_t target = {0};
    target.memory_type = strcmp(start->memory_type, "LPDDR4") == 0
                       ? DDR_MEMORY_LPDDR4 : DDR_MEMORY_LPDDR4X;
    memcpy(target.mv, start->target_mv, sizeof(target.mv));
    if (pc_boot_mode_parse(start->boot_mode, &target.boot_mode) != 0) {
        cJSON_Delete(timing);
        return slt_status_response(sequence, "failed", "invalidArgument", response);
    }
    if (!handoff_slt_begin(cookie)) {
        cJSON_Delete(timing);
        return slt_status_response(sequence, "failed", "busy", response);
    }
    const char *error = pc_power_service_start(PC_POWER_PREPARE_SLT, &target, start->reboot);
    if (error) {
        (void)handoff_slt_cancel(cookie);
        cJSON_Delete(timing);
        return slt_status_response(sequence, "failed", error, response);
    }
#endif
    s_test.name = "memtest86";
    save_site_id(fru.site_label);
    s_test.site_id[sizeof(s_test.site_id) - 1U] = '\0';
    s_test.active = s_test.owner_connected = s_test.preparing = s_test.slt = 1U;
    s_test.discarded = 0U;
    s_test.slt_cancelled = 0U;
    s_test.slt_off_started = s_test.slt_off_done = s_test.slt_error_reported = 0U;
    s_test.slt_error = NULL;
    s_test.cookie = cookie;
    s_test.timing = rk_timing;
    s_test.timing_cursor = s_test.timing_inflight = 0U;
    s_test.configuring = s_test.report_prepared = 1U;
    s_test.check_frequency_tx = rk_timing.frequency >= 0;
    s_test.ready_required = rk_timing.present != 0U;
    s_last_frequency = rk_timing.frequency;
    rk_link_frequency_begin(cookie, rk_timing.frequency);
    if (s_test.ready_required) (void)rk_link_ready_begin(cookie);
    pc_trace_start(PC_COMMAND_SLT_START, start, s_test.name, cookie);
    cJSON_Delete(s_test.timing_parameters);
    s_test.timing_parameters = timing;
    return result;
}

/* 只由原测试服务消费内部OFF结果；串口命令和PC电源命令不参与此流程。
 * RELEASED表示EDK最终OK已实际发完，避免PC先收到XML便切断后续HTML上传。
 */
static int poll_slt_completion(uint32_t sequence, pc_dispatch_response_t *response)
{
    handoff_slt_status_t status = handoff_slt_status(s_test.cookie);
    if (!s_test.slt_error && !s_test.slt_cancelled && status == HANDOFF_SLT_FAILED) {
        s_test.slt_error = "executionFailed";
    }
    if (!s_test.slt_error && !s_test.slt_cancelled && !s_test.slt_off_started &&
        status == HANDOFF_SLT_OFF_READY) {
        s_test.slt_error = "powerOff";
    }
    if (!s_test.slt_error && !s_test.slt_off_started &&
        (status == HANDOFF_SLT_RELEASED || s_test.slt_cancelled)) {
        s_test.slt_error = pc_power_service_start(PC_POWER_OFF, NULL, 0U);
        s_test.slt_off_started = s_test.slt_error == NULL;
    }
    if (!s_test.slt_error && s_test.slt_off_started && !s_test.slt_off_done) {
        const char *error = NULL;
        if (pc_power_service_take(PC_POWER_OFF, &error)) {
            s_test.slt_error = error;
            s_test.slt_off_done = error == NULL;
        }
    }
    if (s_test.slt_error) {
        if (!s_test.owner_connected || s_test.slt_error_reported) return 0;
        int result = slt_status_response(sequence, "failed", s_test.slt_error, response);
        if (result == PC_DISPATCH_RESPONSE_READY) s_test.slt_error_reported = 1U;
        return result;
    }
    /* 已取消且从未进入EDK时无需会话状态；两种路径均要求真实OFF及协议恢复。 */
    if (!s_test.slt_off_done || pwr_ctrl_get_state() != PWR_CTRL_STATE_OFF ||
        handoff_mcu_busy() || (!s_test.slt_cancelled && status != HANDOFF_SLT_OFF_READY)) {
        return 0;
    }
    rk_link_frequency_end(s_test.cookie);
    s_test.active = s_test.owner_connected = 0U;
    /* RELEASE只释放执行占用；不新增PC完成通知，最终灯态仍由0107决定。 */
    return PC_DISPATCH_NO_RESPONSE;
}

/* 启动/发送错误是业务失败，不伪造芯片FAIL。部分已发送设置不回滚、不自动重发。 */
static int preparation_failed(const char *error, uint32_t sequence, pc_dispatch_response_t *response)
{
    if (s_test.check_frequency_tx) pc_power_service_invalidate();
    rk_link_frequency_end(s_test.cookie);
    /* 尚未进入EDK才可撤销自动预约；已经运行则保留占用等待RELEASE。 */
    s_test.active = s_test.slt && !s_test.preparing;
    if (s_test.slt) {
        s_test.slt_cancelled = handoff_slt_cancel(s_test.cookie) != 0;
        s_test.active = !s_test.slt_cancelled;
    }
    s_test.preparing = s_test.configuring = s_test.timing_inflight = 0U;
    if (!s_test.owner_connected || s_test.discarded || !response) return 0;
    return s_test.slt ? slt_status_response(sequence, "failed", error, response) :
        status_response(s_test.kind->start_command, sequence, s_test.index, error, response);
}

/* 只等待本地发送事件，忽略RK尚未定义的设置回包；不把进度或错误算法结果当成功ACK。 */
static int poll_preparation(uint32_t sequence, pc_dispatch_response_t *response)
{
    const char *error = NULL;
    if (s_test.preparing) {
#if !defined(ENABLE_V2_POWER_SIMULATION) || !ENABLE_V2_POWER_SIMULATION
        if (!pc_power_service_take(s_test.slt ? PC_POWER_PREPARE_SLT : PC_POWER_PREPARE_TEST, &error)) return 0;
#endif
        if (error) return preparation_failed(error, sequence, response);
        s_test.preparing = 0U;
        s_test.configuring = 1U;
        if (s_test.ready_required) s_test.startup_since_ms = HAL_GetTick();
    }
    if (s_test.timing_inflight) {
        rk_response_t event;
        do { if (!rk_link_receive(&event)) return 0; } while (event.cookie != s_test.cookie);
        s_test.timing_inflight = 0U;
        if (event.status != RK_RESPONSE_SENT)
            return preparation_failed(event.status == RK_RESPONSE_TX_FAILED ? "txFailed" :
                event.status == RK_RESPONSE_POWER_OFF ? "powerOff" : "executionFailed", sequence, response);
    }
    /* 丢弃后收完已入队设置，不继续投递；SLT仍保留原有占用，不自动取消其执行。 */
    if (!s_test.owner_connected || s_test.discarded) {
        if (s_test.check_frequency_tx) pc_power_service_invalidate();
        rk_link_frequency_end(s_test.cookie);
        s_test.configuring = 0U;
        if (!s_test.slt) s_test.active = 0U;
        else s_test.slt_cancelled = handoff_slt_cancel(s_test.cookie) != 0;
        return 0;
    }
#if defined(B1_RK_UART)
    /* 准备结束后掉电不能继续发送，也不能把最后一个SENT当成仍已准备好的SLT。 */
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON)
        return preparation_failed("powerOff", sequence, response);
#endif
    if (s_test.slt && s_test.timing.present &&
        handoff_slt_status(s_test.cookie) == HANDOFF_SLT_EARLY_EDK) {
        return preparation_failed("notReady", sequence, response);
    }
    /* 0表示RK尚未查询，可继续；只有实际回复发送失败才终止本次准备。 */
    if (s_test.check_frequency_tx && rk_link_frequency_status(s_test.cookie) < 0)
        return preparation_failed("txFailed", sequence, response);
    if (s_test.ready_required) {
        int ready = rk_link_ready_poll(s_test.cookie);
        if (ready < 0) return preparation_failed("notReady", sequence, response);
        if (!ready) {
            if ((uint32_t)(HAL_GetTick() - s_test.startup_since_ms) >= PC_CONFIG_STARTUP_TIMEOUT_MS)
                return preparation_failed("timeout", sequence, response);
            return 0;
        }
        s_test.ready_required = 0U;
    }
    s_test.check_frequency_tx = 0U;
    /* 无普通时序的SLT可在DDR查询前向PC报告准备完成，保留本次Freq至收尾。 */
    if (!s_test.slt || s_test.timing.present) rk_link_frequency_end(s_test.cookie);
    unsigned next = s_test.timing_cursor;
    while (next < PC_TIMING_PARAMETER_COUNT && !(s_test.timing.present & (1U << next))) ++next;
    if (next < PC_TIMING_PARAMETER_COUNT) {
        error = queue_timing(&s_test.timing, &s_test.timing_cursor, s_test.cookie);
        if (error) return preparation_failed(error, sequence, response);
        s_test.timing_inflight = 1U;
        return 0;
    }
    int report = s_test.slt || s_test.report_prepared;
    int result = 0;
    if (report) {
        result = s_test.slt ? slt_status_response(sequence, "completed", NULL, response) :
            command_status_response(s_test.kind->start_command, sequence, s_test.index, "completed", NULL, response);
        if (result != PC_DISPATCH_RESPONSE_READY) {
            if (!s_test.slt) {
                s_test.active = s_test.owner_connected = s_test.configuring = 0U;
            }
            return result;
        }
    }
    if (!s_test.slt && (error = queue_test(&s_test.pending)) != NULL)
        return preparation_failed(error, sequence, response);
    if (s_test.slt && !handoff_slt_prepared(s_test.cookie)) {
        return preparation_failed("notReady", sequence, response);
    }
    s_test.configuring = 0U;
    return result;
}

/* 组包完成或原连接明确放弃后才释放；TIMEOUT仍须等待RK恢复事件。 */
static void release_delivery(void)
{
    if (s_test.delivery.status != RK_RESPONSE_TIMEOUT) {
        s_test.active = 0U;
        s_test.owner_connected = 0U;
    }
    s_test.delivery_pending = 0U;
}

static const char *begin_stop(int closing_site)
{
    if (s_stop.active && (!s_stop.error || !s_stop.reported))
        return closing_site ? NULL : "busy";
    const char *error = pc_power_service_stop();
    if (error && !closing_site) return error;
    if (s_test.active && !s_test.discarded) pc_trace_discard(s_test.cookie, "stop");
    s_test.discarded = 1U;
    s_test.owner_connected = s_test.preparing = s_test.configuring = 0U;
    s_test.timing_inflight = s_test.delivery_pending = 0U;
    s_test.active = 1U; /* 直到RK任务确认清理完毕，下一算法仍须返回busy。 */
    rk_link_frequency_end(s_test.cookie);
    monitor_idd_cancel();
    handoff_mcu_stop();
    rk_task_stop_begin();
    s_stop.active = 1U;
    s_stop.owner_connected = !closing_site;
    s_stop.power_done = s_stop.reported = 0U;
    s_stop.error = error;
    return NULL;
}

const char *pc_test_service_stop(void)
{
    return begin_stop(0);
}

void pc_test_service_stop_site(void)
{
    s_round_active = 0U;
    (void)begin_stop(1);
}

static int poll_stop(uint32_t sequence, pc_dispatch_response_t *response)
{
    if (!s_stop.power_done && !s_stop.error &&
        pc_power_service_take(PC_POWER_OFF, &s_stop.error)) {
        s_stop.power_done = s_stop.error == NULL;
        s_stop.cleanup_since_ms = HAL_GetTick();
    }
    int ready = s_stop.power_done && pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF &&
                !handoff_mcu_busy() && rk_task_stop_ready();
    if (!ready && s_stop.power_done && !s_stop.error &&
        (uint32_t)(HAL_GetTick() - s_stop.cleanup_since_ms) >= PC_STOP_CLEANUP_TIMEOUT_MS)
        s_stop.error = "timeout";
    if (!s_stop.error && !ready) return 0;
    if (s_stop.reported) return 0;
    int result = PC_DISPATCH_NO_RESPONSE;
    if (s_stop.owner_connected) {
        if (!response) return 0;
        result = operation_response(PC_COMMAND_STOP_TEST, NULL, sequence,
            s_stop.error ? "failed" : "completed", s_stop.error, response);
        if (result != PC_DISPATCH_RESPONSE_READY) return 0;
    }
    s_stop.reported = 1U;
    if (!s_stop.error) {
        rk_task_stop_end();
        s_stop.active = s_stop.owner_connected = 0U;
        s_test.active = s_test.slt = 0U;
        s_last_frequency = -1;
    }
    pc_site_stop_finished(s_stop.error);
    return result;
}

int pc_test_service_poll(uint32_t sequence, pc_dispatch_response_t *response)
{
    /* 即使有连接且response=NULL也须推进停止；最终应答仍只由next_response领取。 */
    if (s_stop.active) return poll_stop(sequence, response);
    if (s_test.owner_connected && response == NULL) return 0;
    if (s_test.preparing || s_test.configuring) return poll_preparation(sequence, response);
    if (s_test.active && s_test.slt) return poll_slt_completion(sequence, response);
    rk_response_t result;
    for (;;) {
        if (!s_test.delivery_pending) {
            if (!rk_link_receive(&result)) return 0;
            if (!s_test.active || result.cookie != s_test.cookie) continue;
            if (result.status == RK_RESPONSE_RECOVERED) {
                s_test.active = 0U;
                s_test.owner_connected = 0U;
                continue; /* 迟到结果只解除未知状态，不补报第二个“测试成功”。 */
            }
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
            /* 只在首次领取时冻结统计；组包重试不能改变本次测量窗口。 */
            if (s_test.kind->category == RK_COMMAND_CATEGORY_FUNCTIONAL) v2_power_sim_test_end(!s_test.discarded && result.status == RK_RESPONSE_COMPLETE);
#elif defined(B1_RK_UART)
            /* 统计在首次领取时冻结；不是RK或JEDEC定义的同步IDD测量窗口。 */
            if (s_test.kind->category == RK_COMMAND_CATEGORY_FUNCTIONAL) {
                if (!s_test.discarded && result.status == RK_RESPONSE_COMPLETE) monitor_idd_finish();
                else monitor_idd_cancel();
            }
#endif
            if (result.status == RK_RESPONSE_COMPLETE) {
                if (s_test.kind->category == RK_COMMAND_CATEGORY_PARAMETRIC)
                    result.result.elapsed_ms = (uint32_t)(HAL_GetTick() - s_test.accepted_ms);
                pc_trace_result(s_test.name, s_test.index, &result.result);
            }
            s_test.delivery = result;
            s_test.delivery_pending = 1U;
        }
        result = s_test.delivery;
        if (!s_test.owner_connected || s_test.discarded) {
            release_delivery();
            continue;
        }
        int ready;
        if (result.status == RK_RESPONSE_COMPLETE) {
            /* 完整结果已经由RK任务校验：未列出的颗粒才可判PASS；先按编号归位。 */
            pc_functional_die_result_t dies[PC_FUNCTIONAL_DIE_COUNT] = {{0U, "", 0U}, {0U, "", 0U}};
            _Static_assert(PC_FUNCTIONAL_DIE_COUNT == RK_RESULT_DIE_COUNT, "DDR颗粒数量必须一致");
            for (uint8_t i = 0U; i < result.result.failure_count; ++i) {
                const rk_failure_detail_t *failure = &result.result.failures[i];
                dies[failure->die].failed = 1U;
                dies[failure->die].error_address = failure->error_address;
            }
            /* 一包包含同一B1的两颗结果，共用siteId、耗时和三轨测量均只出现一次。 */
            int idd = s_test.kind->category == RK_COMMAND_CATEGORY_PARAMETRIC;
            uint64_t elapsed_ms = idd ? (uint32_t)(HAL_GetTick() - s_test.accepted_ms)
                                      : result.result.elapsed_ms;
            cJSON *body = pc_algorithm_result_create(s_test.kind->algorithm_type, s_test.name, s_test.index,
                s_test.site_id, elapsed_ms, dies);
            int measurement_ok = body != NULL;
            if (body != NULL && idd) measurement_ok = pc_test_add_measurement(body, &result.measurement);
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
            else if (body != NULL) measurement_ok = v2_power_sim_add_measurement(body);
#elif defined(B1_RK_UART)
            else if (body != NULL) {
                idd_stats_t stats;
                measurement_ok = monitor_idd_get(&stats) && pc_test_add_measurement(body, &stats);
            }
#endif
            if (!measurement_ok) {
                cJSON_Delete(body);
                return 0; /* 暂留冻结结果重试组包，不重采样或重复执行RK。 */
            }
            ready = pack(s_test.kind->result_command, sequence, body, response);
            cJSON_Delete(body);
        } else {
            /* 用表区分通信异常，不能把发送失败、超时或掉电写成芯片FAIL。 */
            static const char *const errors[] = {
                [RK_RESPONSE_TIMEOUT] = "timeout",
                [RK_RESPONSE_TX_FAILED] = "txFailed",
                [RK_RESPONSE_INVALID] = "invalidArgument",
                [RK_RESPONSE_POWER_OFF] = "powerOff",
            };
            const char *error = (unsigned)result.status < sizeof(errors) / sizeof(errors[0])
                                ? errors[result.status] : NULL;
            ready = status_response(s_test.kind->start_command, sequence, s_test.index, error ? error : "executionFailed", response);
        }
        if (ready != PC_DISPATCH_RESPONSE_READY) return 0;
        /* 只承诺组包成功；TCP发送失败仍按断线规则处理，不跨连接重发旧结果。 */
        release_delivery();
        return ready;
    }
}

void pc_test_service_disconnected(void)
{
    s_stop.owner_connected = 0U;
    if (s_test.active && !s_test.discarded && !s_test.slt) pc_trace_discard(s_test.cookie, "disconnect");
    if (!slt_keeps_boot_frequency()) rk_link_frequency_end(s_test.cookie);
    s_round_active = 0U;
    s_test.discarded = 1U;
    s_test.owner_connected = 0U;
}
