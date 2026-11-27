/* B1首板联调的轨间时序，单位ms；只配置参数，不在MCU启动时自动开电。 */
#ifndef BOARD_POWER_CONFIG_H
#define BOARD_POWER_CONFIG_H

#include "pwr_ctrl.h"
#include "pc_power_service.h"

/* 电源状态到达 ON 后，等待本次 RK 启动协议心跳的上限，单位 ms。 */
#ifndef B1_RK_STARTUP_HEARTBEAT_TIMEOUT_MS
#define B1_RK_STARTUP_HEARTBEAT_TIMEOUT_MS 20000U
#endif
#if B1_RK_STARTUP_HEARTBEAT_TIMEOUT_MS < 1 || B1_RK_STARTUP_HEARTBEAT_TIMEOUT_MS > INT32_MAX
#error "B1_RK_STARTUP_HEARTBEAT_TIMEOUT_MS 必须在 1 到 2147483647 毫秒之间"
#endif

/* shortcut: 以下为联调初值；首板测量电源波形与训练窗口后修订，不作为验收值。 */
static const pwr_ctrl_config_t b1_power_default_timing = {
    /* 外设3.3V先建立，再开启同时供给RK806/TPS的4V，避免在4V开启后长等。 */
    .rail_order = PWR_CTRL_RAIL_ORDER_3V3_FIRST,
    .first_to_second_delay_ms = 10U,
    /* 给4V及TPS启动留初始等待；实际I2C可访问时间与训练前余量需实测。 */
    .second_to_tps_config_delay_ms = 5U,
    /* 正常下电反序：先关4V，10ms后关3.3V；这不等于DDR已经放电完毕。 */
    .system_rail_off_spacing_ms = 10U,
};

/* 电源业务等待时间均以 ms 计。启动心跳超时由发起请求的业务处理。 */
static const pc_power_config_t b1_power_default_business = {
    .transition_timeout_ms = 2000U, /* 轨间动作及TPS访问的等待上限。 */
    .ready_timeout_ms = B1_RK_STARTUP_HEARTBEAT_TIMEOUT_MS,
    .off_hold_ms = 500U,           /* 两路均OFF后停留500ms，再准备下一次启动。 */
    .boot_settle_ms = 10U,         /* Boot DAC成功写完后等待10ms再启电源。 */
};

#endif
