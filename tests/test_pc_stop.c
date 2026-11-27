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

/* 0104和0107可合用一轮下电，两个成功应答都必须等轨关断及RK清理。 */
static void round_end_joins_stop(void)
{
    pc_dispatch_response_t response;
    configure_power(); round_begin();
    mock_power_state = PWR_CTRL_STATE_ON;
    unsigned off = power_off_calls;
    request_stop();
    CHECK(dispatch(PC_COMMAND_ROUND_END, "{\"failChips\":[]}", 401U, &response) == PC_DISPATCH_NO_RESPONSE);
    step_power(0U);
    CHECK(power_off_calls == off + 1U && pc_dispatch_next_response(402U, &response) == 0);
    mock_power_state = PWR_CTRL_STATE_OFF;
    step_power(10U);
    CHECK(pc_dispatch_next_response(402U, &response) == 0);
    handoff_test.reserved = 0;
    stop_ready = 1;
    CHECK(pc_dispatch_next_response(402U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 402U);
    number_is(body, "requestCommandId", PC_COMMAND_STOP_TEST);
    string_is(body, "status", "completed"); cJSON_Delete(body);
    power_completion(PC_COMMAND_ROUND_END, NULL);
    CHECK(!stop_requested && !pc_test_service_busy() && power_off_calls == off + 1U);
}

/* 已受理0502时整轮结束接管同一次OFF；断线后完成清理，且不向新连接送旧应答。 */
static void round_end_owns_power_off(void)
{
    pc_dispatch_response_t response;
    configure_power(); round_begin();
    mock_power_state = PWR_CTRL_STATE_ON;
    unsigned off = power_off_calls;
    power_command(PC_COMMAND_POWER_OFF, "accepted", NULL);
    step_power(0U);
    CHECK(dispatch(PC_COMMAND_ROUND_END, "{\"failChips\":[]}", 401U, &response) == PC_DISPATCH_NO_RESPONSE);
    pc_dispatch_disconnected();
    complete_stop(0);
    CHECK(power_off_calls == off + 1U);

    round_begin();
    mock_power_state = PWR_CTRL_STATE_ON;
    power_failure = PWR_CTRL_ERR_RESOURCE_BUSY;
    CHECK(dispatch(PC_COMMAND_ROUND_END, "{\"failChips\":[]}", 401U, &response) == PC_DISPATCH_NO_RESPONSE);
    step_power(0U);
    power_completion(PC_COMMAND_ROUND_END, "busy");
    CHECK(pc_test_service_busy() && stop_requested && mock_power_state == PWR_CTRL_STATE_ON);
    power_failure = PWR_CTRL_OK;
    request_stop(); complete_stop(1);
}

/* 人工强制下电复用0104清理。控制台回执与PC连接、被抢占的普通电源回执各自独立。 */
static void console_force_stop(void)
{
    pc_dispatch_response_t response;
    configure_power(); round_begin();
    mock_power_state = PWR_CTRL_STATE_ON;
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_COMPLETE};
    response_ready = 1;
    unsigned completed = console_force_completions;
    console_force_request = 1;
    pc_dispatch_poll();
    CHECK(stop_requested && console_force_completions == completed);
    complete_stop(0);
    CHECK(console_force_completions == ++completed && !console_force_error);
    mock_power_state = PWR_CTRL_STATE_ON;
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL);
    console_force_request = 1;
    pc_dispatch_poll();
    pc_dispatch_disconnected();
    complete_stop(0);
    CHECK(console_force_completions == ++completed && !console_force_error);

    /* 手动启动可被独立的强制下电请求抢占，下一次手动启动仍能正常受理。 */
    configure_power();
    unsigned power_completed = console_completions;
    console_command(LOG_CONSOLE_POWER_START_RK);
    step_power(sequence_config.off_hold_ms);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_BOOT_SETTLE);
    unsigned pending_on = power_on_calls;
    mock_tick += sequence_config.boot_settle_ms;
    console_force_request = 1;
    pc_dispatch_poll();
    CHECK(power_on_calls == pending_on);
    CHECK(console_completions == power_completed + 1U && strcmp(console_error, "cancelled") == 0);
    complete_stop(0);
    CHECK(console_force_completions == ++completed && !console_force_error);
    console_command(LOG_CONSOLE_POWER_START_RK);
    console_cold_finish(DDR_MEMORY_LPDDR4X);

    /* 下电错误只通知控制台。再次发强制下电可重试，并与既有0104共用清理。 */
    power_failure = PWR_CTRL_ERR_RESOURCE_BUSY;
    console_force_request = 1;
    pc_dispatch_poll(); step_power(0U);
    CHECK(console_force_completions == ++completed && strcmp(console_force_error, "busy") == 0);
    CHECK(pc_test_service_busy() && pc_dispatch_next_response(401U, &response) == 0);
    power_failure = PWR_CTRL_OK;
    request_stop();
    console_force_request = 1;
    pc_dispatch_poll();
    complete_stop(1);
    CHECK(console_force_completions == ++completed && !console_force_error);

    /* 独立010E中止后停止推进下一点，也不补发010F。 */
    const char *plan = "{\"algorithmType\":\"functional\",\"algorithmName\":\"gtest-rand\","
        "\"shmoo\":{\"x\":{\"type\":\"vddq\",\"min\":600,\"max\":605,\"step\":5},"
        "\"y\":{\"type\":\"vdd1\",\"min\":1800,\"max\":1800,\"step\":10}}}";
    CHECK(dispatch(PC_COMMAND_SHMOO_START, plan, 403U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *ack = decode(&response, PC_COMMAND_COMMAND_RESP, 403U);
    string_is(ack, "status", "accepted"); cJSON_Delete(ack);
    unsigned on = power_on_calls;
    console_force_request = 1;
    pc_dispatch_poll();
    complete_stop(0);
    CHECK(console_force_completions == ++completed && !console_force_error && on == power_on_calls);
}

/* 串口请求由业务轮询取走；最终文本归调试端，不能成为8888的新响应。 */
static void console_func_command(const char *name, const char *expected_error)
{
    unsigned finished = console_func_completions;
    CHECK(!console_func_name[0] && strlen(name) < sizeof(console_func_name));
    strcpy(console_func_name, name);
    pc_dispatch_poll();
    CHECK(!console_func_name[0]);
    if (expected_error) {
        CHECK(console_func_completions == finished + 1U);
        CHECK(strstr(console_func_reply, name) && strstr(console_func_reply, expected_error));
    } else {
        CHECK(console_func_completions == finished && pc_test_service_busy());
    }
}

static void console_func_check(void)
{
    pc_dispatch_response_t response;
    unsigned queued = queue_calls, on = power_on_calls;
    console_func_command("gtest-rand", "powerOff");
    CHECK(power_on_calls == on);
    mock_power_state = PWR_CTRL_STATE_ON;
    console_func_command("gtest-rand", "notReady");
    console_spl_ready = 1;
    console_func_command("idd_2P", "notSupported");
    console_func_command("gtest-unknown", "notSupported");
    readback_held = 1;
    console_func_command("gtest-rand", "busy");
    readback_held = 0;
    CHECK(queue_calls == queued);
    queue_full = 1;
    console_func_command("gtest-rand", "busy");
    queue_full = 0;
    CHECK(queue_calls == ++queued && !pc_test_service_busy());

    for (unsigned run = 0U; run < 2U; ++run) {
        console_func_command("gtest-rand", NULL);
        CHECK(queued_request.command_id == 2U && queued_request.timeout_ms == 0U);
        CHECK(queue_calls == ++queued);
        round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
        unsigned finished = console_func_completions;
        if (run) pc_dispatch_disconnected();
        inject_mask(RK_RESPONSE_COMPLETE, run ? 2U : 0U, 0U, 0U);
        if (run) pc_dispatch_poll();
        CHECK(pc_dispatch_next_response(1U, &response) == 0 && !pc_test_service_busy());
        CHECK(console_func_completions == finished + 1U && strstr(console_func_reply, "gtest-rand"));
        CHECK(strstr(console_func_reply, "die0=PASS"));
        CHECK(strstr(console_func_reply, run ? "die1=FAIL@0x0000000000000000" : "die1=PASS"));
    }

    /* 正式整轮开始后，即便尚无PC算法在途，也不让调试命令占用RK。 */
    round_begin();
    console_func_command("gtest-rand", "busy");
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    console_func_command("gtest-rand", "busy");
    result_is(0U, NULL, "SITE-7");
    round_end();
    mock_power_state = PWR_CTRL_STATE_ON;
    console_spl_ready = 1;
    console_func_command("gtest-rand", NULL);
    unsigned finished = console_func_completions;
    console_force_request = 1;
    pc_dispatch_poll();
    complete_stop(0);
    CHECK(console_func_completions == finished + 1U && strstr(console_func_reply, "stopped"));
    console_spl_ready = 0;
    message("PASS: PC RK service console FUNC, mirrored results, boot and round gates, 8888 isolation and force stop\n");
}

void test_main(void)
{
    pc_dispatch_response_t response;
    power_fixture = 1;
    configure_power();
    console_func_check();
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
    round_end_joins_stop();
    round_end_owns_power_off();
    console_force_stop();
    message("PASS: PC RK service 0104 and console force OFF, cleanup gates, same round, startup preemption, failures and disconnect\n");
    finish(0U);
}
