#include "pc_test_service.h"
#include "pc_tester_info.h"
#include "rk_link.h"
#include "ota_app.h"
#if defined(B1_RK_UART)
#include "monitor_task.h"
#endif
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
#include "v2_power_sim.h"
#endif
#include <string.h>

/* 仅唯一TCP任务读写。断线后仍保存cookie，旧结果不得交给新连接。 */
static struct {
    uint32_t cookie;
    uint32_t index;
    const char *name;           /* 来自RK只读算法表，不借用已经释放的JSON。 */
    char site_id[16];           /* 本次受理时从W25 FRU读取的站点标签。 */
    uint8_t active;             /* 包含已经超时但RK仍可能执行的状态。 */
    uint8_t owner_connected;    /* 只有发起测试的原TCP连接可接收结果。 */
} s_test;

/* body由调用者释放；成功只代表组帧完成，不代表PC已经收到。 */
static int pack(uint16_t command, uint32_t sequence, const cJSON *body,
                pc_dispatch_response_t *response)
{
    char json[PC_DISPATCH_RESPONSE_CAPACITY - PC_PROTOCOL_HEADER_SIZE];
    if (!body || !response) return PC_DISPATCH_ERROR;
    if (!cJSON_PrintPreallocated((cJSON *)body, json, sizeof(json), 0))
        return PC_DISPATCH_ERROR;
    return pc_protocol_build_response(command, sequence, (const uint8_t *)json,
        (uint32_t)strlen(json), response->data, sizeof(response->data), &response->length) == 0
        ? PC_DISPATCH_RESPONSE_READY : PC_DISPATCH_ERROR;
}

/* error=NULL是已受理；非NULL是业务/传输失败，绝不等价于内存算法FAIL。 */
static int status_response(uint32_t sequence, uint32_t index, const char *error,
                           pc_dispatch_response_t *response)
{
    cJSON *body = cJSON_CreateObject();
    int result = PC_DISPATCH_ERROR;
    if (body && cJSON_AddNumberToObject(body, "requestCommandId", PC_COMMAND_START_TEST) &&
        cJSON_AddNumberToObject(body, "algorithmIndex", index) &&
        cJSON_AddStringToObject(body, "status", error ? "failed" : "accepted") &&
        (!error || cJSON_AddStringToObject(body, "errorCode", error)))
        result = pack(PC_COMMAND_COMMAND_RESP, sequence, body, response);
    cJSON_Delete(body);
    return result;
}

int pc_test_service_busy(void)
{
    return s_test.active != 0U;
}

pc_dispatch_result_t pc_test_service_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response)
{
    if (!start || !response) return PC_DISPATCH_ERROR;
    if (!ota_app_is_confirmed())
        return status_response(sequence, start->algorithm_index, "otaNotReady", response);
    if (s_test.active)
        return status_response(sequence, start->algorithm_index, "busy", response);

    /* 默认只执行“保持外部现状”。V2显式电源模拟可接收完整配置，
     * 但不向真实器件执行；B1上电、训练及ready合同不因模拟被放行。
     */
#if !defined(ENABLE_V2_POWER_SIMULATION) || !ENABLE_V2_POWER_SIMULATION
    if (!start->external_ready)
        return status_response(sequence, start->algorithm_index, "notReady", response);
#endif
#if defined(B1_RK_UART)
    /* 用户确认RK测试程序已就绪，但仍要求MCU真实电源状态为ON。
     * 不改Boot/电压，也不把心跳或ON状态当作自动DDR-ready确认。
     */
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON)
        return status_response(sequence, start->algorithm_index, "notReady", response);
#endif
    if (strcmp(start->algorithm_type, "functional") != 0)
        return status_response(sequence, start->algorithm_index, "notSupported", response);
    const rk_command_descriptor_t *algorithm = rk_command_find_by_name(start->algorithm_name);
    if (!algorithm)
        return status_response(sequence, start->algorithm_index, "notSupported", response);
#if defined(ENABLE_OTA_BOOT) && ENABLE_OTA_BOOT
    ota_download_progress_t progress;
    ota_download_get_progress(&progress);
    if (progress.state == OTA_DOWNLOAD_RECEIVING || progress.state == OTA_DOWNLOAD_READY ||
        progress.state == OTA_DOWNLOAD_COMMIT_UNKNOWN)
        return status_response(sequence, start->algorithm_index, "busy", response);
#endif
    fru_info_t fru;
    if (pc_tester_info_load(&fru) != 0)
        return status_response(sequence, start->algorithm_index, "flashError", response);

    /* 先保证受理应答能组出，再投递命令；内存分配失败不能偷偷启动RK。 */
    int result = status_response(sequence, start->algorithm_index, NULL, response);
    if (result != PC_DISPATCH_RESPONSE_READY) return result;
#if defined(B1_RK_UART)
    /* 先启动实际采样窗口，失败时不能下发RK；仅后台新采样会计入统计。
     * ponytail: 复用有界毫秒窗口，上限约24.8天，覆盖小时级测试；更长需扩展时基。
     */
    int measuring = monitor_idd_start(INT32_MAX, 1U);
    if (measuring != IF_OK)
        return status_response(sequence, start->algorithm_index,
                               measuring == IF_TIMEOUT ? "busy" : "notReady", response);
#endif
    rk_request_t request = {algorithm->command_id, s_test.cookie + 1U, start->timeout_ms};
    int queued = rk_link_request(&request);
    if (queued != 0) {
#if defined(B1_RK_UART)
        monitor_idd_cancel(); /* 未下发成功，不保留运行中的孤立窗口。 */
#endif
        return status_response(sequence, start->algorithm_index,
                               queued == -2 ? "busy" : "notReady", response);
    }
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    /* 仍投递真实RK命令。队列拒绝时不改变虚拟电源、不清空上一轮统计。 */
    v2_power_sim_test_begin(start);
#endif
    s_test.cookie = request.cookie;
    s_test.index = start->algorithm_index;
    s_test.name = algorithm->name;
    memcpy(s_test.site_id, fru.site_label, sizeof(s_test.site_id));
    s_test.site_id[sizeof(s_test.site_id) - 1U] = '\0';
    s_test.active = 1U;
    s_test.owner_connected = 1U;
    return PC_DISPATCH_RESPONSE_READY;
}

int pc_test_service_poll(uint32_t sequence, pc_dispatch_response_t *response)
{
    if (s_test.owner_connected && response == NULL) return 0;
    rk_response_t result;
    while (rk_link_receive(&result)) {
        if (!s_test.active || result.cookie != s_test.cookie) continue;
        if (result.status == RK_RESPONSE_RECOVERED) {
            s_test.active = 0U;
            s_test.owner_connected = 0U;
            continue; /* 迟到结果只解除未知状态，不补报第二个“测试成功”。 */
        }
        int deliver = s_test.owner_connected && response != NULL;
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
        /* 即使发起连接已断开也要冻结本次统计；新连接不能获得旧0105。 */
        v2_power_sim_test_end(result.status == RK_RESPONSE_COMPLETE);
#elif defined(B1_RK_UART)
        /* 统计在MCU领取结果时冻结；不是RK或JEDEC定义的同步IDD测量窗口。 */
        if (result.status == RK_RESPONSE_COMPLETE) monitor_idd_finish();
        else monitor_idd_cancel();
#endif
        if (result.status != RK_RESPONSE_TIMEOUT) {
            s_test.active = 0U;
            s_test.owner_connected = 0U;
        }
        if (!deliver) continue;
        if (result.status == RK_RESPONSE_COMPLETE) {
            /* 一个算法只上报一个整体结果。没有地址到芯片的映射表时不猜die；
             * 没有B2编号时mirrorIndex=0；默认未采样为null。
             */
            cJSON *body = pc_functional_result_create(s_test.name, s_test.index,
                s_test.site_id, 0U, "", result.result.error_address, result.result.elapsed_ms,
                result.result.failed);
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
            if (!body || !v2_power_sim_add_measurement(body)) {
                cJSON_Delete(body);
                return PC_DISPATCH_ERROR;
            }
#elif defined(B1_RK_UART)
            idd_stats_t stats;
            if (!body || !monitor_idd_get(&stats) || !pc_test_add_measurement(body, &stats)) {
                cJSON_Delete(body);
                return PC_DISPATCH_ERROR;
            }
#endif
            int ready = pack(PC_COMMAND_TEST_RESULT_REPORT, sequence, body, response);
            cJSON_Delete(body);
            return ready;
        }
        /* 用表区分通信异常，不能把发送失败、超时或掉电写成芯片FAIL。 */
        static const char *const errors[] = {
            [RK_RESPONSE_TIMEOUT] = "timeout",
            [RK_RESPONSE_TX_FAILED] = "txFailed",
            [RK_RESPONSE_INVALID] = "invalidArgument",
            [RK_RESPONSE_POWER_OFF] = "powerOff",
        };
        const char *error = (unsigned)result.status < sizeof(errors) / sizeof(errors[0])
                            ? errors[result.status] : NULL;
        return status_response(sequence, s_test.index, error ? error : "executionFailed", response);
    }
    return 0;
}

void pc_test_service_disconnected(void)
{
    s_test.owner_connected = 0U;
}
