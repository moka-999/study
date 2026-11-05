/* 临时全链路验收适配器：不写DAC/TPS，不启动RK供电；脚本通过串口扮演RK。
 * 正式镜像不编译本文件，不能将此处的颗粒/PC结果映射当成批准的业务合同。
 */
#include "rk_gateway_test.h"
#include "rk_link.h"
#include "pc_protocol.h"
#include <stdio.h>
#include <string.h>

/* 仅用于串口模拟RK的10秒等待上限，不是实际内存算法的统一时限。 */
#define RK_GATEWAY_TEST_TIMEOUT_MS 10000U

static uint32_t s_cookie, s_index;
static const char *s_name;
static uint8_t s_active;

static int response_json(uint16_t command, uint32_t sequence, const char *json,
                         pc_dispatch_response_t *response)
{
    return pc_protocol_build_response(command, sequence, (const uint8_t *)json,
        (uint32_t)strlen(json), response->data, sizeof(response->data), &response->length) == 0 ? 1 : -1;
}
static int execution_response(uint32_t sequence, uint32_t index, const char *error,
                               pc_dispatch_response_t *response)
{
    char json[240];
    if (error == NULL)
        snprintf(json, sizeof(json), "{\"requestCommandId\":259,\"status\":\"accepted\",\"algorithmIndex\":%lu,\"testOnly\":true}", (unsigned long)index);
    else
        snprintf(json, sizeof(json), "{\"requestCommandId\":259,\"status\":\"failed\",\"errorCode\":\"%s\",\"algorithmIndex\":%lu,\"testOnly\":true}", error, (unsigned long)index);
    return response_json(0x0006U, sequence, json, response);
}
pc_dispatch_result_t rk_gateway_test_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response)
{
    if (s_active) return execution_response(sequence, start->algorithm_index, "busy", response);
    if (strcmp(start->algorithm_type, "functional") != 0)
        return execution_response(sequence, start->algorithm_index, "notSupported", response);
    /* 只做联调名称映射；使用正式RK命令目录，避免两张表顺序不同导致发错算法。 */
    const rk_command_descriptor_t *command = rk_command_find_by_name(start->algorithm_name);
    if (command == NULL || command->category != RK_COMMAND_CATEGORY_FUNCTIONAL)
        return execution_response(sequence, start->algorithm_index, "notSupported", response);
    rk_request_t request = {command->command_id, ++s_cookie, RK_GATEWAY_TEST_TIMEOUT_MS, 0U};
    if (rk_link_request(&request) != 0)
        return execution_response(sequence, start->algorithm_index, "busy", response);
    s_name = command->name;
    s_index = start->algorithm_index;
    s_active = 1U;
    return execution_response(sequence, s_index, NULL, response);
}
int rk_gateway_test_poll(uint32_t sequence, pc_dispatch_response_t *response)
{
    rk_response_t result;
    while (rk_link_receive(&result)) {
        if (!s_active || result.cookie != s_cookie) continue;
        s_active = 0U;
        if (result.status != RK_RESPONSE_COMPLETE)
            return execution_response(sequence, s_index, "executionFailed", response);
        char json[PC_DISPATCH_RESPONSE_CAPACITY - 18U];
        pc_functional_die_result_t dies[PC_FUNCTIONAL_DIE_COUNT] = {{0U, "", 0U}, {0U, "", 0U}};
        for (uint8_t i = 0U; i < result.result.failure_count; ++i) {
            const rk_failure_detail_t *failure = &result.result.failures[i];
            dies[failure->die].failed = 1U;
            dies[failure->die].error_address = failure->error_address;
        }
        cJSON *root = pc_functional_result_create(s_name, s_index, "RK_SIM", result.result.elapsed_ms, dies);
        if (root == NULL) return -1;
        int ok = cJSON_PrintPreallocated(root, json, sizeof(json), 0);
        cJSON_Delete(root);
        return ok ? response_json(0x0105U, sequence, json, response) : -1;
    }
    return 0;
}
void rk_gateway_test_disconnect(void)
{
    /* 断线仅撤销PC会话归属，不谎报停止RK，也不复用旧cookie。 */
    s_active = 0U;
}
