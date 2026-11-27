/* 复用实际 PC 分发、RK 结果解码和电源编排，只有硬件推进及队列由夹具代替。 */
#define test_main pc_service_baseline_main
#include "test_pc_rk_service.c"
#undef test_main

#define PLAN(axes) "{\"algorithmType\":\"functional\",\"algorithmName\":\"gtest-rand\",\"shmoo\":{" axes "}}"
#define AXIS(key, name, low, high, step) "\"" key "\":{\"type\":\"" name "\",\"min\":" low ",\"max\":" high ",\"step\":" step "}"
#define VOLTAGE AXIS("x", "Vddq", "600.0", "605.0", "5.0")
#define FREQUENCY AXIS("y", "Freq", "400", "500", "100")
#define THREE_AXES PLAN(VOLTAGE "," FREQUENCY "," AXIS("z", "tWTR", "16", "18", "2"))
#define THREE_RAIL_FREQ PLAN(VOLTAGE "," FREQUENCY "," AXIS("z", "Vdd1", "1790.0", "1800.0", "10.0"))
#define TWO_RAILS PLAN(VOLTAGE "," AXIS("y", "vdd1", "1800", "1800", "10"))
#define MEMORY_PLAN(type) "{\"algorithmType\":\"functional\",\"algorithmName\":\"gtest-rand\"," \
    "\"memoryType\":" type ",\"shmoo\":{" AXIS("x", "Vdd1", "1800", "1800", "10") "," \
    AXIS("y", "vdd2", "1100", "1100", "5") "}}"

static void scan_start(const char *json, const char *error)
{
    pc_dispatch_response_t response;
    CHECK(dispatch(PC_COMMAND_SHMOO_START, json, 10U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 10U);
    number_is(body, "requestCommandId", PC_COMMAND_SHMOO_START);
    string_is(body, "status", error ? "failed" : "accepted");
    CHECK(!cJSON_GetObjectItemCaseSensitive(body, "algorithmIndex"));
    if (error) string_is(body, "errorCode", error);
    cJSON_Delete(body);
}

/* 推进一个冷启动。故意重复频率和其它未变化的轴，核对同频点也有新的训练上电。 */
static void scan_boot(int64_t frequency, int reply, int ready)
{
    unsigned previous_ons = power_on_calls;
    unsigned previous_arms = readback_arms;
    pc_dispatch_response_t response;
    for (unsigned n = 0U; n < 20U; ++n) {
        if (pc_power_service_diagnostic()->phase == PC_POWER_WAIT_RK_READY) break;
        if (mock_power_state == PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF) mock_power_state = PWR_CTRL_STATE_OFF;
        if (mock_power_state == PWR_CTRL_STATE_WAIT_FIRST_RAIL) mock_power_state = PWR_CTRL_STATE_ON;
        step_power(5U);
        CHECK(pc_dispatch_next_response(11U, &response) == 0);
    }
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_WAIT_RK_READY);
    CHECK(power_on_calls == previous_ons + 1U);
    CHECK(readback_arms == previous_arms + 1U);
    CHECK(last_boot_mode == BOOT_MODE_FSPI_M1);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD2) == 1100U);
    uint32_t token;
    CHECK(rk_link_frequency_get(&token) == frequency);
    if (frequency >= 0) CHECK(token != 0U);
    if (reply) rk_link_frequency_note_tx(token, 1);
    if (ready) confirm_power(1);
    step_power(ready ? 1U : sequence_config.ready_timeout_ms + 1U);
}

static void scan_algorithm(unsigned timing)
{
    pc_dispatch_response_t response;
    unsigned previous_done = readback_settings_done;
    CHECK(readback_deferred == (timing != 0U));
    CHECK(pc_dispatch_next_response(12U, &response) == 0);
    if (timing) {
        CHECK(queued_request.command_id == 0x2FU && queued_request.parameter_value == timing);
        queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_SENT};
        response_ready = 1;
        CHECK(pc_dispatch_next_response(12U, &response) == 0);
        CHECK(readback_settings_done == previous_done + 1U);
    }
    CHECK(queued_request.command_id == 2U && queued_request.timeout_ms == 0U);
}

/* z为0的用例是二维，结果必须省略z；三维用例在这里明确使用非零tWTR档位。 */
static void scan_report(unsigned x, unsigned y, unsigned z, unsigned failed, int last)
{
    pc_dispatch_response_t response;
    int result = pc_dispatch_next_response(20U, &response);
    if (last && mock_power_state != PWR_CTRL_STATE_OFF) {
        CHECK(result == PC_DISPATCH_NO_RESPONSE);
        complete_test_power_off();
        result = pc_dispatch_next_response(20U, &response);
    }
    CHECK(result == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_SHMOO_RESULT, 20U);
    CHECK(cJSON_GetArraySize(body) == 4);
    string_is(body, "algorithmName", "gtest-rand");
    string_is(body, "algorithmType", "functional");
    string_is(body, "shmooprogress", last ? "completed" : "running");
    cJSON *results = cJSON_GetObjectItemCaseSensitive(body, "testResults");
    CHECK(cJSON_IsArray(results) && cJSON_GetArraySize(results) == 2);
    for (unsigned die = 0U; die < 2U; ++die) {
        cJSON *entry = cJSON_GetArrayItem(results, die);
        CHECK(cJSON_IsObject(entry) && cJSON_GetArraySize(entry) == 3);
        number_is(entry, "mirrorIndex", die);
        number_is(entry, "testStatus", (failed & (1U << die)) ? 0U : 1U);
        cJSON *point = cJSON_GetObjectItemCaseSensitive(entry, "shmoopoint");
        CHECK(cJSON_IsObject(point) && cJSON_GetArraySize(point) == (z ? 3 : 2));
        number_is(point, "x", x);
        number_is(point, "y", y);
        if (z) number_is(point, "z", z);
    }
    cJSON_Delete(body);
    unsigned on = power_on_calls, off = power_off_calls;
    step_power(5U);
    CHECK(!pc_dispatch_next_response(21U, &response) && power_on_calls == on && power_off_calls == off);
    pc_dispatch_response_sent(PC_COMMAND_SHMOO_RESULT);
}

static void scan_grid(const char *json, unsigned z_start, unsigned z_step, bool voltage_z)
{
    configure_power();
    scan_start(json, NULL);
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    round_command(PC_COMMAND_ROUND_END, "{\"failChips\":[]}", "failed", "invalidState");
    unsigned total = z_start ? 8U : 2U;
    unsigned off = power_off_calls;
    for (unsigned i = 0U; i < total; ++i) {
        unsigned x = 600U + (i % 2U) * 5U;
        unsigned y = z_start ? 400U + ((i / 2U) % 2U) * 100U : 1800U;
        unsigned z = z_start + (i / 4U) * z_step;
        unsigned timing = voltage_z ? 0U : z;
        unsigned queued = queue_calls;
        char status[256];
        pc_test_format_status(status, sizeof(status));
        CHECK(strstr(status, "type=shmoo algorithm=gtest-rand") && strstr(status, "progress=--"));
        char expected[80];
        (void)snprintf(expected, sizeof(expected), "completedPoints=%u/%u x=%u y=%u", i, total, x, y);
        CHECK(strstr(status, expected));
        CHECK((strstr(status, " z=") != NULL) == (z != 0U));
        scan_boot(z_start ? (int64_t)y : -1, !!z_start, 1);
        CHECK(ddr_power_get_memory_type() == DDR_MEMORY_LPDDR4X);
        CHECK(ddr_power_get_training_mv(DDR_RAIL_VDDQ) == x);
        CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD1) == (voltage_z ? z : 1800U));
        scan_algorithm(timing);
        mock_progress = 37;
        pc_test_format_status(status, sizeof(status));
        CHECK(strstr(status, "phase=running progress=37%"));
        mock_progress = -1;
        CHECK(queue_calls == queued + (timing ? 2U : 1U));
        unsigned failed = i % 2U ? 2U : 0U;
        inject_mask(RK_RESPONSE_COMPLETE, failed, 0U, 0U);
        if (i == 0U) {
            /* PC结果组包暂时缺内存，下次发送仍对应同一个已完成坐标。 */
            pc_dispatch_response_t response;
            cJSON_Hooks hooks = {no_memory, free};
            cJSON_InitHooks(&hooks);
            CHECK(pc_dispatch_next_response(20U, &response) == PC_DISPATCH_NO_RESPONSE);
            cJSON_InitHooks(NULL);
            CHECK(queue_calls == queued + (timing ? 2U : 1U));
        }
        scan_report(x, y, z, failed, i + 1U == total);
    }
    CHECK(power_off_calls == off + total && mock_power_state == PWR_CTRL_STATE_OFF && !pc_test_service_busy());
}

static void scan_validation(void)
{
    const char *const invalid[] = {
        PLAN(VOLTAGE),
        PLAN(VOLTAGE "," AXIS("y", "vddq", "600", "600", "5")),
        PLAN(VOLTAGE "," AXIS("y", "Vdd1", "700.0", "1100.0", "50.0")),
        PLAN(AXIS("x", "vdd2", "1100", "1110", "3") "," FREQUENCY),
        PLAN(AXIS("x", "vddq", "600.5", "605", "5") "," FREQUENCY),
        PLAN(AXIS("x", "vddq", "\"600\"", "605", "5") "," FREQUENCY),
        PLAN(AXIS("x", "vddq", "605", "600", "5") "," FREQUENCY),
        PLAN(AXIS("x", "vddq", "600", "605", "0") "," FREQUENCY)
    };
    unsigned on = power_on_calls, off = power_off_calls, before = queue_calls;
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        scan_start(invalid[i], "invalidArgument");
    CHECK(power_on_calls == on && power_off_calls == off && queue_calls == before);
    round_command(PC_COMMAND_SHMOO_RESULT, "{}", "failed", "invalidDirection");
}

static void scan_memory_types(void)
{
    const struct { const char *json; ddr_memory_type_t type; unsigned vddq; } cases[] = {
        {MEMORY_PLAN("\"LPDDR4\""), DDR_MEMORY_LPDDR4, 1100U},
        {MEMORY_PLAN("\"LPDDR4X\""), DDR_MEMORY_LPDDR4X, 600U}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        configure_power();
        scan_start(cases[i].json, NULL);
        scan_boot(-1, 0, 1);
        CHECK(ddr_power_get_memory_type() == cases[i].type);
        CHECK(ddr_power_get_training_mv(DDR_RAIL_VDDQ) == cases[i].vddq);
        scan_algorithm(0U);
        inject(RK_RESPONSE_COMPLETE, 0U);
        scan_report(1800U, 1100U, 0U, 0U, 1);
    }
    scan_start(MEMORY_PLAN("42"), "invalidArgument");
    scan_start(MEMORY_PLAN("\"DDR5\""), "invalidArgument");
}

static void scan_cleanup(void)
{
    pc_dispatch_response_t response;
    configure_power();
    scan_start(TWO_RAILS, NULL);
    scan_boot(-1, 0, 1);
    scan_algorithm(0U);
    CHECK(dispatch(PC_COMMAND_STOP_TEST, "{}", 30U, &response) == PC_DISPATCH_NO_RESPONSE);
    unsigned queued = queue_calls, on = power_on_calls;
    step_power(1U);
    mock_power_state = PWR_CTRL_STATE_OFF;
    handoff_test.reserved = 0;
    stop_ready = 1;
    step_power(10U);
    power_completion(PC_COMMAND_STOP_TEST, NULL);
    CHECK(!pc_test_service_busy() && queue_calls == queued && power_on_calls == on);

    /* 断线前已发出的点可以收尾；禁止向新连接交付或启动下一点。 */
    configure_power();
    scan_start(TWO_RAILS, NULL);
    scan_boot(-1, 0, 1);
    scan_algorithm(0U);
    queued = queue_calls; on = power_on_calls;
    pc_dispatch_disconnected();
    inject(RK_RESPONSE_COMPLETE, 0U);
    step_power(10U);
    CHECK(!pc_dispatch_next_response(31U, &response));
    CHECK(queue_calls == queued && power_on_calls == on && !pc_test_service_busy());

    /* 010C关闭站点期间，当前扫描和再送的010E都不能触发下一次冷启动。 */
    configure_power();
    scan_start(TWO_RAILS, NULL);
    const char *close = "{\"testerId\":2,\"siteId\":3,\"siteEnabled\":false,\"dutEnabled\":[]}";
    round_command(PC_COMMAND_SITE_ENABLE, close, "accepted", NULL);
    scan_start(TWO_RAILS, "siteDisabled");
    step_power(1U);
    mock_power_state = PWR_CTRL_STATE_OFF;
    handoff_test.reserved = 0;
    stop_ready = 1;
    step_power(10U);
    power_completion(PC_COMMAND_SITE_ENABLE, NULL);
    CHECK(power_on_calls == on && queue_calls == queued && !pc_test_service_busy());
    const char *open = "{\"testerId\":2,\"siteId\":3,\"siteEnabled\":true,\"dutEnabled\":["
                       "{\"mirrorIndex\":0,\"dut\":[true]},{\"mirrorIndex\":1,\"dut\":[true]}]}";
    round_command(PC_COMMAND_SITE_ENABLE, open, "completed", NULL);
}

static void scan_boot_failure(void)
{
    pc_dispatch_response_t response;
    configure_power();
    scan_start(TWO_RAILS, NULL);
    unsigned queued = queue_calls;
    scan_boot(-1, 0, 0);
    complete_test_power_off();
    scan_report(600U, 1800U, 0U, 3U, 0);
    CHECK(queue_calls == queued && mock_power_state == PWR_CTRL_STATE_OFF);
    scan_boot(-1, 0, 1);
    scan_algorithm(0U);
    inject(RK_RESPONSE_COMPLETE, 0U);
    scan_report(605U, 1800U, 0U, 0U, 1);

    /* 最后一个点同样可能训练失败。只回这一个completed点，真实OFF后不再上电。 */
    configure_power();
    scan_start(MEMORY_PLAN("\"LPDDR4X\""), NULL);
    queued = queue_calls;
    scan_boot(-1, 0, 0);
    unsigned on = power_on_calls;
    scan_report(1800U, 1100U, 0U, 3U, 1);
    step_power(1000U);
    CHECK(power_on_calls == on && queue_calls == queued && !pc_test_service_busy());
    CHECK(pc_dispatch_next_response(32U, &response) == PC_DISPATCH_NO_RESPONSE);
    if (B1_RK_FREQ_ENABLE) {
        configure_power();
        scan_start(PLAN(VOLTAGE "," FREQUENCY), NULL);
        queued = queue_calls;
        scan_boot(400, 0, 1);
        CHECK(pc_dispatch_next_response(32U, &response) == PC_DISPATCH_RESPONSE_READY);
        cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 32U);
        number_is(body, "requestCommandId", PC_COMMAND_SHMOO_START);
        string_is(body, "status", "failed");
        string_is(body, "errorCode", "notSupported");
        cJSON_Delete(body);
        CHECK(queue_calls == queued && !pc_test_service_busy());
    }
}

/* 末点下电失败应报告010E操作失败；离线期间继续下电，但不把010F交给新连接。 */
static void scan_final_shutdown(void)
{
    for (unsigned mode = 0U; mode < 3U; ++mode) {
        configure_power();
        scan_start(MEMORY_PLAN("\"LPDDR4X\""), NULL);
        scan_boot(-1, 0, 1);
        scan_algorithm(0U);
        pc_dispatch_response_t response;
        if (mode == 1U) pc_dispatch_disconnected(); /* 算法执行时原PC离开。 */
        inject(RK_RESPONSE_COMPLETE, 0U);
        CHECK(pc_dispatch_next_response(33U, &response) == PC_DISPATCH_NO_RESPONSE);
        if (mode == 0U) {
            power_failure = PWR_CTRL_ERR_FAULT;
            step_power(0U);
            CHECK(pc_dispatch_next_response(33U, &response) == PC_DISPATCH_RESPONSE_READY);
            cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 33U);
            number_is(body, "requestCommandId", PC_COMMAND_SHMOO_START);
            string_is(body, "status", "failed");
            string_is(body, "errorCode", "executionFailed");
            cJSON_Delete(body);
            CHECK(mock_power_state == PWR_CTRL_STATE_ON);
            power_failure = PWR_CTRL_OK;
        } else {
            if (mode == 2U) pc_dispatch_disconnected(); /* 已开始收尾，OFF尚未到达。 */
            complete_test_power_off();
            CHECK(mock_power_state == PWR_CTRL_STATE_OFF);
        }
        CHECK(pc_dispatch_next_response(34U, &response) == PC_DISPATCH_NO_RESPONSE);
        CHECK(!pc_test_service_busy() && !pc_power_service_busy());
    }
}

static void scan_then_regular_test(void)
{
    configure_power();
    scan_start(PLAN(AXIS("x", "vddq", "600", "600", "5") ","
                    AXIS("y", "tWTR", "16", "16", "1")), NULL);
    scan_boot(-1, 0, 1);
    scan_algorithm(16U);
    inject(RK_RESPONSE_COMPLETE, 0U);
    scan_report(600U, 16U, 0U, 0U, 1);

    /* 普通0103使用相同的电压和Boot、reboot=false；仍应冷启动清除扫描留下的tWTR。 */
    round_begin();
    cJSON *body = cJSON_Parse(COLD_START); CHECK(body);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(body, "bootMode", cJSON_CreateString("fspiM1")));
    char *json = cJSON_PrintUnformatted(body); CHECK(json);
    round_command(PC_COMMAND_START_TEST, json, "accepted", NULL);
    cJSON_free(json); cJSON_Delete(body);
    cold_to_ready(); ready_starts_algorithm();
    result_is(0U, NULL, site_label);
    round_end();
}

void test_main(void)
{
    power_fixture = 1;
    configure_power();
    /* 普通整轮独占，结束后010E可直接运行。即使前轮曾写非标称电压，扫描只覆盖选定轴。 */
    round_begin();
    scan_start(TWO_RAILS, "busy");
    round_end();
    const uint16_t previous_targets[] = {1860U, 1145U, 625U};
    CHECK(ddr_power_configure_training(DDR_MEMORY_LPDDR4, previous_targets) == IF_OK);
    scan_validation();
    scan_grid(TWO_RAILS, 0U, 0U, false);
    scan_memory_types();
    if (B1_RK_FREQ_ENABLE) scan_grid(THREE_RAIL_FREQ, 1790U, 10U, true);
    if (B1_RK_TIMING_ENABLE && B1_RK_FREQ_ENABLE) scan_grid(THREE_AXES, 16U, 2U, false);
    else scan_start(THREE_AXES, "notSupported");
    if (!B1_RK_FREQ_ENABLE) scan_start(PLAN(VOLTAGE "," FREQUENCY), "notSupported");
    if (!B1_RK_TIMING_ENABLE)
        scan_start(PLAN(VOLTAGE "," AXIS("y", "tWTR", "16", "20", "2")), "notSupported");
    scan_boot_failure();
    scan_final_shutdown();
    if (B1_RK_TIMING_ENABLE) scan_then_regular_test();
    scan_cleanup();
    round_begin(); round_end();
    message("PASS: PC RK service Shmoo 010E/010F cold points, axes, mirrors, stop and ownership\n");
    finish(0U);
}
