/*
 * ===================================================================
 * DDR 电源管理实现 — 三路 TPS62868 统一控制
 * ===================================================================
 * 实例: 每轨一个 tps62868_t, bus 指向对应 I2C 接口。
 * B1的I2C1/I2C2为硬件控制器，i2c4_if为PH4/PH5软件开漏总线。
 * 本模块不在启动时自动访问器件，只在pwr_ctrl完成前级上电后初始化。
 * ===================================================================
 */
#include "ddr_power.h"
#include <string.h>

extern i2c_if_t i2c1_if;
extern i2c_if_t i2c2_if;
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
static uint16_t _mv[DDR_RAIL_COUNT] = {
    DDR_VDD1_DEFAULT_MV, DDR_VDD2_DEFAULT_MV, DDR_VDDQ_DEFAULT_MV
};

static void _bind_buses(void)
{
    _ddr[DDR_RAIL_VDD1].bus = &i2c1_if;   /* PB7/PB8 */
    _ddr[DDR_RAIL_VDD2].bus = &i2c2_if;   /* PF0/PF1 */
    _ddr[DDR_RAIL_VDDQ].bus = &i2c4_if;   /* PH4/PH5软件开漏 */
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

static int _targets_are_writable(const uint16_t mv[DDR_RAIL_COUNT])
{
    if (!mv) return 0;

    /*
     * 此处只校验TPS是否能精确编码。测试会故意超出DDR工作区，
     * 因此不再使用JESD建议值或三路相对电压拦截。
     */
    return mv[DDR_RAIL_VDD1] >= DDR_VDD1_WRITABLE_MIN_MV &&
           mv[DDR_RAIL_VDD1] <= DDR_VDD1_WRITABLE_MAX_MV &&
           (mv[DDR_RAIL_VDD1] % 10U) == 0U &&
           mv[DDR_RAIL_VDD2] >= DDR_VDDX_WRITABLE_MIN_MV &&
           mv[DDR_RAIL_VDD2] <= DDR_VDDX_WRITABLE_MAX_MV &&
           ((mv[DDR_RAIL_VDD2] - DDR_VDDX_WRITABLE_MIN_MV) % 5U) == 0U &&
           mv[DDR_RAIL_VDDQ] >= DDR_VDDX_WRITABLE_MIN_MV &&
           mv[DDR_RAIL_VDDQ] <= DDR_VDDX_WRITABLE_MAX_MV &&
           ((mv[DDR_RAIL_VDDQ] - DDR_VDDX_WRITABLE_MIN_MV) % 5U) == 0U;
}

int ddr_power_build_step_targets(ddr_memory_type_t type,
                                 const int16_t steps[DDR_RAIL_COUNT],
                                 uint16_t mv[DDR_RAIL_COUNT])
{
    if (!steps || !mv || type > DDR_MEMORY_LPDDR4X)
        return IF_ERR;

    for (uint8_t rail = 0U; rail < DDR_RAIL_COUNT; rail++) {
        int32_t target = (int32_t)_nominal_mv[type][rail] +
                         (int32_t)steps[rail] * DDR_VOLTAGE_STEP_MV;
        if (target < 0 || target > UINT16_MAX) return IF_ERR;
        mv[rail] = (uint16_t)target;
    }
    return _targets_are_writable(mv) ? IF_OK : IF_ERR;
}

int ddr_power_select_memory_type(ddr_memory_type_t type)
{
    if (type > DDR_MEMORY_LPDDR4X) return IF_ERR;

    _memory_type = type;
    memcpy(_mv, _nominal_mv[type], sizeof(_mv));
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
    memcpy(nominal, _nominal_mv[_memory_type], sizeof(nominal));
    memcpy(_mv, nominal, sizeof(_mv));
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
    if (!_targets_are_writable(candidate)) return IF_ERR;

    /*
     * 三颗芯片无法原子改压。为尽量缩短常规拉偏时的不利过渡态：
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

int ddr_power_apply_steps(const int16_t steps[DDR_RAIL_COUNT])
{
    uint16_t target[DDR_RAIL_COUNT];
    if (ddr_power_build_step_targets(_memory_type, steps, target) != IF_OK)
        return IF_ERR;
    if (ddr_power_set(DDR_RAIL_MASK_ALL, target) != IF_OK)
        return IF_ERR;
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
