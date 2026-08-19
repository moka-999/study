/*
 * ===================================================================
 * TPS62868/TPS62869 驱动实现 (TI SLVSFS3B)
 * ===================================================================
 * I2C 8-bit 寄存器寻址 (HAL_I2C_Mem_Read/Write), 7-bit 设备地址。
 * VOUT 换算: code = (mv*10/factor_x10 - 400)/5, 边界 0x00..0xFF
 * ===================================================================
 */
#include "tps62868.h"

/* ── 寄存器读/写 ── */
int tps62868_read_reg(tps62868_t *d, uint8_t reg, uint8_t *val)
{
    if (!d || !d->bus || !d->bus->read_reg) return IF_ERR;
    return d->bus->read_reg(d->bus, d->addr, reg, val);
}

int tps62868_write_reg(tps62868_t *d, uint8_t reg, uint8_t val)
{
    if (!d || !d->bus || !d->bus->write_reg) return IF_ERR;
    return d->bus->write_reg(d->bus, d->addr, reg, val);
}

/* ── 地址扫描 (C 版固定 0x42, 但保留通用扫描) ── */
int tps62868_scan(i2c_if_t *bus, uint8_t *addrs, uint8_t max)
{
    uint8_t found = 0;
    if (!bus || !bus->is_ready) return 0;
    for (uint8_t a = 0x40; a <= 0x4F && found < max; a++) {
        if (bus->is_ready(bus, a) == IF_OK)
            addrs[found++] = a;
    }
    return found;
}

/* CONTROL 默认值 (Table 8-8): bit6=1 FPWM-during-change, bit5=1 sw-enable,
 * bit3=1 discharge, bit2=1 HICCUP, bit1:0=11 ramp 1mV/us → 0x6F */
#define _CTL_DEFAULT   (TPS62868_CTL_FPWM_DURING_CHANGE | TPS62868_CTL_SW_ENABLE | \
                        TPS62868_CTL_OUT_DISCHARGE | TPS62868_CTL_HICCUP | \
                        TPS62868_CTL_RAMP_SPEED(TPS62868_RAMP_1MV_US))

/* ── VOUT 设置: 按因子换算并写寄存器 ── */
static int _set_vout(tps62868_t *d, uint8_t reg, uint16_t mv)
{
    int32_t code;

    if (!d || d->factor_x10 == 0) return IF_ERR;
    if (mv < (uint16_t)(40 * d->factor_x10)) return IF_ERR;  /* 低于因子最小值 */

    code = ((int32_t)mv * 10 / d->factor_x10 - 400) / 5;
    if (code < 0 || code > 0xFF) return IF_ERR;       /* 超出可设范围 */
    return tps62868_write_reg(d, reg, (uint8_t)code);
}

int tps62868_set_vout1(tps62868_t *d, uint16_t mv)
{ return _set_vout(d, TPS62868_REG_VOUT1, mv); }

int tps62868_set_vout2(tps62868_t *d, uint16_t mv)
{ return _set_vout(d, TPS62868_REG_VOUT2, mv); }

/* ── CONTROL 修改 (write-only → 软件镜像 ctl_cache) ── */
static int _ctl_rmw(tps62868_t *d, uint8_t set_mask, uint8_t clr_mask)
{
    uint8_t ctl;

    if (!d) return IF_ERR;
    if (d->ctl_cache == 0) d->ctl_cache = _CTL_DEFAULT;  /* 首次使用默认 */
    ctl = (d->ctl_cache & ~clr_mask) | set_mask;
    if (tps62868_write_reg(d, TPS62868_REG_CONTROL, ctl) != IF_OK)
        return IF_ERR;
    d->ctl_cache = ctl;
    return IF_OK;
}

int tps62868_enable(tps62868_t *d)
{ return _ctl_rmw(d, TPS62868_CTL_SW_ENABLE, 0); }

int tps62868_disable(tps62868_t *d)
{ return _ctl_rmw(d, 0, TPS62868_CTL_SW_ENABLE); }

/* ── 输出放电 / 斜坡速率 ── */
int tps62868_set_discharge(tps62868_t *d, uint8_t on)
{
    if (on) return _ctl_rmw(d, TPS62868_CTL_OUT_DISCHARGE, 0);
    return _ctl_rmw(d, 0, TPS62868_CTL_OUT_DISCHARGE);
}

int tps62868_set_ramp(tps62868_t *d, uint8_t ramp)
{
    if (ramp > 3) return IF_ERR;
    return _ctl_rmw(d, TPS62868_CTL_RAMP_SPEED(ramp),
                    TPS62868_CTL_RAMP_SPEED(0x3));
}

/* ── STATUS 读 (读后自动清零) ── */
int tps62868_read_status(tps62868_t *d, uint8_t *flags)
{
    return tps62868_read_reg(d, TPS62868_REG_STATUS, flags);
}

/* ── 复位所有寄存器 (CONTROL bit7 自清零) ── */
int tps62868_reset_regs(tps62868_t *d)
{
    int r = tps62868_write_reg(d, TPS62868_REG_CONTROL, TPS62868_CTL_RESET);
    if (r == IF_OK) d->ctl_cache = _CTL_DEFAULT;
    return r;
}