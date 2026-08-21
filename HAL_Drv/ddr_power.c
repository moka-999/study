/*
 * ===================================================================
 * DDR 电源管理实现 — 三路 TPS62868 统一控制
 * ===================================================================
 * 实例: 每轨一个 tps62868_t, bus 指向对应 I2C 接口。
 * V2 阶段 i2c2_if 未配置 → rail1返回 IF_ERR; B1补 I2C2 外设后全通。
 * ===================================================================
 */
#include "ddr_power.h"
#include <string.h>

extern i2c_if_t i2c1_if;
extern i2c_if_t i2c2_if;   /* B1 补 I2C2 外设 + i2c2_if (PF0/1), V2 未配置 */
extern i2c_if_t i2c4_if;

/* 三路实例 (A 版地址由 VSET/PG 电阻决定，启动电压 ≤ 目标电压) */
static tps62868_t _ddr[3] = {
    /* VDD1: TPS628692A, 56.2k, factor 2 */ { .bus = NULL, .addr = TPS62868_ADDR_VDD1, .factor_x10 = TPS62868_FACTOR_2X0 },
    /* VDD2: TPS628681A, 86.8k (TI 86.6k slot), factor 1 */ { .bus = NULL, .addr = TPS62868_ADDR_VDD2, .factor_x10 = TPS62868_FACTOR_1X0 },
    /* VDDQ: TPS628681A, 15.4k, factor 1 */ { .bus = NULL, .addr = TPS62868_ADDR_VDDQ, .factor_x10 = TPS62868_FACTOR_1X0 },
};

static uint16_t _mv[3] = { DDR_VDD1_DEFAULT_MV, DDR_VDD2_DEFAULT_MV, DDR_VDDQ_DEFAULT_MV };

static void _bind_buses(void)
{
    _ddr[DDR_RAIL_VDD1].bus = &i2c1_if;   /* V2 PB8/9; B1 PB7/8 */
    _ddr[DDR_RAIL_VDD2].bus = &i2c2_if;   /* B1 PF0/1 */
    _ddr[DDR_RAIL_VDDQ].bus = &i2c4_if;   /* V2 PF14/15; B1 PH4/5 */
}

/* 单轨设置电压 (mV→code 换算 + 边界检查) */
static int _set_one(uint8_t rail, uint16_t mv)
{
    if (rail >= DDR_RAIL_COUNT) return IF_ERR;
    if (!_ddr[rail].bus) return IF_ERR;            /* 总线未接入 (如 V2 的 VDD2) */
    if (tps62868_set_vout1(&_ddr[rail], mv) != IF_OK)
        return IF_ERR;
    _mv[rail] = mv;
    return IF_OK;
}

/* 初始化: 清 STATUS 残留标志 + 写基准电压 + ramp 最快 (EN 拉高后调用) */
int ddr_power_init(void)
{
    uint8_t st;
    int r = IF_OK;

    _bind_buses();

    for (int rail = 0; rail < DDR_RAIL_COUNT; rail++) {
        if (!_ddr[rail].bus) { r = IF_ERR; continue; }
        /* 读 STATUS 清残留标志 (读后自动清零) */
        if (tps62868_read_status(&_ddr[rail], &st) != IF_OK)
            r = IF_ERR;
        /* ramp 设最快 20mV/µs, 缩短调压/切换窗口 */
        if (tps62868_set_ramp(&_ddr[rail], TPS62868_RAMP_20MV_US) != IF_OK)
            r = IF_ERR;
        /* 写基准电压 */
        if (tps62868_set_vout1(&_ddr[rail], _mv[rail]) != IF_OK)
            r = IF_ERR;
    }
    return r;
}

/* 动态调压: mask 选轨, mv[3] 各轨目标电压 */
int ddr_power_set(uint8_t rail_mask, const uint16_t *mv)
{
    int r = IF_OK;

    _bind_buses();
    for (int rail = 0; rail < DDR_RAIL_COUNT; rail++) {
        if (!(rail_mask & (1u << rail))) continue;
        if (_set_one(rail, mv ? mv[rail] : _mv[rail]) != IF_OK)
            r = IF_ERR;
    }
    return r;
}

/* 读某轨 STATUS */
int ddr_power_read_status(uint8_t rail, uint8_t *flags)
{
    if (rail >= DDR_RAIL_COUNT || !flags) return IF_ERR;
    _bind_buses();
    if (!_ddr[rail].bus) return IF_ERR;
    return tps62868_read_status(&_ddr[rail], flags);
}

/* 查询某轨当前目标电压 */
uint16_t ddr_power_get_mv(uint8_t rail)
{
    if (rail >= DDR_RAIL_COUNT) return 0;
    return _mv[rail];
}
