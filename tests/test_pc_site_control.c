/* 通过正式PC分发驱动010C；模拟电源、UART清理和GPIO端口，逐步确认应答时点。 */
#define test_main pc_service_baseline_main
#include "../../test/tests/test_pc_rk_service.c"
#undef test_main
#include "pc_site_control.h"
#include "board_network_config.h"
#include "rk_ddr_capacity.h"

#define SITE_TARGET(tester, site, enabled, groups) \
    "{\"testerId\":" tester ",\"siteId\":" site ",\"siteEnabled\":" enabled ",\"dutEnabled\":" groups "}"
#define SITE_BODY(enabled, groups) SITE_TARGET("2", "3", enabled, groups)
#define GROUP(index, dut) "{\"mirrorIndex\":" index ",\"dut\":" dut "}"
#define ONLY_FIRST "[" GROUP("1","[false]") "," GROUP("0","[true]") "]"
#define OPEN_TARGET(tester, site) SITE_TARGET(tester, site, "true", ONLY_FIRST)
#define CLOSE_SITE SITE_BODY("false", "null")
#define OPEN_FIRST SITE_BODY("true", ONLY_FIRST)
#define OPEN_NEITHER SITE_BODY("true", "[" GROUP("0","[false]") "," GROUP("1","[false]") "]")

static void site_request(const char *json, const char *status, const char *error)
{
    round_command(PC_COMMAND_SITE_ENABLE, json, status, error);
}

static void site_reply(uint16_t command, const char *error)
{
    pc_dispatch_response_t response;
    CHECK(pc_dispatch_next_response(92U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 92U);
    number_is(body, "requestCommandId", command);
    string_is(body, "status", error ? "failed" : "completed");
    if (error) string_is(body, "errorCode", error);
    cJSON_Delete(body);
}

static void no_reply(void)
{
    pc_dispatch_response_t response;
    CHECK(pc_dispatch_next_response(93U, &response) == PC_DISPATCH_NO_RESPONSE);
}

static void confirm_shutdown(void)
{
    unsigned on = power_on_calls;
    CHECK(!pc_site_enabled() && !site_ready_led && !visible_leds && stop_requested);
    step_power(0U);
    mock_power_state = PWR_CTRL_STATE_OFF;
    step_power(10U);
    no_reply();
    handoff_test.reserved = 0;
    no_reply();
    response_ready = 0; /* RK队列由实际停止流程清理。 */
    stop_ready = 1;
    step_power(0U);
    CHECK(mock_power_state == PWR_CTRL_STATE_OFF && power_on_calls == on);
}

static void check_format(void)
{
    static const char *const invalid[] = {
        "{}", "{\"testerId\":2,\"siteId\":3,\"siteEnabled\":false}", SITE_BODY("0", "null"),
        SITE_BODY("true", "null"), SITE_BODY("true", "[]"),
        SITE_BODY("true", "[" GROUP("0","[true]") "," GROUP("0","[false]") "]"),
        SITE_BODY("true", "[" GROUP("0","[true]") "," GROUP("2","[false]") "]"),
        SITE_BODY("true", "[" GROUP("0","[1]") "," GROUP("1","[true]") "]"),
        SITE_BODY("true", "[" GROUP("0","[true,false]") "," GROUP("1","[true]") "]"),
        "{\"siteId\":3,\"siteEnabled\":false,\"dutEnabled\":[]}",
        "{\"testerId\":2,\"siteEnabled\":false,\"dutEnabled\":[]}",
        SITE_TARGET("1.5", "3", "false", "[]"), SITE_TARGET("2", "-1", "false", "[]"),
        SITE_TARGET("65536", "3", "false", "[]"), SITE_TARGET("2", "65536", "false", "[]"),
        SITE_TARGET("\"2\"", "3", "false", "[]"), SITE_TARGET("2", "null", "false", "[]"),
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        site_request(invalid[i], "failed", "invalidArgument");
    CHECK(pc_site_enabled() && !stop_requested && visible_leds == 7U);
}

static void check_target(const char *json, int rejected)
{
    site_request(json, rejected ? "failed" : "completed", rejected ? "invalidArgument" : NULL);
}

static void check_target_switches(void)
{
    /* 单板直连时验证零值、物理上下界和 uint16 最大值。 */
    b2_site = 0U;
    check_target(OPEN_TARGET("1", "1"), 0);
    check_target(OPEN_TARGET("21", "8"), 0);
    check_target(OPEN_TARGET("0", "3"), B1_010C_CHECK_TESTER_ID);
    check_target(OPEN_TARGET("2", "0"), B1_010C_CHECK_SITE_ID);
    check_target(OPEN_TARGET("22", "3"), B1_010C_CHECK_TESTER_ID);
    check_target(OPEN_TARGET("2", "9"), B1_010C_CHECK_SITE_ID);
    check_target(OPEN_TARGET("65535", "65535"), B1_010C_CHECK_TESTER_ID || B1_010C_CHECK_SITE_ID);

    /* B2 分配为 2/3 后，分别让一个合法编号与本机不符。 */
    b2_site = 3U;
    check_target(OPEN_TARGET("1", "3"), B1_010C_CHECK_TESTER_ID);
    check_target(OPEN_TARGET("2", "4"), B1_010C_CHECK_SITE_ID);
    check_target(OPEN_TARGET("2", "3"), 0);
    CHECK(pc_site_enabled() && !stop_requested);
}

static void check_disabled_commands(void)
{
    const uint16_t blocked[] = {PC_COMMAND_QUERY_TESTER_INFO, PC_COMMAND_STOP_TEST, PC_COMMAND_ROUND_BEGIN,
        PC_COMMAND_ROUND_END, PC_COMMAND_PARA_START, PC_COMMAND_SLT_START, PC_COMMAND_POWER_ON,
        PC_COMMAND_RESET_RK, PC_COMMAND_QUERY_POWER_STATUS, PC_COMMAND_QUERY_POWER_MONITOR};
    for (unsigned i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i)
        round_command(blocked[i], "{}", "failed", "siteDisabled");
    pc_dispatch_response_t response;
    CHECK(dispatch(PC_COMMAND_START_TEST, START_JSON, 44U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 44U);
    number_is(body, "algorithmIndex", 37U);
    string_is(body, "errorCode", "siteDisabled");
    cJSON_Delete(body);
    CHECK(dispatch(PC_COMMAND_CLIENT_HEARTBEAT, "{}", 45U, &response) == 1);
    body = decode(&response, PC_COMMAND_SERVER_HEARTBEAT, 45U);
    CHECK(!body->child); cJSON_Delete(body);
    console_command(LOG_CONSOLE_POWER_START_RK);
    CHECK(console_error && strcmp(console_error, "siteDisabled") == 0);
}

void test_main(void)
{
    pc_dispatch_response_t response;
    power_fixture = 1;
    configure_power();
    check_format();
    check_target_switches();
    /* DUT1灯被屏蔽时，FUNC仍产生含DUT1失败地址的两颗结果。 */
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    deliver_round_chip_info(2048U);
    mock_power_state = PWR_CTRL_STATE_ON;
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    unsigned queued = queue_calls;
    site_request(OPEN_FIRST, "completed", NULL);
    CHECK(pc_test_service_busy() && !stop_requested && visible_leds == 5U && site_ready_led);
    CHECK(rk_ddr_capacity_snapshot().current_mib == 8192U);
    CHECK(queue_calls == queued && power_off_calls == 0U);
    inject_mask(RK_RESPONSE_COMPLETE, 2U, 0U, 0x42U);
    CHECK(pc_dispatch_next_response(90U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 90U);
    const cJSON *dies = cJSON_GetObjectItemCaseSensitive(body, "testResults");
    CHECK(cJSON_GetArraySize(dies) == 2);
    number_is(cJSON_GetArrayItem(dies, 0), "testStatus", 1U);
    number_is(cJSON_GetArrayItem(dies, 1), "testStatus", 0U);
    cJSON_Delete(body);

    /* IDD在途关站丢弃尚未交付结果。下电途中开站只记住最新显示，不能提前就绪。 */
    char *para = para_json("parametric", "idd_2NS", 7U);
    round_command(PC_COMMAND_PARA_START, para, "accepted", NULL);
    cJSON_free(para);
    queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_COMPLETE};
    response_ready = 1;
    const char *close_zero = SITE_TARGET("0", "0", "false", "[{}]");
    if (B1_010C_CHECK_TESTER_ID || B1_010C_CHECK_SITE_ID) {
        site_request(close_zero, "failed", "invalidArgument");
        CHECK(pc_site_enabled() && pc_test_service_busy() && !stop_requested);
        site_request(SITE_BODY("false", "[{}]"), "accepted", NULL);
    } else {
        site_request(close_zero, "accepted", NULL);
    }
    CHECK(!rk_ddr_capacity_snapshot().round_active && !rk_ddr_capacity_snapshot().has_completed);
    site_request(OPEN_FIRST, "accepted", NULL);
    check_disabled_commands();
    confirm_shutdown();
    site_reply(PC_COMMAND_SITE_ENABLE, NULL);
    site_reply(PC_COMMAND_SITE_ENABLE, NULL);
    no_reply();
    CHECK(pc_site_enabled() && !pc_test_service_busy() && site_ready_led && visible_leds == 5U);
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "invalidState");
    round_begin();
    CHECK(power_on_calls == 0U);

    /* 开站后的下一项SLT按既有参数上电；EDK尚未RELEASE也能被关站抢占。 */
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL);
    slt_complete(0);
    CHECK(power_on_calls == 1U && handoff_test.reserved);
    unsigned off = power_off_calls;
    site_request(CLOSE_SITE, "accepted", NULL);
    site_request(OPEN_FIRST, "accepted", NULL);
    site_request(CLOSE_SITE, "accepted", NULL);
    confirm_shutdown();
    site_reply(PC_COMMAND_SITE_ENABLE, NULL);
    site_reply(PC_COMMAND_SITE_ENABLE, "cancelled");
    site_reply(PC_COMMAND_SITE_ENABLE, NULL);
    no_reply();
    CHECK(!pc_site_enabled() && power_off_calls == off + 1U);
    site_request(CLOSE_SITE, "completed", NULL);
    pc_dispatch_disconnected();
    check_disabled_commands();
    site_request(OPEN_NEITHER, "completed", NULL);
    CHECK(site_ready_led && visible_leds == 4U && power_on_calls == 1U);

    /* 加入已受理的0104，两条命令各回自己的终态，只执行一次物理下电。 */
    mock_power_state = PWR_CTRL_STATE_ON;
    CHECK(dispatch(PC_COMMAND_STOP_TEST, "{}", 99U, &response) == PC_DISPATCH_NO_RESPONSE);
    site_request(CLOSE_SITE, "accepted", NULL);
    confirm_shutdown();
    site_reply(PC_COMMAND_STOP_TEST, NULL);
    site_reply(PC_COMMAND_SITE_ENABLE, NULL);
    no_reply();
    CHECK(!pc_site_enabled());
    site_request(OPEN_FIRST, "completed", NULL);

    /* 清理失败保留闭站；显式重试并断线后仍兑现开站，不向新连接补终态。 */
    mock_power_state = PWR_CTRL_STATE_ON;
    power_failure = PWR_CTRL_ERR_RESOURCE_BUSY;
    site_request(CLOSE_SITE, "accepted", NULL);
    site_request(OPEN_FIRST, "accepted", NULL);
    step_power(0U);
    site_reply(PC_COMMAND_SITE_ENABLE, "busy");
    site_reply(PC_COMMAND_SITE_ENABLE, "busy");
    CHECK(!pc_site_enabled() && pc_test_service_busy());
    power_failure = PWR_CTRL_OK;
    site_request(OPEN_FIRST, "accepted", NULL);
    pc_dispatch_disconnected();
    confirm_shutdown(); no_reply();
    CHECK(pc_site_enabled() && site_ready_led && !pc_test_service_busy() && power_on_calls == 1U);

    /* 启动准备中关站，两条TCP连接不能领到彼此的应答，旧算法也不能再入队。 */
    round_begin();
    round_command(PC_COMMAND_START_TEST, COLD_START, "accepted", NULL);
    queued = queue_calls;
    site_request(CLOSE_SITE, "accepted", NULL);
    pc_dispatch_disconnected();
    site_request(OPEN_NEITHER, "accepted", NULL);
    confirm_shutdown();
    site_reply(PC_COMMAND_SITE_ENABLE, NULL); no_reply();
    CHECK(pc_site_enabled() && visible_leds == 4U && queue_calls == queued && power_on_calls == 1U);

    /* 每个accepted都有独立终态；超过16个待应答的位置不能覆盖已受理的配置。 */
    site_request(CLOSE_SITE, "accepted", NULL);
    for (unsigned i = 1; i < 16U; ++i) site_request(OPEN_FIRST, "accepted", NULL);
    site_request(OPEN_NEITHER, "failed", "busy");
    confirm_shutdown();
    for (unsigned i = 0; i < 16U; ++i) site_reply(PC_COMMAND_SITE_ENABLE, NULL);
    no_reply();
    CHECK(pc_site_enabled() && visible_leds == 5U && power_on_calls == 1U);
    message("PASS: PC RK service 010C target, DUT results, command gate, abort, queued reopen, failure and reconnect\n");
    finish(0U);
}
