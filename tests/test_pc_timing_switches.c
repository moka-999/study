/* 复用既有PC/电源/队列夹具；只替换本次入口，正式解析、编排和RK频率快照均用实际C。 */
#define test_main pc_service_baseline_main
#include "../../test/tests/test_pc_rk_service.c"
#undef test_main
#include "board_rk_config.h"

static const char *const switch_timing_names[] = {
    "CASWriteLatency", "CASReadLatency", "tREFI", "tWR", "tRTP", "tRCD", "tRP",
    "tRASmin", "tRASmax", "tWTR", "tRFCmin", "tFAW", "tCCD", "tRRD", "BL"
};

static char *switch_request(uint16_t command, const char *frequency, int external)
{
    cJSON *root = cJSON_Parse(command == PC_COMMAND_SLT_START ? SLT_START :
                             external ? START_JSON : COLD_START);
    CHECK(root);
    if (command == PC_COMMAND_PARA_START) {
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "algorithmType", cJSON_CreateString("parametric")));
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "algorithmName", cJSON_CreateString("idd_0")));
    }
    cJSON *timing = cJSON_GetObjectItemCaseSensitive(root, "TimingParameters");
    CHECK(cJSON_AddStringToObject(timing, "Freq", frequency));
    for (unsigned i = 0U; i < PC_TIMING_PARAMETER_COUNT; ++i) {
        char value[12];
        snprintf(value, sizeof(value), "%u", i + 1U);
        CHECK(cJSON_AddStringToObject(timing, switch_timing_names[i], value));
    }
    char *json = cJSON_PrintUnformatted(root); CHECK(json);
    cJSON_Delete(root);
    return json;
}

/* 不注入RK频率请求；只推进硬件端口和普通设置的本地SENT事件，检验可选查询不会卡流程。 */
static void switch_execute(uint16_t command, const char *frequency, int external)
{
    pc_dispatch_response_t response;
    char *json = switch_request(command, frequency, external);
    unsigned observed = queue_calls, settings = 0U, algorithms = 0U;
    trace_capture_begin();
    round_command(command, json, "accepted", NULL);
    CHECK(strstr(trace_capture_text(), "timingEnable=" S(B1_RK_TIMING_ENABLE)
                 " freqEnable=" S(B1_RK_FREQ_ENABLE)));
    trace_capture_stop();
    cJSON_free(json);
    const cJSON *saved = pc_test_service_timing_parameters();
    string_is(saved, "Freq", frequency);
    string_is(saved, "BL", "15");
    if (!external && (command == PC_COMMAND_SLT_START ||
                      pc_power_service_diagnostic()->phase != PC_POWER_DONE)) {
        uint32_t token;
        CHECK(rk_link_frequency_get(&token) == (B1_RK_FREQ_ENABLE ? strtoll(frequency, NULL, 10) : -1));
        CHECK(token != 0U && rk_link_frequency_status(token) == 0);
    }
    int prepared = external;
    for (unsigned step = 0U; step < 80U; ++step) {
        if (queue_calls != observed) {
            CHECK(queue_calls == observed + 1U);
            observed = queue_calls;
            if (rk_is_timing_command(queued_request.command_id)) {
                CHECK(B1_RK_TIMING_ENABLE && settings < PC_TIMING_PARAMETER_COUNT);
                CHECK(queued_request.command_id == pc_timing_command(settings));
                CHECK(queued_request.parameter_value == settings + 1U);
                ++settings;
                queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_SENT};
                response_ready = 1;
            } else {
                CHECK(command != PC_COMMAND_SLT_START && algorithms == 0U);
                CHECK(queued_request.command_id == (command == PC_COMMAND_PARA_START ? 21U : 17U));
                ++algorithms;
            }
        }
        if (prepared && (command == PC_COMMAND_SLT_START || algorithms == 1U)) break;
        if (mock_power_state == PWR_CTRL_STATE_WAIT_FIRST_RAIL) mock_power_state = PWR_CTRL_STATE_ON;
        if (mock_power_state == PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF) mock_power_state = PWR_CTRL_STATE_OFF;
        /* SLT关闭普通时序后不需要额外SPL心跳；FUNC/PARA原有ready门禁仍生效。 */
        if (mock_power_state == PWR_CTRL_STATE_ON && (command != PC_COMMAND_SLT_START || B1_RK_TIMING_ENABLE))
            confirm_power(1);
        step_power(20U);
        if (pc_dispatch_next_response(91U, &response)) {
            CHECK(!external && !prepared);
            cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 91U);
            number_is(body, "requestCommandId", command);
            string_is(body, "status", "completed");
            cJSON_Delete(body);
            prepared = 1;
        }
    }
    CHECK(prepared && settings == (B1_RK_TIMING_ENABLE ? PC_TIMING_PARAMETER_COUNT : 0U));
    CHECK(algorithms == (command == PC_COMMAND_SLT_START ? 0U : 1U));
    string_is(pc_test_service_timing_parameters(), "Freq", frequency);
    if (command == PC_COMMAND_SLT_START) {
        if (!B1_RK_TIMING_ENABLE) {
            /* 未主动设置时序时，completed之后的DDR查询仍属于本轮配置。 */
            CHECK(rk_link_frequency_get(NULL) == (B1_RK_FREQ_ENABLE ? strtoll(frequency, NULL, 10) : -1));
        }
        slt_finish_session();
    } else {
        inject(RK_RESPONSE_COMPLETE, 0U);
        CHECK(pc_dispatch_next_response(92U, &response) == 1);
        cJSON *body = decode(&response, command == PC_COMMAND_PARA_START ? PC_COMMAND_PARA_RESULT :
                            PC_COMMAND_TEST_RESULT_REPORT, 92U);
        cJSON_Delete(body);
    }
    CHECK(!pc_test_service_busy());
}

void test_main(void)
{
    power_fixture = 1;
    round_begin();
    const uint16_t commands[] = {PC_COMMAND_START_TEST, PC_COMMAND_PARA_START, PC_COMMAND_SLT_START};
    for (unsigned i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        configure_power();
        switch_execute(commands[i], "2133", 0);
    }
    configure_power();
    switch_execute(PC_COMMAND_START_TEST, "2133", 0);
    unsigned on = power_on_calls, off = power_off_calls;
    switch_execute(PC_COMMAND_START_TEST, "1600", 0);
    CHECK(power_on_calls == on + B1_RK_FREQ_ENABLE);
    CHECK(power_off_calls == off + B1_RK_FREQ_ENABLE);
    configure_power();
    switch_execute(PC_COMMAND_START_TEST, "-123", 0);

    char *external = switch_request(PC_COMMAND_START_TEST, "2133", 1);
    if (B1_RK_FREQ_ENABLE) {
        unsigned before = queue_calls;
        round_command(PC_COMMAND_START_TEST, external, "failed", "invalidState");
        CHECK(queue_calls == before);
    } else {
        switch_execute(PC_COMMAND_START_TEST, "2133", 1);
        switch_execute(PC_COMMAND_PARA_START, "2133", 1);
    }
    cJSON *bad = cJSON_Parse(external); CHECK(bad); cJSON_free(external);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(bad, "TimingParameters"),
                                                "tWR", cJSON_CreateString("invalid")));
    char *bad_json = cJSON_PrintUnformatted(bad); CHECK(bad_json); cJSON_Delete(bad);
    unsigned before = queue_calls;
    round_command(PC_COMMAND_START_TEST, bad_json, "failed", "invalidArgument");
    cJSON_free(bad_json);
    CHECK(queue_calls == before && !pc_test_service_busy());
    round_end();
    message("PASS: PC RK service timing switches, FUNC/PARA/SLT, 15 settings, default frequency, cold reuse and validation\n");
    finish(0U);
}
