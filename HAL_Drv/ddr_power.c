/*
 * ===================================================================
 * DDR 电源管理实现 — 三路 TPS62868 统一控制
 * ===================================================================
 * 实例: 每轨一个 tps62868_t, bus 指向对应 I2C 接口。
 * V2 阶段 i2c2_if 未配置 → rail1返回 IF_ERR；隔离Mock会替换三路总线。
 * B1阶段还需将i2c4_if映射到PH4/PH5软件开漏总线后再做真板验收。
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

static const uint16_t _nominal_mv[2][DDR_RAIL_COUNT] = {
    [DDR_MEMORY_LPDDR4]  = {1800U, 1100U, 1100U},
    [DDR_MEMORY_LPDDR4X] = {1800U, 1100U,  600U},
};

static ddr_memory_type_t _memory_type = DDR_MEMORY_LPDDR4X;
static ddr_bias_profile_t _bias_profile = DDR_BIAS_NOMINAL;
static uint16_t _mv[DDR_RAIL_COUNT] = {
    DDR_VDD1_DEFAULT_MV, DDR_VDD2_DEFAULT_MV, DDR_VDDQ_DEFAULT_MV
};

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

static int _targets_are_safe(const uint16_t mv[DDR_RAIL_COUNT])
{
    if (!mv) return 0;

    /* JESD209-4E绝对最大额定值，并保留上电/掉电期间的电源相对关系。 */
    if (mv[DDR_RAIL_VDD1] > 2100U || mv[DDR_RAIL_VDD2] > 1400U ||
        mv[DDR_RAIL_VDDQ] > 1400U)
        return 0;
    if (mv[DDR_RAIL_VDD1] < mv[DDR_RAIL_VDD2]) return 0;
    if ((uint32_t)mv[DDR_RAIL_VDD2] + 200U < mv[DDR_RAIL_VDDQ]) return 0;
    return 1;
}

int ddr_power_build_profile(ddr_memory_type_t type, ddr_bias_profile_t profile,
                            uint16_t mv[DDR_RAIL_COUNT])
{
    static const uint8_t percent[] = {100U, 105U, 95U, 90U};

    if (!mv || type > DDR_MEMORY_LPDDR4X || profile > DDR_BIAS_MINUS_10_PERCENT)
        return IF_ERR;

    for (uint8_t rail = 0U; rail < DDR_RAIL_COUNT; rail++) {
        uint32_t scaled = (uint32_t)_nominal_mv[type][rail] * percent[profile];
        if ((scaled % 100U) != 0U) return IF_ERR;
        mv[rail] = (uint16_t)(scaled / 100U);
    }
    return _targets_are_safe(mv) ? IF_OK : IF_ERR;
}

int ddr_power_select_memory_type(ddr_memory_type_t type)
{
    uint16_t nominal[DDR_RAIL_COUNT];
    if (ddr_power_build_profile(type, DDR_BIAS_NOMINAL, nominal) != IF_OK)
        return IF_ERR;

    _memory_type = type;
    _bias_profile = DDR_BIAS_NOMINAL;
    memcpy(_mv, nominal, sizeof(_mv));
    return IF_OK;
}

/* 初始化: 清 STATUS 残留标志 + 写基准电压 + ramp 最快 (EN 拉高后调用) */
int ddr_power_init(void)
{
    uint8_t st;
    int r = IF_OK;
    uint16_t nominal[DDR_RAIL_COUNT];

    _bind_buses();

    /*
     * TPS在PG11拉低后寄存器复位，软件镜像也必须同步复位。更重要的是，
     * 上一次测试留下的拉偏目标绝不能带入下一次DRAM初始化窗口。
     */
    if (ddr_power_build_profile(_memory_type, DDR_BIAS_NOMINAL, nominal) != IF_OK)
        return IF_ERR;
    memcpy(_mv, nominal, sizeof(_mv));
    _bias_profile = DDR_BIAS_NOMINAL;
    for (uint8_t rail = 0U; rail < DDR_RAIL_COUNT; rail++)
        _ddr[rail].ctl_cache = 0U;

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
    uint16_t candidate[DDR_RAIL_COUNT];
    uint16_t previous[DDR_RAIL_COUNT];
    uint8_t order[DDR_RAIL_COUNT];
    uint8_t order_count = 0U;
    uint8_t applied_count = 0U;

    if (!mv || rail_mask == 0U || (rail_mask & ~DDR_RAIL_MASK_ALL) != 0U)
        return IF_ERR;
    _bind_buses();
    memcpy(previous, _mv, sizeof(previous));
    memcpy(candidate, _mv, sizeof(candidate));
    for (uint8_t rail = 0U; rail < DDR_RAIL_COUNT; rail++) {
        if (rail_mask & (1U << rail)) candidate[rail] = mv[rail];
    }
    if (!_targets_are_safe(candidate)) return IF_ERR;

    /*
     * 三颗芯片无法原子改压。为让过渡态也满足JESD电源相对关系：
     * 1) 先抬高VDD1，为VDD2升压留余量；
     * 2) 先降低VDDQ，为VDD2降压留余量；
     * 3) 再改变中间轨VDD2；
     * 4) 最后抬高VDDQ或降低VDD1。
     * 未变化的轨不重复写寄存器。
     */
    if ((rail_mask & DDR_RAIL_MASK_VDD1) &&
        candidate[DDR_RAIL_VDD1] > previous[DDR_RAIL_VDD1])
        order[order_count++] = DDR_RAIL_VDD1;
    if ((rail_mask & DDR_RAIL_MASK_VDDQ) &&
        candidate[DDR_RAIL_VDDQ] < previous[DDR_RAIL_VDDQ])
        order[order_count++] = DDR_RAIL_VDDQ;
    if ((rail_mask & DDR_RAIL_MASK_VDD2) &&
        candidate[DDR_RAIL_VDD2] != previous[DDR_RAIL_VDD2])
        order[order_count++] = DDR_RAIL_VDD2;
    if ((rail_mask & DDR_RAIL_MASK_VDDQ) &&
        candidate[DDR_RAIL_VDDQ] > previous[DDR_RAIL_VDDQ])
        order[order_count++] = DDR_RAIL_VDDQ;
    if ((rail_mask & DDR_RAIL_MASK_VDD1) &&
        candidate[DDR_RAIL_VDD1] < previous[DDR_RAIL_VDD1])
        order[order_count++] = DDR_RAIL_VDD1;

    while (applied_count < order_count) {
        uint8_t rail = order[applied_count];
        if (_set_one(rail, candidate[rail]) != IF_OK) {
            /* 逆序恢复已经修改的轨，使回退路径与前向安全顺序严格对称。 */
            while (applied_count > 0U) {
                rail = order[--applied_count];
                (void)_set_one(rail, previous[rail]);
            }
            return IF_ERR;
        }
        applied_count++;
    }
    return IF_OK;
}

int ddr_power_apply_bias(ddr_bias_profile_t profile)
{
    uint16_t target[DDR_RAIL_COUNT];
    if (ddr_power_build_profile(_memory_type, profile, target) != IF_OK)
        return IF_ERR;
    if (ddr_power_set(DDR_RAIL_MASK_ALL, target) != IF_OK)
        return IF_ERR;
    _bias_profile = profile;
    return IF_OK;
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

ddr_memory_type_t ddr_power_get_memory_type(void)
{
    return _memory_type;
}

ddr_bias_profile_t ddr_power_get_bias_profile(void)
{
    return _bias_profile;
}
