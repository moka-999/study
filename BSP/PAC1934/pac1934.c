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
    uint8_t id = 0;

    if (!d) return IF_ERR;
    if (d->rsense_uohm == 0) d->rsense_uohm = 10000;  /* 默认 0.01Ω */

    /* 读产品 ID 校验 (应 0x5B = PAC1934) */
    if (_read(d, PAC1934_REG_PRODUCT_ID, &id, 1) != IF_OK)
        return IF_ERR;
    if (id != PAC1934_PRODUCT_ID) return IF_ERR;

    /* 禁用未用通道 (1Ch): bit0-3 = ch1-4 禁用位 */
    uint8_t dis = (uint8_t)(~ch_mask) & PAC1934_CH_MASK_ALL;
    if (_write(d, PAC1934_REG_CHANNEL_DIS, dis) != IF_OK)
        return IF_ERR;

    /* 采样率 (01h) + 连续扫描模式 (SLEEP=0, SING=0) */
    uint8_t ctrl = PAC1934_CTRL_SAMPLE_RATE(sample_rate);
    if (_write(d, PAC1934_REG_CTRL, ctrl) != IF_OK)
        return IF_ERR;

    /* 启动转换 (00h REFRESH 命令, Send Byte: 复位累加器并开始连续采样) */
    if (_cmd(d, PAC1934_REG_REFRESH) != IF_OK)
        return IF_ERR;

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

    if (!d || !out || ch > 3) return IF_ERR;

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
    uint8_t buf[8];

    if (!d || !ch) return IF_ERR;

    /* 刷新 (1Fh REFRESH_V 命令, Send Byte: 锁存最新转换结果, 不复位累加器) */
    if (_cmd(d, PAC1934_REG_REFRESH_V) != IF_OK)
        return IF_ERR;

    /* VBUS 1-4 (07h-0Ah, 连续块读 8 字节) */
    if (_read(d, PAC1934_REG_VBUS1, buf, 8) != IF_OK) return IF_ERR;
    for (int i = 0; i < 4; i++) ch[i].vbus = _u16(buf + i * 2);

    /* VSENSE 1-4 (0Bh-0Eh, 连续块读 8 字节) */
    if (_read(d, PAC1934_REG_VSENSE1, buf, 8) != IF_OK) return IF_ERR;
    for (int i = 0; i < 4; i++) ch[i].vsense = _u16(buf + i * 2);

    /* VPOWER 1-4 (17h-1Ah, 连续块读 16 字节) */
    for (int i = 0; i < 4; i++) {
        if (_read(d, PAC1934_REG_VPOWER1 + i, buf, 4) != IF_OK) return IF_ERR;
        ch[i].vpower = _u32(buf);
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
    uint64_t v = (uint64_t)vsense_reg * 100000000u / rsense_uohm;
    return (uint32_t)(v / 65536u);
}

uint32_t pac1934_power_mw(uint32_t vpower_reg, uint32_t rsense_uohm)
{
    /* P = PowerFSR × VPOWER_reg / 2^28
     * PowerFSR = (0.1V/Rsense) × 32V = 3.2/rsense_ohm W
     * P_mW = 3.2e9/rsense_uohm × VPOWER_reg / 2^28
     * 0.01Ω(10000µΩ): 满量程 320W=320000mW @ VPOWER_reg=2^28 ✓ */
    uint64_t v = (uint64_t)vpower_reg * 3200000000u / rsense_uohm;
    return (uint32_t)(v / 268435456u);   /* 2^28 */
}