/*
 * ===================================================================
 * DDR 电源管理实现 — 三路 TPS62868 统一控制
 * ===================================================================
 * 实例: 每轨一个 tps62868_t, bus 指向对应 I2C 接口。
 * V2 阶段 i2c2_if 未配置 → rail1返回 IF_ERR；隔离Mock会替换三路总线。
 * B1已将i2c4_if映射到PH4/PH5软件开漏总线，电气及时序仍待真板验收。
 * ===================================================================
 */
#include "ddr_power.h"
#include "board_interfaces.h"
#include <string.h>

/* 三路实例：地址按预期VSET贴装定义；启动值不等于本次训练目标。 */
static tps62868_t _ddr[3] = {
    /* VDD1：TPS628692A，56.2kΩ，电压倍率2。 */ { .bus = NULL, .addr = TPS62868_ADDR_VDD1, .factor_x10 = TPS62868_FACTOR_2X0 },
    /* VDD2：TPS628681A，2134版原理图为86.6kΩ，电压倍率1。 */ { .bus = NULL, .addr = TPS62868_ADDR_VDD2, .factor_x10 = TPS62868_FACTOR_1X0 },
    /* VDDQ：TPS628681A，15.4kΩ，电压倍率1。 */ { .bus = NULL, .addr = TPS62868_ADDR_VDDQ, .factor_x10 = TPS62868_FACTOR_1X0 },
};

static const uint16_t _nominal_mv[2][DDR_RAIL_COUNT] = {
    [DDR_MEMORY_LPDDR4]  = {1800U, 1100U, 1100U},
    [DDR_MEMORY_LPDDR4X] = {1800U, 1100U,  600U},
};

/* steps按轨换算；每增加1档，恰好增加一个TPS电压寄存器码。 */
static const uint16_t _step_mv[DDR_RAIL_COUNT] = {
    DDR_VDD1_STEP_MV, DDR_VDDX_STEP_MV, DDR_VDDX_STEP_MV
};

static ddr_memory_type_t _memory_type = DDR_MEMORY_LPDDR4X;
/*
 * 训练目标与运行中的拉偏缓存分开保存。前者仅在OFF时由控制层选择，
 * 每次冷启动重新写入；后者随实时调压变化，绝不能反向覆盖训练目标。
 * 这只是请求目标，不是“RK已经在此电压下训练成功”的记录。
 */
static uint16_t _training_mv[DDR_RAIL_COUNT] = {
    DDR_VDD1_DEFAULT_MV, DDR_VDD2_DEFAULT_MV, DDR_VDDQ_DEFAULT_MV
};

uint16_t ddr_power_get_training_mv(uint8_t rail)
{
    return rail < DDR_RAIL_COUNT ? _training_mv[rail] : 0U;
}
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

static int _targets_are_writable(const uint16_t mv[DDR_RAIL_COUNT])
{
    if (!mv) return 0;

    /*
     * 此处只校验TPS是否能精确编码。测试会故意超出DDR工作区，
     * 因此不再使用JESD建议值或三路相对电压拦截。
     */
    return mv[DDR_RAIL_VDD1] >= DDR_VDD1_WRITABLE_MIN_MV &&
           mv[DDR_RAIL_VDD1] <= DDR_VDD1_WRITABLE_MAX_MV &&
           (mv[DDR_RAIL_VDD1] % DDR_VDD1_STEP_MV) == 0U &&
           mv[DDR_RAIL_VDD2] >= DDR_VDDX_WRITABLE_MIN_MV &&
           mv[DDR_RAIL_VDD2] <= DDR_VDDX_WRITABLE_MAX_MV &&
           ((mv[DDR_RAIL_VDD2] - DDR_VDDX_WRITABLE_MIN_MV) % DDR_VDDX_STEP_MV) == 0U &&
           mv[DDR_RAIL_VDDQ] >= DDR_VDDX_WRITABLE_MIN_MV &&
           mv[DDR_RAIL_VDDQ] <= DDR_VDDX_WRITABLE_MAX_MV &&
           ((mv[DDR_RAIL_VDDQ] - DDR_VDDX_WRITABLE_MIN_MV) % DDR_VDDX_STEP_MV) == 0U;
}

int ddr_power_build_step_targets(ddr_memory_type_t type,
                                 const int16_t steps[DDR_RAIL_COUNT],
                                 uint16_t mv[DDR_RAIL_COUNT])
{
    if (!steps || !mv || (uint32_t)type > DDR_MEMORY_LPDDR4X)
        return IF_ERR;

    for (uint8_t rail = 0U; rail < DDR_RAIL_COUNT; rail++) {
        int32_t target = (int32_t)_nominal_mv[type][rail] +
                         (int32_t)steps[rail] * _step_mv[rail];
        if (target < 0 || target > UINT16_MAX) return IF_ERR;
        mv[rail] = (uint16_t)target;
    }
    return _targets_are_writable(mv) ? IF_OK : IF_ERR;
}

int ddr_power_select_memory_type(ddr_memory_type_t type)
{
    if ((uint32_t)type > DDR_MEMORY_LPDDR4X) return IF_ERR;

    /* 兼容旧的“只选择类型”入口：显式撤销自定义训练值，恢复该类型标称值。 */
    (void)ddr_power_configure_training(type, _nominal_mv[type]);
    memcpy(_mv, _nominal_mv[type], sizeof(_mv));
    return IF_OK;
}

int ddr_power_configure_training(ddr_memory_type_t type,
                                 const uint16_t mv[DDR_RAIL_COUNT])
{
    if ((uint32_t)type > DDR_MEMORY_LPDDR4X || !_targets_are_writable(mv)) {
        return IF_ERR;
    }

    /* 全部参数合法后一次性复制，调用后不再借用上层缓冲；此处不访问I2C。 */
    memcpy(_training_mv, mv, sizeof(_training_mv));
    _memory_type = type;
    return IF_OK;
}

/* TPS可访问后直接写本次训练值，不先写标称值、再做一次运行期拉偏。 */
int ddr_power_init(void)
{
    uint8_t st;

    _bind_buses();

    /*
     * TPS掉电/EN关闭会复位寄存器，CONTROL软件镜像也必须重新建立。
     * B1按当前NC贴装由PC6切断4V，不能假定PG11一定能关闭TPS。
     * 先检查全部回调，避免明知缺一条总线还先修改其它轨。
     */
    for (uint8_t rail = 0U; rail < DDR_RAIL_COUNT; rail++) {
        if (!_ddr[rail].bus->read_reg || !_ddr[rail].bus->write_reg) {
            return IF_ERR;
        }
        _ddr[rail].ctl_cache = 0U;
    }

    for (uint8_t rail = 0U; rail < DDR_RAIL_COUNT; rail++) {
        /*
         * 保持VDD1/VDD2/VDDQ的事务顺序，每轨清STATUS、设置ramp、写目标。
         * 短路求值：首次总线错误立即返回，由pwr_ctrl关断；不在此处重试，
         * 也不写其它轨来“凑齐配置”。同步总线超时仍须纳入真板时间预算。
         */
        if (tps62868_read_status(&_ddr[rail], &st) != IF_OK ||
            tps62868_set_ramp(&_ddr[rail], TPS62868_RAMP_20MV_US) != IF_OK ||
            tps62868_set_vout1(&_ddr[rail], _training_mv[rail]) != IF_OK) {
            return IF_ERR;
        }
    }

    /* 全部写入成功才更新目标缓存；仍不证明模拟电压稳定或RK训练已完成。 */
    memcpy(_mv, _training_mv, sizeof(_mv));
    return IF_OK;
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
