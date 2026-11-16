/* 0104使用实际PC分发/电源服务；硬件及RK确认由夹具显式推进，不访问真板。 */
#define test_main baseline_test_main
#include "test_pc_rk_service.c"
#undef test_main

static void request_stop(void)
{
    pc_dispatch_response_t response;
    CHECK(dispatch(PC_COMMAND_STOP_TEST, "{}", 400U, &response) == PC_DISPATCH_NO_RESPONSE);
    CHECK(pc_test_service_busy() && stop_requested && !stop_ready);
    CHECK(handoff_mcu_busy());
    CHECK(pc_dispatch_next_response(401U, &response) == 0);
}

static void complete_stop(int connected)
{
    pc_dispatch_response_t response;
    step_power(0U);
    mock_power_state = PWR_CTRL_STATE_OFF;
    step_power(10U);
    CHECK(!pc_power_service_busy() && pc_test_service_busy());
    CHECK(pc_dispatch_next_response(401U, &response) == 0);
    /* 仅OFF不够；网络恢复后仍须等RK队列与解析器清理确认。 */
    handoff_test.reserved = 0;
    CHECK(pc_dispatch_next_response(401U, &response) == 0);
    response_ready = 0;
    stop_ready = 1;
    step_power(0U);
    if (connected) power_completion(PC_COMMAND_STOP_TEST, NULL);
    else CHECK(pc_dispatch_next_response(401U, &response) == 0);
    CHECK(!stop_requested && !pc_test_service_busy() && !pc_power_service_busy());
    CHECK(mock_power_state == PWR_CTRL_STATE_OFF);
}

void test_main(void)
{
    pc_dispatch_response_t response;
    power_fixture = 1;
    configure_power();
    /* 无0106、已OFF也可以请求停止；不会暗中开电。 */
    unsigned on = power_on_calls;
    request_stop(); complete_stop(1);
    CHECK(power_on_calls == on);
    round_begin();

    /* 同一轮内可中止任意FUNC/PARA，保留0106；旧结果不上传。 */
    const struct { uint16_t command; const char *json; } algorithms[] = {
        {PC_COMMAND_START_TEST, START_JSON},
        {PC_COMMAND_PARA_START, "{\"algorithmType\":\"parametric\",\"algorithmName\":\"idd_2P\","
         "\"algorithmIndex\":37,\"power\":null,\"reboot\":false,\"bootMode\":null,\"TimingParameters\":{}}"}
    };
    for (unsigned i = 0; i < sizeof(algorithms) / sizeof(algorithms[0]); ++i) {
        mock_power_state = PWR_CTRL_STATE_ON;
        round_command(algorithms[i].command, algorithms[i].json, "accepted", NULL);
        CHECK(dispatch(PC_COMMAND_STOP_TEST, "{\"bad\":1}", 400U, &response) == 1);
        cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 400U);
        string_is(body, "errorCode", "invalidArgument"); cJSON_Delete(body);
        CHECK(!stop_requested);
        queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_COMPLETE};
        response_ready = 1;
        request_stop();
        unsigned queued = queue_calls;
        round_command(PC_COMMAND_STOP_TEST, "{}", "failed", "busy");
        round_command(PC_COMMAND_START_TEST, COLD_START, "failed", "busy");
        CHECK(dispatch(PC_COMMAND_CLIENT_HEARTBEAT, "{}", 401U, &response) == 1);
        body = decode(&response, PC_COMMAND_SERVER_HEARTBEAT, 401U); cJSON_Delete(body);
        complete_stop(1);
        CHECK(queue_calls == queued);
    }

    /* 启动各阶段均可覆盖为停止；尚未提交的算法不能补发。 */
    for (unsigned phase = 0; phase < 5U; ++phase) {
        configure_power();
        unsigned queued = queue_calls;
        round_command(PC_COMMAND_START_TEST, COLD_START, "accepted", NULL);
        if (phase >= 1U) step_power(10U);
        if (phase >= 2U) step_power(5U);
        if (phase == 3U) mock_power_state = PWR_CTRL_STATE_WAIT_SECOND_RAIL;
        if (phase == 4U) { mock_power_state = PWR_CTRL_STATE_ON; step_power(1U); }
        request_stop(); complete_stop(1);
        CHECK(queue_calls == queued);
    }

    /* 普通时序在途停止，剩余时序和算法不再投递。 */
    mock_power_state = PWR_CTRL_STATE_ON;
    const char *timing = "{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,"
        "\"power\":null,\"reboot\":false,\"bootMode\":null,\"TimingParameters\":{\"BL\":\"16\"}}";
    round_command(PC_COMMAND_START_TEST, timing, "accepted", NULL);
    unsigned queued = queue_calls;
    request_stop(); complete_stop(1);
    CHECK(queue_calls == queued);

    /* SLT预约、已进入EDK及RELEASE前均能停止，无需伪造RELEASE。 */
    for (unsigned phase = 0; phase < 2U; ++phase) {
        configure_power();
        round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL);
        if (phase) { slt_complete(0); handoff_test.cancel_allowed = 0; }
        request_stop(); complete_stop(1);
    }

    /* 原PC/串口启动操作不可误消费0104的OFF结果，也不再补旧completed。 */
    configure_power(); power_command(PC_COMMAND_POWER_ON, "accepted", NULL);
    request_stop(); complete_stop(1);
    configure_power(); console_command(LOG_CONSOLE_POWER_START_RK);
    unsigned completions = console_completions;
    request_stop();
    CHECK(console_completions == completions + 1U && strcmp(console_error, "cancelled") == 0);
    complete_stop(1);

    /* 已在反序下电中不能重复驱动、缩短轨间延时。 */
    mock_power_state = PWR_CTRL_STATE_ON;
    power_command(PC_COMMAND_POWER_OFF, "accepted", NULL);
    step_power(0U);
    unsigned off = power_off_calls;
    request_stop();
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_WAIT_OFF);
    complete_stop(1); CHECK(power_off_calls == off);

    /* 下电失败只报一次并保持busy；显式0104可重试。 */
    mock_power_state = PWR_CTRL_STATE_ON;
    power_failure = PWR_CTRL_ERR_RESOURCE_BUSY;
    request_stop(); step_power(0U);
    power_completion(PC_COMMAND_STOP_TEST, "busy");
    CHECK(pc_test_service_busy() && stop_requested);
    power_failure = PWR_CTRL_OK;
    request_stop(); complete_stop(1);

    /* UART/网络恢复未证实时不能提前成功，跨tick回绕也有清理时限。 */
    mock_tick = UINT32_MAX - 100U;
    request_stop(); step_power(0U); step_power(3000U);
    power_completion(PC_COMMAND_STOP_TEST, "timeout");
    CHECK(pc_test_service_busy());
    request_stop(); complete_stop(1);

    /* 断线后继续下电，不向后续连接发送旧0104或算法结果。 */
    mock_power_state = PWR_CTRL_STATE_ON;
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    request_stop(); pc_dispatch_disconnected(); complete_stop(0);
    round_begin(); round_end();
    message("PASS: PC RK service 0104 abort, OFF/cleanup gates, same round, startup preemption, failures and disconnect\n");
    finish(0U);
}
