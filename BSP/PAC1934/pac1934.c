/*
 * ===================================================================
 * PAC1934T 驱动实现 (Microchip DS20005850)
 * ===================================================================
 * I2C 8-bit 寄存器寻址 (HAL_I2C_Mem_Read/Write), 7-bit 设备地址。
 * 换算均为 unipolar 模式 (默认), 数据为大端 (MSB first)。
 * ===================================================================
 */
#include "pac1934.h"

/* ── 寄存器读/写 ── */
static int _read(pac1934_t *d, uint8_t reg, uint8_t *buf, uint16_t len)
{
    if (!d || !d->bus || !d->bus->read_bytes) return IF_ERR;
    return d->bus->read_bytes(d->bus, d->addr, reg, buf, len);
}

static int _write(pac1934_t *d, uint8_t reg, uint8_t val)
{
    if (!d || !d->bus || !d->bus->write_reg) return IF_ERR;
    return d->bus->write_reg(d->bus, d->addr, reg, val);
}

/* ── Send Byte 命令 (REFRESH 系列: 只发命令字节, 无数据, 不触发指针递增写) ── */
static int _cmd(pac1934_t *d, uint8_t cmd)
{
    if (!d || !d->bus || !d->bus->write_cmd) return IF_ERR;
    return d->bus->write_cmd(d->bus, d->addr, cmd);
}

/* ── 初始化 ── */
int pac1934_init(pac1934_t *d, uint8_t ch_mask, uint8_t sample_rate)
{
    uint8_t product_id = 0;
    uint8_t manufacturer_id = 0;

    if (!d || ch_mask == 0U || (ch_mask & ~PAC1934_CH_MASK_ALL) != 0U ||
        sample_rate > 3U)
        return IF_ERR;
    if (d->rsense_uohm == 0) d->rsense_uohm = 10000;  /* 默认 0.01Ω */

    /* 同时校验产品和厂商ID，避免总线浮空值被误判成器件在线。 */
    if (_read(d, PAC1934_REG_PRODUCT_ID, &product_id, 1) != IF_OK ||
        _read(d, PAC1934_REG_MANUF_ID, &manufacturer_id, 1) != IF_OK)
        return IF_ERR;
    if (product_id != PAC1934_PRODUCT_ID || manufacturer_id != PAC1934_MANUF_ID)
        return IF_ERR;

    /* 0x1C的CH1_OFF..CH4_OFF位于bit7..4；bit3..1属于SMBus控制。 */
    uint8_t dis = PAC1934_CH_DISABLE_BITS(ch_mask);
    if (_write(d, PAC1934_REG_CHANNEL_DIS, dis) != IF_OK)
        return IF_ERR;

    /* 采样率 (01h) + 连续扫描模式 (SLEEP=0, SING=0) */
    uint8_t ctrl = PAC1934_CTRL_SAMPLE_RATE(sample_rate);
    if (_write(d, PAC1934_REG_CTRL, ctrl) != IF_OK)
        return IF_ERR;

    /* 启动转换 (00h REFRESH 命令, Send Byte: 复位累加器并开始连续采样) */
    if (_cmd(d, PAC1934_REG_REFRESH) != IF_OK)
        return IF_ERR;

    d->active_mask = ch_mask;
    return IF_OK;
}

/* ── 读 2 字节大端 → uint16 ── */
static uint16_t _u16(const uint8_t *b)
{ return (uint16_t)((b[0] << 8) | b[1]); }

/* ── 读 4 字节大端 → uint32 ── */
static uint32_t _u32(const uint8_t *b)
{ return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
        ((uint32_t)b[2] << 8) | b[3]; }

/* ── 读单通道 (ch=0..3) ── */
int pac1934_read_ch(pac1934_t *d, uint8_t ch, pac1934_chan_t *out)
{
    uint8_t buf[4];

    if (!d || !out || ch > 3 || !(d->active_mask & (1U << ch))) return IF_ERR;

    /* VBUS 结果 (07h+ch*1, 2B) */
    if (_read(d, PAC1934_REG_VBUS1 + ch, buf, 2) != IF_OK) return IF_ERR;
    out->vbus = _u16(buf);

    /* VSENSE 结果 (0Bh+ch*1, 2B) */
    if (_read(d, PAC1934_REG_VSENSE1 + ch, buf, 2) != IF_OK) return IF_ERR;
    out->vsense = _u16(buf);

    /* VPOWER 结果 (17h+ch*1, 4B) */
    if (_read(d, PAC1934_REG_VPOWER1 + ch, buf, 4) != IF_OK) return IF_ERR;
    out->vpower = _u32(buf);

    return IF_OK;
}

/* ── 刷新并读取全部 4 通道 (REFRESH 后需 ≥1ms 再读, 由调用方延时) ── */
int pac1934_read_all(pac1934_t *d, pac1934_chan_t *ch)
{
    if (!d || !ch || !d->wait_ms || d->active_mask == 0U) return IF_ERR;

    /* 刷新 (1Fh REFRESH_V 命令, Send Byte: 锁存最新转换结果, 不复位累加器) */
    if (_cmd(d, PAC1934_REG_REFRESH_V) != IF_OK)
        return IF_ERR;

    /* Microchip要求所有结果寄存器在REFRESH_V后等待1ms才稳定。 */
    d->wait_ms(1U);

    /*
     * 禁用通道时PAC1934默认会让自动递增指针跳过对应寄存器。逐通道读取
     * 可保持通道号与结果一一对应，也避免把CH4后的寄存器误当成CH4数据。
     */
    for (int i = 0; i < 4; i++) {
        ch[i].vbus = 0U;
        ch[i].vsense = 0U;
        ch[i].vpower = 0U;
        if (!(d->active_mask & (1U << i))) continue;
        if (pac1934_read_ch(d, (uint8_t)i, &ch[i]) != IF_OK) return IF_ERR;
    }

    return IF_OK;
}

int pac1934_measure_all(pac1934_t *d, pac1934_measurement_t *out)
{
    pac1934_chan_t raw[4];
    if (!d || !out || pac1934_read_all(d, raw) != IF_OK) return IF_ERR;

    for (uint8_t ch = 0U; ch < 4U; ch++) {
        out[ch].vbus_mv = 0U;
        out[ch].current_ma = 0U;
        out[ch].power_mw = 0U;
        if (!(d->active_mask & (1U << ch))) continue;
        out[ch].vbus_mv = pac1934_vbus_mv(raw[ch].vbus);
        out[ch].current_ma = pac1934_current_ma(raw[ch].vsense,
                                                d->rsense_uohm);
        out[ch].power_mw = pac1934_power_mw(raw[ch].vpower,
                                            d->rsense_uohm);
    }
    return IF_OK;
}

/* ── 换算 (unipolar) ── */
uint32_t pac1934_vbus_mv(uint16_t vbus_reg)
{
    /* Vbus = VBUS_reg × 32V / 2^16  → mV = VBUS_reg × 32000 / 65536 */
    return (uint32_t)vbus_reg * 32000u / 65536u;
}

uint32_t pac1934_current_ma(uint16_t vsense_reg, uint32_t rsense_uohm)
{
    /* I = FSC × VSENSE_reg / 2^16,  FSC = 100mV/Rsense
     * I_mA = (0.1V/rsense) × VSENSE_reg/65536 × 1000
     *      = 1e8 / rsense_uohm × VSENSE_reg / 65536
     * 0.01Ω(10000µΩ): 满量程 10A=10000mA @ VSENSE_reg=65536 ✓ */
    if (rsense_uohm == 0U) return 0U;
    uint64_t v = (uint64_t)vsense_reg * 100000000u / rsense_uohm;
    return (uint32_t)(v / 65536u);
}

uint32_t pac1934_power_mw(uint32_t vpower_reg, uint32_t rsense_uohm)
{
    /* P = PowerFSR × VPOWER_reg / 2^28
     * PowerFSR = (0.1V/Rsense) × 32V = 3.2/rsense_ohm W
     * P_mW = 3.2e9/rsense_uohm × VPOWER_reg / 2^28
     * 0.01Ω(10000µΩ): 满量程 320W=320000mW @ VPOWER_reg=2^28 ✓ */
    if (rsense_uohm == 0U) return 0U;
    uint64_t v = (uint64_t)vpower_reg * 3200000000u / rsense_uohm;
    return (uint32_t)(v / 268435456u);   /* 2^28 */
}

